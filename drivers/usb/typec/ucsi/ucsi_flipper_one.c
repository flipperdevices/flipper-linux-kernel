// SPDX-License-Identifier: GPL-2.0
/*
 * UCSI driver for the Flipper One MCU Type-C controller
 * Copyright (C) 2026 Flipper FZCO
 */

#include <linux/interrupt.h>
#include <linux/mfd/flipper-one-mcu.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/unaligned.h>

#include "ucsi.h"

struct flipper_one_ucsi {
	struct device *dev;
	struct regmap *regmap;
	struct ucsi *ucsi;
	int irq;
};

/*
 * Read @len bytes from the UCSI register at MCU address @reg.
 *
 * regmap_raw_read() only accepts lengths that are a multiple of the register
 * width, but UCSI messages can have odd lengths, so round @len up into a bounce
 * buffer (the largest possible read is one message window)
 */
static int flipper_one_ucsi_read(struct ucsi *ucsi, unsigned int reg,
				 void *val, size_t len)
{
	struct flipper_one_ucsi *fo = ucsi_get_drvdata(ucsi);
	u8 buf[FOMCU_UCSI_MESSAGE_LEN];
	size_t aligned = round_up(len, regmap_get_val_bytes(fo->regmap));
	int ret;

	if (aligned > sizeof(buf))
		return -EINVAL;

	ret = regmap_raw_read(fo->regmap, reg, buf, aligned);
	if (ret)
		return ret;

	memcpy(val, buf, len);
	return 0;
}

static int flipper_one_ucsi_read_version(struct ucsi *ucsi, u16 *version)
{
	return flipper_one_ucsi_read(ucsi, FOMCU_REG_UCSI_VERSION, version,
				     sizeof(*version));
}

static int flipper_one_ucsi_read_cci(struct ucsi *ucsi, u32 *cci)
{
	return flipper_one_ucsi_read(ucsi, FOMCU_REG_UCSI_CCI, cci,
				     sizeof(*cci));
}

static int flipper_one_ucsi_read_message_in(struct ucsi *ucsi, void *val,
					    size_t len)
{
	return flipper_one_ucsi_read(ucsi, FOMCU_REG_UCSI_MESSAGE_IN, val, len);
}

static int flipper_one_ucsi_async_control(struct ucsi *ucsi, u64 command)
{
	struct flipper_one_ucsi *fo = ucsi_get_drvdata(ucsi);

	return regmap_raw_write(fo->regmap, FOMCU_REG_UCSI_CONTROL,
				&command, sizeof(command));
}

static const struct ucsi_operations flipper_one_ucsi_ops = {
	.read_version = flipper_one_ucsi_read_version,
	.read_cci = flipper_one_ucsi_read_cci,
	.poll_cci = flipper_one_ucsi_read_cci,
	.read_message_in = flipper_one_ucsi_read_message_in,
	.sync_control = ucsi_sync_control_common,
	.async_control = flipper_one_ucsi_async_control,
};

static irqreturn_t flipper_one_ucsi_irq(int irq, void *data)
{
	struct flipper_one_ucsi *fo = data;
	u32 cci;
	int ret;

	ret = flipper_one_ucsi_read_cci(fo->ucsi, &cci);
	if (ret)
		return IRQ_NONE;

	ucsi_notify_common(fo->ucsi, cci);

	return IRQ_HANDLED;
}

static int flipper_one_ucsi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct flipper_one_ucsi *fo;
	int ret;

	fo = devm_kzalloc(dev, sizeof(*fo), GFP_KERNEL);
	if (!fo)
		return -ENOMEM;

	fo->dev = dev;

	fo->regmap = dev_get_regmap(dev->parent, NULL);
	if (!fo->regmap)
		return dev_err_probe(dev, -ENODEV, "Failed to get MCU regmap\n");

	fo->irq = platform_get_irq(pdev, 0);
	if (fo->irq < 0)
		return fo->irq;

	fo->ucsi = ucsi_create(dev, &flipper_one_ucsi_ops);
	if (IS_ERR(fo->ucsi))
		return PTR_ERR(fo->ucsi);

	ucsi_set_drvdata(fo->ucsi, fo);
	platform_set_drvdata(pdev, fo);

	ret = request_threaded_irq(fo->irq, NULL, flipper_one_ucsi_irq,
				   IRQF_ONESHOT, dev_name(dev), fo);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to request IRQ\n");
		goto err_destroy;
	}

	ret = ucsi_register(fo->ucsi);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to register UCSI\n");
		goto err_free_irq;
	}

	return 0;

err_free_irq:
	free_irq(fo->irq, fo);
err_destroy:
	ucsi_destroy(fo->ucsi);
	return ret;
}

static void flipper_one_ucsi_remove(struct platform_device *pdev)
{
	struct flipper_one_ucsi *fo = platform_get_drvdata(pdev);

	ucsi_unregister(fo->ucsi);
	free_irq(fo->irq, fo);
	ucsi_destroy(fo->ucsi);
}

static const struct platform_device_id flipper_one_ucsi_ids[] = {
	{ "flipper-one-typec" },
	{}
};
MODULE_DEVICE_TABLE(platform, flipper_one_ucsi_ids);

static struct platform_driver flipper_one_ucsi_driver = {
	.driver = {
		.name = "flipper-one-typec",
	},
	.probe = flipper_one_ucsi_probe,
	.remove = flipper_one_ucsi_remove,
	.id_table = flipper_one_ucsi_ids,
};
module_platform_driver(flipper_one_ucsi_driver);

MODULE_DESCRIPTION("UCSI driver for Flipper One MCU Type-C controller");
MODULE_AUTHOR("Alexey Charkov <alchark@flipper.net>");
MODULE_LICENSE("GPL");
