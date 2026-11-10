// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Flipper FZCO
/*
 * Flipper One MCU firmware update driver
 */

#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/crc-itu-t.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>

#include <linux/mfd/flipper-one-mcu.h>

/*
 * Updater states, errors and commands are part of the wire protocol shared
 * with the MCU firmware; keep them in sync with its updater service.
 */
enum fo_fwupd_state {
	FO_FWUPD_STATE_IDLE = 0,
	FO_FWUPD_STATE_RUNNING,
	FO_FWUPD_STATE_BUSY,
	FO_FWUPD_STATE_DONE,
	FO_FWUPD_STATE_ERROR,
};

enum fo_fwupd_error {
	FO_FWUPD_ERR_NONE = 0,
	FO_FWUPD_ERR_NOT_STARTED,
	FO_FWUPD_ERR_SIZE,
	FO_FWUPD_ERR_BLOCK_CRC,
	FO_FWUPD_ERR_BLOCK_INDEX,
	FO_FWUPD_ERR_FW_CRC,
	FO_FWUPD_ERR_FW_VERSION,
	FO_FWUPD_ERR_PROTOCOL,
	FO_FWUPD_ERR_FW_WRITE,
	FO_FWUPD_ERR_FW_INVALIDATE,
	FO_FWUPD_ERR_FW_PARAMS,
	FO_FWUPD_ERR_NUM
};

#define FO_FWUPD_CMD_START		1
#define FO_FWUPD_CMD_ABORT		2

/* Running firmware version, as packed by the MCU build */
#define FO_FWUPD_VERSION_MAJOR		GENMASK(15, 12)
#define FO_FWUPD_VERSION_MINOR		GENMASK(11, 4)
#define FO_FWUPD_VERSION_PATCH		GENMASK(3, 0)

#define FO_FWUPD_START_ACK_MS		1000
#define FO_FWUPD_ERASE_MS		120000
#define FO_FWUPD_BLOCK_MS		5000
#define FO_FWUPD_FINALIZE_MS		5000
#define FO_FWUPD_ABORT_MS		5000

#define FO_FWUPD_ERASE_POLL_US		20000
#define FO_FWUPD_POLL_US		1000

/* Flash address of the image start, as seen from inside its partition */
#define FO_FWUPD_FLASH_BASE		0x10000000

/* The RP2350 boot ROM looks for the first picobin block in the first 4 KiB */
#define PICOBIN_BLOCK_MARKER_START	0xffffded3
#define PICOBIN_SEARCH_LEN		SZ_4K

/* UF2 container, as produced by picotool */
#define UF2_MAGIC_START0		0x0a324655
#define UF2_MAGIC_START1		0x9e5d5157
#define UF2_MAGIC_END			0x0ab16f30
#define UF2_FLAG_NOT_MAIN_FLASH		BIT(0)
#define UF2_FLAG_FAMILY_ID		BIT(13)
#define UF2_FLAG_EXTENSION_TAGS		BIT(15)
#define UF2_FAMILY_RP2350_ARM_S		0xe48bff59
/* Size byte and 24-bit type of the tag marking a block to be skipped */
#define UF2_TAG_RP2_IGNORE_BLOCK	0x9957e304
#define UF2_DATA_LEN			476

struct uf2_block {
	__le32 magic_start0;
	__le32 magic_start1;
	__le32 flags;
	__le32 target_addr;
	__le32 payload_size;
	__le32 block_no;
	__le32 num_blocks;
	__le32 family_id;
	u8 data[UF2_DATA_LEN];
	__le32 magic_end;
} __packed;
static_assert(sizeof(struct uf2_block) == 512);

struct fo_fwupd {
	struct device *dev;
	struct regmap *regmap;
	struct fw_upload *fwl;
	/* MCU firmware build target matching this board, if known */
	const char *target;

	/* Serialises starting an update against system suspend */
	struct mutex lock;
	bool busy;
	bool suspended;

	bool uf2;
	u16 blocks;
	u16 next_block;
	bool cancel_req;
	bool done;
	u8 frame[FOMCU_FWUPD_FRAME_LEN];
};

static const char * const fo_fwupd_err_names[FO_FWUPD_ERR_NUM] = {
	[FO_FWUPD_ERR_NONE]		= "unspecified",
	[FO_FWUPD_ERR_NOT_STARTED]	= "update unavailable",
	[FO_FWUPD_ERR_SIZE]		= "image size",
	[FO_FWUPD_ERR_BLOCK_CRC]	= "block CRC",
	[FO_FWUPD_ERR_BLOCK_INDEX]	= "block index",
	[FO_FWUPD_ERR_FW_CRC]		= "image CRC",
	[FO_FWUPD_ERR_FW_VERSION]	= "image version",
	[FO_FWUPD_ERR_PROTOCOL]		= "protocol",
	[FO_FWUPD_ERR_FW_WRITE]		= "flash write",
	[FO_FWUPD_ERR_FW_INVALIDATE]	= "partition invalidation",
	[FO_FWUPD_ERR_FW_PARAMS]	= "missing parameters",
};

static const enum fw_upload_err fo_fwupd_err_map[FO_FWUPD_ERR_NUM] = {
	[FO_FWUPD_ERR_NONE]		= FW_UPLOAD_ERR_HW_ERROR,
	[FO_FWUPD_ERR_NOT_STARTED]	= FW_UPLOAD_ERR_HW_ERROR,
	[FO_FWUPD_ERR_SIZE]		= FW_UPLOAD_ERR_INVALID_SIZE,
	[FO_FWUPD_ERR_BLOCK_CRC]	= FW_UPLOAD_ERR_RW_ERROR,
	[FO_FWUPD_ERR_BLOCK_INDEX]	= FW_UPLOAD_ERR_RW_ERROR,
	[FO_FWUPD_ERR_FW_CRC]		= FW_UPLOAD_ERR_FW_INVALID,
	[FO_FWUPD_ERR_FW_VERSION]	= FW_UPLOAD_ERR_FW_INVALID,
	[FO_FWUPD_ERR_PROTOCOL]		= FW_UPLOAD_ERR_RW_ERROR,
	[FO_FWUPD_ERR_FW_WRITE]		= FW_UPLOAD_ERR_HW_ERROR,
	[FO_FWUPD_ERR_FW_INVALIDATE]	= FW_UPLOAD_ERR_HW_ERROR,
	[FO_FWUPD_ERR_FW_PARAMS]	= FW_UPLOAD_ERR_HW_ERROR,
};

static enum fw_upload_err fo_fwupd_mcu_error(struct fo_fwupd *fw,
					     unsigned int error)
{
	if (error >= FO_FWUPD_ERR_NUM) {
		dev_err(fw->dev, "MCU reported unknown update error %u\n",
			error);
		return FW_UPLOAD_ERR_HW_ERROR;
	}

	dev_err(fw->dev, "MCU reported %s error\n", fo_fwupd_err_names[error]);
	return fo_fwupd_err_map[error];
}

/*
 * Poll until the MCU reaches one of @states. A latched error, a cancel request
 * and the timeout all end the wait early. @what names the wait in the timeout
 * message; NULL leaves reporting the timeout to the caller.
 */
static enum fw_upload_err fo_fwupd_wait(struct fo_fwupd *fw,
					unsigned long states,
					unsigned int timeout_ms,
					unsigned int poll_us, const char *what)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	unsigned int val, state;
	int ret;

	for (;;) {
		if (READ_ONCE(fw->cancel_req))
			return FW_UPLOAD_ERR_CANCELED;

		ret = regmap_read(fw->regmap, FOMCU_REG_FWUPD_STATUS, &val);
		if (ret) {
			dev_err(fw->dev, "Failed to read update status: %d\n",
				ret);
			return FW_UPLOAD_ERR_RW_ERROR;
		}

		state = FIELD_GET(FOMCU_FWUPD_STATUS_STATE, val);
		if (state == FO_FWUPD_STATE_ERROR)
			return fo_fwupd_mcu_error(fw,
					FIELD_GET(FOMCU_FWUPD_STATUS_ERROR, val));
		if (state > FO_FWUPD_STATE_ERROR) {
			dev_err(fw->dev, "MCU reported unknown update state %u\n",
				state);
			return FW_UPLOAD_ERR_HW_ERROR;
		}
		if (states & BIT(state))
			return FW_UPLOAD_ERR_NONE;

		if (time_after(jiffies, deadline)) {
			if (what)
				dev_err(fw->dev,
					"Timed out waiting for %s (state %u)\n",
					what, state);
			return FW_UPLOAD_ERR_TIMEOUT;
		}

		fsleep(poll_us);
	}
}

/* Return the MCU to idle, also clearing a latched error */
static int fo_fwupd_abort(struct fo_fwupd *fw)
{
	unsigned int val;
	int ret;

	ret = regmap_write(fw->regmap, FOMCU_REG_FWUPD_CMD, FO_FWUPD_CMD_ABORT);
	if (ret)
		goto err;

	ret = regmap_read_poll_timeout(fw->regmap, FOMCU_REG_FWUPD_STATUS, val,
				       FIELD_GET(FOMCU_FWUPD_STATUS_STATE, val) ==
						FO_FWUPD_STATE_IDLE,
				       FO_FWUPD_POLL_US,
				       FO_FWUPD_ABORT_MS * USEC_PER_MSEC);
	if (!ret)
		return 0;

err:
	dev_err(fw->dev, "Failed to abort the update: %d\n", ret);
	return ret;
}

/* Let suspend through again and forget any cancel of the finished update */
static void fo_fwupd_finish(struct fo_fwupd *fw)
{
	guard(mutex)(&fw->lock);

	WRITE_ONCE(fw->cancel_req, false);
	fw->busy = false;
}

static bool fo_uf2_is_block(const struct uf2_block *blk)
{
	return le32_to_cpu(blk->magic_start0) == UF2_MAGIC_START0 &&
	       le32_to_cpu(blk->magic_start1) == UF2_MAGIC_START1 &&
	       le32_to_cpu(blk->magic_end) == UF2_MAGIC_END;
}

static bool fo_uf2_is_skipped(const struct uf2_block *blk)
{
	u32 payload_size = le32_to_cpu(blk->payload_size);
	u32 flags = le32_to_cpu(blk->flags);
	u32 off, tag;

	if (flags & UF2_FLAG_NOT_MAIN_FLASH)
		return true;
	if (!(flags & UF2_FLAG_EXTENSION_TAGS) || payload_size > UF2_DATA_LEN)
		return false;

	/* Each tag is a size byte and a 24-bit type, padded to a word */
	off = ALIGN(payload_size, 4);
	while (off + 4 <= UF2_DATA_LEN) {
		tag = get_unaligned_le32(&blk->data[off]);
		if (!(tag & 0xff))
			break;
		if (tag == UF2_TAG_RP2_IGNORE_BLOCK)
			return true;
		off += ALIGN(tag & 0xff, 4);
	}

	return false;
}

/*
 * Find image block @index at the start of @data. Report its payload, how long
 * it is, and how many bytes of @data it spans together with any UF2 blocks
 * skipped on the way. A NULL payload means @data holds no further blocks.
 */
static enum fw_upload_err fo_fwupd_next_block(struct fo_fwupd *fw,
					      const u8 *data, u32 size,
					      u32 index, const u8 **payload,
					      u32 *len, u32 *consumed)
{
	const struct uf2_block *blk;
	u32 off;

	if (!fw->uf2) {
		*payload = data;
		*len = min(size, FOMCU_FWUPD_BLOCK_SIZE);
		*consumed = *len;
		return FW_UPLOAD_ERR_NONE;
	}

	for (off = 0; off < size; off += sizeof(*blk)) {
		blk = (const struct uf2_block *)(data + off);
		if (!fo_uf2_is_block(blk)) {
			dev_err(fw->dev, "Malformed UF2 block\n");
			return FW_UPLOAD_ERR_FW_INVALID;
		}

		if (fo_uf2_is_skipped(blk))
			continue;

		if (!(le32_to_cpu(blk->flags) & UF2_FLAG_FAMILY_ID) ||
		    le32_to_cpu(blk->family_id) != UF2_FAMILY_RP2350_ARM_S) {
			dev_err(fw->dev, "UF2 block is not for an RP2350 Arm image\n");
			return FW_UPLOAD_ERR_FW_INVALID;
		}

		if (le32_to_cpu(blk->payload_size) != FOMCU_FWUPD_BLOCK_SIZE ||
		    le32_to_cpu(blk->target_addr) !=
				FO_FWUPD_FLASH_BASE + index * FOMCU_FWUPD_BLOCK_SIZE) {
			dev_err(fw->dev, "UF2 block does not continue the image at block %u\n",
				index);
			return FW_UPLOAD_ERR_FW_INVALID;
		}

		*payload = blk->data;
		*len = FOMCU_FWUPD_BLOCK_SIZE;
		*consumed = off + sizeof(*blk);
		return FW_UPLOAD_ERR_NONE;
	}

	*payload = NULL;
	*len = 0;
	*consumed = size;
	return FW_UPLOAD_ERR_NONE;
}

static bool fo_fwupd_has_picobin_marker(const u8 *data, u32 len)
{
	u32 off;

	for (off = 0; off + 4 <= len; off += 4)
		if (get_unaligned_le32(data + off) == PICOBIN_BLOCK_MARKER_START)
			return true;

	return false;
}

/*
 * Walk the whole image before the MCU erases anything, so that a file which
 * is not an RP2350 image cannot cost the rollback image in the inactive slot.
 */
static enum fw_upload_err fo_fwupd_parse(struct fo_fwupd *fw, const u8 *data,
					 u32 size, u16 *crc)
{
	u32 off = 0, blocks = 0, len, consumed;
	enum fw_upload_err ret;
	bool marker = false;
	const u8 *payload;

	fw->uf2 = size >= sizeof(struct uf2_block) &&
		  fo_uf2_is_block((const struct uf2_block *)data);
	if (fw->uf2 && size % sizeof(struct uf2_block)) {
		dev_err(fw->dev, "UF2 file size is not a multiple of its block size\n");
		return FW_UPLOAD_ERR_INVALID_SIZE;
	}

	*crc = 0;
	while (off < size) {
		ret = fo_fwupd_next_block(fw, data + off, size - off, blocks,
					  &payload, &len, &consumed);
		if (ret != FW_UPLOAD_ERR_NONE)
			return ret;

		off += consumed;
		if (!payload)
			break;

		if (blocks == U16_MAX) {
			dev_err(fw->dev, "Image is too large\n");
			return FW_UPLOAD_ERR_INVALID_SIZE;
		}

		if (!marker && blocks * FOMCU_FWUPD_BLOCK_SIZE < PICOBIN_SEARCH_LEN)
			marker = fo_fwupd_has_picobin_marker(payload, len);

		/* The MCU checks the image as sent, zero padding included */
		*crc = crc_itu_t(*crc, payload, len);
		for (; len < FOMCU_FWUPD_BLOCK_SIZE; len++)
			*crc = crc_itu_t_byte(*crc, 0);

		blocks++;
	}

	if (!blocks) {
		dev_err(fw->dev, "Image is empty\n");
		return FW_UPLOAD_ERR_INVALID_SIZE;
	}

	if (!marker) {
		dev_err(fw->dev, "Image has no RP2350 boot block\n");
		return FW_UPLOAD_ERR_FW_INVALID;
	}

	fw->blocks = blocks;
	fw->next_block = 0;
	return FW_UPLOAD_ERR_NONE;
}

static enum fw_upload_err fo_fwupd_prepare(struct fw_upload *fwl,
					   const u8 *data, u32 size)
{
	struct fo_fwupd *fw = fwl->dd_handle;
	enum fw_upload_err ret;
	unsigned int val;
	u16 crc;

	scoped_guard(mutex, &fw->lock) {
		if (fw->suspended)
			return FW_UPLOAD_ERR_BUSY;
		fw->busy = true;
	}

	/* cancel_req is left alone: a cancel may arrive before prepare() */
	fw->done = false;

	ret = fo_fwupd_parse(fw, data, size, &crc);
	if (ret != FW_UPLOAD_ERR_NONE)
		goto err;

	if (regmap_read(fw->regmap, FOMCU_REG_FWUPD_STATUS, &val)) {
		ret = FW_UPLOAD_ERR_RW_ERROR;
		goto err;
	}

	/* A previous update may have been interrupted or left an error */
	if (FIELD_GET(FOMCU_FWUPD_STATUS_STATE, val) != FO_FWUPD_STATE_IDLE &&
	    fo_fwupd_abort(fw)) {
		ret = FW_UPLOAD_ERR_BUSY;
		goto err;
	}

	if (READ_ONCE(fw->cancel_req)) {
		ret = FW_UPLOAD_ERR_CANCELED;
		goto err;
	}

	if (regmap_write(fw->regmap, FOMCU_REG_FWUPD_CRC, crc) ||
	    regmap_write(fw->regmap, FOMCU_REG_FWUPD_BLOCKS, fw->blocks) ||
	    regmap_write(fw->regmap, FOMCU_REG_FWUPD_CMD, FO_FWUPD_CMD_START)) {
		ret = FW_UPLOAD_ERR_RW_ERROR;
		goto err_abort;
	}

	/* The MCU leaves idle right away, before it starts erasing */
	ret = fo_fwupd_wait(fw, BIT(FO_FWUPD_STATE_RUNNING) |
				BIT(FO_FWUPD_STATE_BUSY) |
				BIT(FO_FWUPD_STATE_DONE),
			    FO_FWUPD_START_ACK_MS, FO_FWUPD_POLL_US, NULL);
	if (ret == FW_UPLOAD_ERR_TIMEOUT) {
		dev_err(fw->dev, "MCU did not start the update, its firmware may not support updates\n");
		ret = FW_UPLOAD_ERR_HW_ERROR;
	}
	if (ret != FW_UPLOAD_ERR_NONE)
		goto err_abort;

	ret = fo_fwupd_wait(fw, BIT(FO_FWUPD_STATE_RUNNING), FO_FWUPD_ERASE_MS,
			    FO_FWUPD_ERASE_POLL_US, "flash erase");
	if (ret != FW_UPLOAD_ERR_NONE)
		goto err_abort;

	return FW_UPLOAD_ERR_NONE;

err_abort:
	/* The upload core only runs cleanup() once prepare() has succeeded */
	fo_fwupd_abort(fw);
err:
	fo_fwupd_finish(fw);
	return ret;
}

static enum fw_upload_err fo_fwupd_write(struct fw_upload *fwl, const u8 *data,
					 u32 offset, u32 size, u32 *written)
{
	struct fo_fwupd *fw = fwl->dd_handle;
	u32 len, consumed;
	enum fw_upload_err ret;
	const u8 *payload;
	u16 block_crc;
	int err;

	ret = fo_fwupd_next_block(fw, data + offset, size, fw->next_block,
				  &payload, &len, &consumed);
	if (ret != FW_UPLOAD_ERR_NONE)
		return ret;
	if (!payload)
		return FW_UPLOAD_ERR_FW_INVALID;

	/*
	 * A RUNNING read may predate the MCU picking up the previous frame.
	 * That is harmless: the MCU queues frames and handles them in order,
	 * up to four at a time, and latches a protocol error on overflow.
	 * Each frame also takes longer on the bus than the MCU needs to
	 * program it, so the queue doesn't build up.
	 */
	ret = fo_fwupd_wait(fw, BIT(FO_FWUPD_STATE_RUNNING), FO_FWUPD_BLOCK_MS,
			    FO_FWUPD_POLL_US, "the next block");
	if (ret != FW_UPLOAD_ERR_NONE)
		return ret;

	memcpy(fw->frame, payload, len);
	memset(fw->frame + len, 0, FOMCU_FWUPD_BLOCK_SIZE - len);
	block_crc = crc_itu_t(0, fw->frame, FOMCU_FWUPD_BLOCK_SIZE);
	put_unaligned_le16(fw->next_block, &fw->frame[FOMCU_FWUPD_BLOCK_SIZE]);
	put_unaligned_le16(block_crc, &fw->frame[FOMCU_FWUPD_BLOCK_SIZE + 2]);

	/* The MCU only takes a frame written in a single transfer */
	err = regmap_raw_write(fw->regmap, FOMCU_REG_FWUPD_DATA, fw->frame,
			       sizeof(fw->frame));
	if (err) {
		dev_err(fw->dev, "Failed to send block %u: %d\n",
			fw->next_block, err);
		return FW_UPLOAD_ERR_RW_ERROR;
	}

	/* Whatever follows the last block holds nothing for the MCU */
	if (++fw->next_block == fw->blocks)
		consumed = size;

	*written = consumed;
	return FW_UPLOAD_ERR_NONE;
}

static enum fw_upload_err fo_fwupd_poll_complete(struct fw_upload *fwl)
{
	struct fo_fwupd *fw = fwl->dd_handle;
	enum fw_upload_err ret;

	ret = fo_fwupd_wait(fw, BIT(FO_FWUPD_STATE_DONE), FO_FWUPD_FINALIZE_MS,
			    FO_FWUPD_POLL_US, "image verification");
	if (ret != FW_UPLOAD_ERR_NONE)
		return ret;

	fw->done = true;
	dev_info(fw->dev, "MCU firmware staged, it runs once the MCU restarts\n");
	return FW_UPLOAD_ERR_NONE;
}

static void fo_fwupd_cancel(struct fw_upload *fwl)
{
	struct fo_fwupd *fw = fwl->dd_handle;

	WRITE_ONCE(fw->cancel_req, true);
}

static void fo_fwupd_cleanup(struct fw_upload *fwl)
{
	struct fo_fwupd *fw = fwl->dd_handle;

	if (!fw->done)
		fo_fwupd_abort(fw);

	fo_fwupd_finish(fw);
}

static const struct fw_upload_ops fo_fwupd_ops = {
	.prepare	= fo_fwupd_prepare,
	.write		= fo_fwupd_write,
	.poll_complete	= fo_fwupd_poll_complete,
	.cancel		= fo_fwupd_cancel,
	.cleanup	= fo_fwupd_cleanup,
};

static ssize_t firmware_version_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct fo_fwupd *fw = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = regmap_read(fw->regmap, FOMCU_REG_FWUPD_VERSION, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%lu.%lu.%lu\n",
			  FIELD_GET(FO_FWUPD_VERSION_MAJOR, val),
			  FIELD_GET(FO_FWUPD_VERSION_MINOR, val),
			  FIELD_GET(FO_FWUPD_VERSION_PATCH, val));
}
static DEVICE_ATTR_RO(firmware_version);

static ssize_t target_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct fo_fwupd *fw = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", fw->target);
}
static DEVICE_ATTR_RO(target);

static struct attribute *fo_fwupd_attrs[] = {
	&dev_attr_firmware_version.attr,
	&dev_attr_target.attr,
	NULL
};

static umode_t fo_fwupd_attr_is_visible(struct kobject *kobj,
					struct attribute *attr, int n)
{
	struct fo_fwupd *fw = dev_get_drvdata(kobj_to_dev(kobj));

	if (attr == &dev_attr_target.attr && !fw->target)
		return 0;

	return attr->mode;
}

static const struct attribute_group fo_fwupd_group = {
	.attrs = fo_fwupd_attrs,
	.is_visible = fo_fwupd_attr_is_visible,
};
__ATTRIBUTE_GROUPS(fo_fwupd);

/* MCU firmware build target for each board revision */
static const struct of_device_id fo_fwupd_targets[] = {
	{ .compatible = "flipper,one-rev-f0b0c1", .data = "f1" },
	{ .compatible = "flipper,one-rev-f0b1c2", .data = "f2" },
	{ /* sentinel */ }
};

static void fo_fwupd_unregister(void *data)
{
	firmware_upload_unregister(data);
}

static int fo_fwupd_probe(struct platform_device *pdev)
{
	struct fo_fwupd *fw;
	int ret;

	fw = devm_kzalloc(&pdev->dev, sizeof(*fw), GFP_KERNEL);
	if (!fw)
		return -ENOMEM;

	fw->dev = &pdev->dev;
	fw->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!fw->regmap)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "Failed to get parent regmap\n");

	ret = devm_mutex_init(&pdev->dev, &fw->lock);
	if (ret)
		return ret;

	fw->target = of_machine_get_match_data(fo_fwupd_targets);
	platform_set_drvdata(pdev, fw);

	fw->fwl = firmware_upload_register(THIS_MODULE, &pdev->dev,
					   "flipper-one-mcu", &fo_fwupd_ops, fw);
	if (IS_ERR(fw->fwl))
		return dev_err_probe(&pdev->dev, PTR_ERR(fw->fwl),
				     "Failed to register firmware upload\n");

	return devm_add_action_or_reset(&pdev->dev, fo_fwupd_unregister,
					fw->fwl);
}

static int fo_fwupd_suspend(struct device *dev)
{
	struct fo_fwupd *fw = dev_get_drvdata(dev);

	guard(mutex)(&fw->lock);

	/* Don't leave the MCU stranded halfway through an erase or transfer */
	if (fw->busy) {
		dev_warn(dev, "MCU firmware update in progress\n");
		return -EBUSY;
	}

	/* An upload queued meanwhile must not start until resume */
	fw->suspended = true;
	return 0;
}

static int fo_fwupd_resume(struct device *dev)
{
	struct fo_fwupd *fw = dev_get_drvdata(dev);

	guard(mutex)(&fw->lock);

	fw->suspended = false;
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(fo_fwupd_pm_ops, fo_fwupd_suspend,
				fo_fwupd_resume);

static const struct platform_device_id fo_fwupd_id_table[] = {
	{ "flipper-one-fw-update" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(platform, fo_fwupd_id_table);

static struct platform_driver fo_fwupd_driver = {
	.probe		= fo_fwupd_probe,
	.driver		= {
		.name		= "flipper-one-fw-update",
		.pm		= pm_sleep_ptr(&fo_fwupd_pm_ops),
		.dev_groups	= fo_fwupd_groups,
	},
	.id_table	= fo_fwupd_id_table,
};
module_platform_driver(fo_fwupd_driver);

MODULE_DESCRIPTION("Flipper One MCU firmware update driver");
MODULE_AUTHOR("Alexey Charkov <alchark@flipper.net>");
MODULE_LICENSE("GPL");
