/*
 * Copyright (c) 2015 NVIDIA Corporation. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/i2c.h>
#include <linux/minmax.h>
#include <linux/overflow.h>
#include <linux/slab.h>

#include <drm/display/drm_scdc_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_print.h>

/**
 * DOC: scdc helpers
 *
 * Status and Control Data Channel (SCDC) is a mechanism introduced by the
 * HDMI 2.0 specification. It is a point-to-point protocol that allows the
 * HDMI source and HDMI sink to exchange data. The same I2C interface that
 * is used to access EDID serves as the transport mechanism for SCDC.
 *
 * Note: The SCDC status is going to be lost when the display is
 * disconnected. This can happen physically when the user disconnects
 * the cable, but also when a display is switched on (such as waking up
 * a TV).
 *
 * This is further complicated by the fact that, upon a disconnection /
 * reconnection, KMS won't change the mode on its own. This means that
 * one can't just rely on setting the SCDC status on enable, but also
 * has to track the connector status changes using interrupts and
 * restore the SCDC status. The typical solution for this is to trigger an
 * empty modeset in drm_connector_helper_funcs.detect_ctx(), like what vc4 does
 * in vc4_hdmi_reset_link().
 */

#define SCDC_I2C_SLAVE_ADDRESS 0x54

#define drm_scdc_dbg(connector, fmt, ...)					\
	drm_dbg_kms((connector)->dev, "[CONNECTOR:%d:%s] " fmt,			\
		    (connector)->base.id, (connector)->name, ##__VA_ARGS__)

static const char *drm_scdc_frl_rate_str(enum drm_scdc_frl_rate rate)
{
	switch (rate) {
	case SCDC_FRL_RATE_OFF:
		return "Off";
	case SCDC_FRL_RATE_3X3:
		return "3 Gbit/s x 3 lanes";
	case SCDC_FRL_RATE_6X3:
		return "6 Gbit/s x 3 lanes";
	case SCDC_FRL_RATE_6X4:
		return "6 Gbit/s x 4 lanes";
	case SCDC_FRL_RATE_8X4:
		return "8 Gbit/s x 4 lanes";
	case SCDC_FRL_RATE_10X4:
		return "10 Gbit/s x 4 lanes";
	case SCDC_FRL_RATE_12X4:
		return "12 Gbit/s x 4 lanes";
	case SCDC_FRL_RATE_RESV_7:
	case SCDC_FRL_RATE_RESV_8:
	case SCDC_FRL_RATE_RESV_9:
	case SCDC_FRL_RATE_RESV_10:
	case SCDC_FRL_RATE_RESV_11:
	case SCDC_FRL_RATE_RESV_12:
	case SCDC_FRL_RATE_RESV_13:
	case SCDC_FRL_RATE_RESV_14:
	case SCDC_FRL_RATE_RESV_15:
		return "(Reserved)";
	default:
		return NULL;
	}
}

/**
 * drm_scdc_read - read a block of data from SCDC
 * @adapter: I2C controller
 * @offset: start offset of block to read
 * @buffer: return location for the block to read
 * @size: size of the block to read
 *
 * Reads a block of data from SCDC, starting at a given offset.
 *
 * Returns:
 * 0 on success, negative error code on failure.
 */
int drm_scdc_read(struct i2c_adapter *adapter, u8 offset, void *buffer,
		  size_t size)
{
	int ret;
	struct i2c_msg msgs[2] = {
		{
			.addr = SCDC_I2C_SLAVE_ADDRESS,
			.flags = 0,
			.len = 1,
			.buf = &offset,
		}, {
			.addr = SCDC_I2C_SLAVE_ADDRESS,
			.flags = I2C_M_RD,
			.len = size,
			.buf = buffer,
		}
	};

	ret = i2c_transfer(adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	if (ret != ARRAY_SIZE(msgs))
		return -EPROTO;

	return 0;
}
EXPORT_SYMBOL(drm_scdc_read);

/**
 * drm_scdc_write - write a block of data to SCDC
 * @adapter: I2C controller
 * @offset: start offset of block to write
 * @buffer: block of data to write
 * @size: size of the block to write
 *
 * Writes a block of data to SCDC, starting at a given offset.
 *
 * Returns:
 * 0 on success, negative error code on failure.
 */
int drm_scdc_write(struct i2c_adapter *adapter, u8 offset, const void *buffer,
		   size_t size)
{
	struct i2c_msg msg = {
		.addr = SCDC_I2C_SLAVE_ADDRESS,
		.flags = 0,
		.len = 1 + size,
		.buf = NULL,
	};
	void *data;
	int err;

	data = kmalloc(1 + size, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	msg.buf = data;

	memcpy(data, &offset, sizeof(offset));
	memcpy(data + 1, buffer, size);

	err = i2c_transfer(adapter, &msg, 1);

	kfree(data);

	if (err < 0)
		return err;
	if (err != 1)
		return -EPROTO;

	return 0;
}
EXPORT_SYMBOL(drm_scdc_write);

/**
 * drm_scdc_get_scrambling_status - what is status of scrambling?
 * @connector: connector
 *
 * Reads the scrambler status over SCDC, and checks the
 * scrambling status.
 *
 * Returns:
 * True if the scrambling is enabled, false otherwise.
 */
bool drm_scdc_get_scrambling_status(struct drm_connector *connector)
{
	u8 status;
	int ret;

	ret = drm_scdc_readb(connector->ddc, SCDC_SCRAMBLER_STATUS, &status);
	if (ret < 0) {
		drm_scdc_dbg(connector, "Failed to read scrambling status: %d\n", ret);
		return false;
	}

	return status & SCDC_SCRAMBLING_STATUS;
}
EXPORT_SYMBOL(drm_scdc_get_scrambling_status);

/**
 * drm_scdc_set_scrambling - enable scrambling
 * @connector: connector
 * @enable: bool to indicate if scrambling is to be enabled/disabled
 *
 * Writes the TMDS config register over SCDC channel, and:
 * enables scrambling when enable = 1
 * disables scrambling when enable = 0
 *
 * Returns:
 * True if scrambling is set/reset successfully, false otherwise.
 */
bool drm_scdc_set_scrambling(struct drm_connector *connector,
			     bool enable)
{
	u8 config;
	int ret;

	ret = drm_scdc_readb(connector->ddc, SCDC_TMDS_CONFIG, &config);
	if (ret < 0) {
		drm_scdc_dbg(connector, "Failed to read TMDS config: %d\n", ret);
		return false;
	}

	if (enable)
		config |= SCDC_SCRAMBLING_ENABLE;
	else
		config &= ~SCDC_SCRAMBLING_ENABLE;

	ret = drm_scdc_writeb(connector->ddc, SCDC_TMDS_CONFIG, config);
	if (ret < 0) {
		drm_scdc_dbg(connector, "Failed to enable scrambling: %d\n", ret);
		return false;
	}

	return true;
}
EXPORT_SYMBOL(drm_scdc_set_scrambling);

/**
 * drm_scdc_set_high_tmds_clock_ratio - set TMDS clock ratio
 * @connector: connector
 * @set: ret or reset the high clock ratio
 *
 *
 *	TMDS clock ratio calculations go like this:
 *		TMDS character = 10 bit TMDS encoded value
 *
 *		TMDS character rate = The rate at which TMDS characters are
 *		transmitted (Mcsc)
 *
 *		TMDS bit rate = 10x TMDS character rate
 *
 *	As per the spec:
 *		TMDS clock rate for pixel clock < 340 MHz = 1x the character
 *		rate = 1/10 pixel clock rate
 *
 *		TMDS clock rate for pixel clock > 340 MHz = 0.25x the character
 *		rate = 1/40 pixel clock rate
 *
 *	Writes to the TMDS config register over SCDC channel, and:
 *		sets TMDS clock ratio to 1/40 when set = 1
 *
 *		sets TMDS clock ratio to 1/10 when set = 0
 *
 * Returns:
 * True if write is successful, false otherwise.
 */
bool drm_scdc_set_high_tmds_clock_ratio(struct drm_connector *connector,
					bool set)
{
	u8 config;
	int ret;

	ret = drm_scdc_readb(connector->ddc, SCDC_TMDS_CONFIG, &config);
	if (ret < 0) {
		drm_scdc_dbg(connector, "Failed to read TMDS config: %d\n", ret);
		return false;
	}

	if (set)
		config |= SCDC_TMDS_BIT_CLOCK_RATIO_BY_40;
	else
		config &= ~SCDC_TMDS_BIT_CLOCK_RATIO_BY_40;

	ret = drm_scdc_writeb(connector->ddc, SCDC_TMDS_CONFIG, config);
	if (ret < 0) {
		drm_scdc_dbg(connector, "Failed to set TMDS clock ratio: %d\n", ret);
		return false;
	}

	/*
	 * The spec says that a source should wait minimum 1ms and maximum
	 * 100ms after writing the TMDS config for clock ratio. Lets allow a
	 * wait of up to 2ms here.
	 */
	usleep_range(1000, 2000);
	return true;
}
EXPORT_SYMBOL(drm_scdc_set_high_tmds_clock_ratio);

/**
 * drm_scdc_set_source_version - set SCDC source version on the sink
 * @connector: connector
 * @ver: source version to advertise (per spec, 1)
 *
 * Reads the sink version for diagnostics and as a guard, then writes
 * the source version unless the sink reports version 0.
 *
 * Returns:
 * 0 on success or when skipped; a negative error code when either
 * the read or the write failed.
 */
int drm_scdc_set_source_version(struct drm_connector *connector, u8 ver)
{
	u8 sink_ver;
	int ret;

	ret = drm_scdc_readb(connector->ddc, SCDC_SINK_VERSION, &sink_ver);
	if (ret) {
		drm_scdc_dbg(connector, "Failed to read SCDC_SINK_VERSION: %d\n", ret);
		return ret;
	}

	drm_scdc_dbg(connector, "Sink reported SCDC ver. %u\n", sink_ver);

	/*
	 * Only advertise our source version to sinks that report a
	 * non-zero sink version.  A sink reporting version 0 is either
	 * not SCDC-version-aware or non-conformant; writing the source
	 * version gains nothing and may upset broken hardware.
	 */
	if (sink_ver) {
		ret = drm_scdc_writeb(connector->ddc, SCDC_SOURCE_VERSION,
				      min_t(u8, sink_ver, ver));
		if (ret)
			drm_scdc_dbg(connector,
				     "Failed to write SCDC_SOURCE_VERSION: %d\n", ret);
	}

	return ret;
}
EXPORT_SYMBOL(drm_scdc_set_source_version);

static void
drm_scdc_parse_status0_flags(u8 val, struct drm_scdc_status_flags *flags)
{
	flags->clock_detected = val & SCDC_CLOCK_DETECT;
	flags->ch0_locked = val & SCDC_CH0_LOCK;
	flags->ch1_locked = val & SCDC_CH1_LOCK;
	flags->ch2_locked = val & SCDC_CH2_LOCK;
	flags->ln3_locked = val & SCDC_LN3_LOCK;
	flags->flt_ready = val & SCDC_FLT_READY;
	flags->dsc_fail = val & SCDC_DSC_FAIL;
}

static void
drm_scdc_parse_status1_2_flags(u8 val_flag1, u8 val_flag2,
			       struct drm_scdc_status_flags *flags)
{
	flags->ln0_training_pattern = FIELD_GET(SCDC_LN_EVEN_TRAIN_PTRN, val_flag1);
	flags->ln1_training_pattern = FIELD_GET(SCDC_LN_ODD_TRAIN_PTRN, val_flag1);

	flags->ln2_training_pattern = FIELD_GET(SCDC_LN_EVEN_TRAIN_PTRN, val_flag2);
	flags->ln3_training_pattern = FIELD_GET(SCDC_LN_ODD_TRAIN_PTRN, val_flag2);
}

static int drm_scdc_parse_error_counters(const u8 scdc[256], u16 counter[4],
					 unsigned int num_lanes)
{
	u8 end_reg;
	u8 sum = 0;
	int i;

	switch (num_lanes) {
	case 3:
		end_reg = SCDC_ERR_DET_CHECKSUM;
		break;
	case 4:
		end_reg = SCDC_ERR_DET_3_H;
		break;
	default:
		return -EINVAL;
	}

	for (i = SCDC_ERR_DET_0_L; i <= end_reg; i++)
		sum = wrapping_add(u8, sum, scdc[i]);

	if (sum)
		return -EPROTO;

	for (i = 0; i < 3; i++) {
		if (scdc[SCDC_ERR_DET_0_H + i * 2] & SCDC_CHANNEL_VALID)
			counter[i] =  (scdc[SCDC_ERR_DET_0_H + i * 2] &
				       ~SCDC_CHANNEL_VALID) << 8 |
				       scdc[SCDC_ERR_DET_0_L + i * 2];
		else
			counter[i] = 0;
	}

	if (num_lanes == 4 && scdc[SCDC_ERR_DET_3_H] & SCDC_CHANNEL_VALID)
		counter[3] =  (scdc[SCDC_ERR_DET_3_H] & ~SCDC_CHANNEL_VALID) << 8 |
			      scdc[SCDC_ERR_DET_3_L];
	else
		counter[3] = 0;

	return 0;
}

/**
 * drm_scdc_read_state - Update state from SCDC
 * @connector: pointer to a &struct drm_connector on which to operate on
 * @state: pointer to a &struct drm_scdc_state to fill
 *
 * Reads the entire 256 byte SCDC state and parses it.
 *
 * Returns: %0 on success, negative errno on failure.
 */
int drm_scdc_read_state(struct drm_connector *connector, struct drm_scdc_state *state)
{
	struct i2c_adapter *ddc;
	struct drm_scdc *scdc;
	u8 *buf = state->scdc;
	int num_lanes;
	int ret;

	if (!state || !connector)
		return -ENODEV;

	scdc = &connector->display_info.hdmi.scdc;
	ddc = connector->ddc;

	if (!scdc->supported || !ddc)
		return -EOPNOTSUPP;

	/* Read in 128-byte chunks, to work around DP<->HDMI converters with issues. */
	ret = drm_scdc_read(ddc, 0, buf, 128);
	if (ret)
		return ret;

	ret = drm_scdc_read(ddc, 128, &buf[128], 128);
	if (ret)
		return ret;

	state->scrambling_enabled = buf[SCDC_TMDS_CONFIG] & SCDC_SCRAMBLING_ENABLE;
	state->tmds_bclk_x40 = buf[SCDC_TMDS_CONFIG] & SCDC_TMDS_BIT_CLOCK_RATIO_BY_40;

	state->scrambling_detected = buf[SCDC_SCRAMBLER_STATUS] & SCDC_SCRAMBLING_STATUS;

	state->rate = FIELD_GET(SCDC_FRL_RATE, buf[SCDC_CONFIG_1]);
	num_lanes = drm_scdc_num_frl_lanes(state->rate);
	if (num_lanes < 0)
		return num_lanes;
	if (!num_lanes)
		num_lanes = 3;

	state->ffe_levels = FIELD_GET(SCDC_FFE_LEVELS, buf[SCDC_CONFIG_1]);

	drm_scdc_parse_status0_flags(buf[SCDC_STATUS_FLAGS_0], &state->stf);
	drm_scdc_parse_status1_2_flags(buf[SCDC_STATUS_FLAGS_1],
				       buf[SCDC_STATUS_FLAGS_2], &state->stf);
	ret = drm_scdc_parse_error_counters(buf, state->error_count, num_lanes);
	if (ret)
		return ret;

	if (state->rate && (buf[SCDC_ERR_DET_RS_H] & SCDC_CHANNEL_VALID))
		state->rs_corrections = (buf[SCDC_ERR_DET_RS_H] & ~SCDC_CHANNEL_VALID) << 8 |
					buf[SCDC_ERR_DET_RS_L];

	return 0;
}
EXPORT_SYMBOL(drm_scdc_read_state);

#define scdc_print_str(_f, key, s) \
	(seq_printf((_f), "%-30s: %s\n", (key), (s)))
#define scdc_print_flag(_f, key, val) \
	(scdc_print_str((_f), (key), str_yes_no((val))))
#define scdc_print_dec(_f, key, val) \
	(seq_printf((_f), "%-30s: %d\n", (key), (val)))

static int scdc_status_show(struct seq_file *m, void *data)
{
	struct drm_connector *connector = m->private;
	struct drm_scdc *scdc = &connector->display_info.hdmi.scdc;
	struct drm_scdc_state *st;
	int i, ret, frl_lanes;

	drm_connector_get(connector);

	ret = mutex_lock_interruptible(&connector->dev->mode_config.mutex);
	if (ret)
		goto err_conn_put;

	if (connector->status != connector_status_connected) {
		ret = -ENODEV;
		goto err_unlock;
	}

	if (scdc->supported) {
		st = kzalloc_obj(*st);
		if (!st) {
			ret = -ENOMEM;
			goto err_unlock;
		}
		ret = drm_scdc_read_state(connector, st);
		if (ret)
			goto err_free_state;

		for (i = 0; i < ARRAY_SIZE(st->scdc); i += 16)
			seq_printf(m, "%*ph\n", 16, &st->scdc[i]);

		seq_puts(m, "\n----------------\n\n");
	}

	scdc_print_flag(m, "SCDC Supported", scdc->supported);
	if (!scdc->supported) {
		ret = 0;
		goto err_unlock;
	}

	scdc_print_flag(m, "Sink Read Request Capable", scdc->read_request);
	scdc_print_flag(m, "Scrambling Supported", scdc->scrambling.supported);
	scdc_print_flag(m, "Low Rate Scrambling Supported", scdc->scrambling.low_rates);

	mutex_unlock(&connector->dev->mode_config.mutex);

	drm_connector_put(connector);

	frl_lanes = drm_scdc_num_frl_lanes(st->rate);

	scdc_print_flag(m, "Scrambling Enabled", st->scrambling_enabled);
	scdc_print_flag(m, "Scrambling Detected", st->scrambling_detected);
	scdc_print_str(m, "FRL Rate", drm_scdc_frl_rate_str(st->rate));
	scdc_print_dec(m, "FFE Levels", st->ffe_levels);

	if (st->tmds_bclk_x40)
		scdc_print_str(m, "TMDS Bit Clock Ratio", "1/40");
	else
		scdc_print_str(m, "TMDS Bit Clock Ratio", "1/10");

	scdc_print_flag(m, "Clock Detected", st->stf.clock_detected);
	scdc_print_flag(m, "Channel 0 Locked", st->stf.ch0_locked);
	scdc_print_flag(m, "Channel 1 Locked", st->stf.ch1_locked);
	scdc_print_flag(m, "Channel 2 Locked", st->stf.ch2_locked);
	if (frl_lanes == 4)
		scdc_print_flag(m, "Lane 3 Locked", st->stf.ln3_locked);

	scdc_print_flag(m, "Sink Ready For Link Training", st->stf.flt_ready);
	scdc_print_flag(m, "Sink Failed To Decode DSC", st->stf.dsc_fail);

	scdc_print_dec(m, "Channel 0 Errors", st->error_count[0]);
	scdc_print_dec(m, "Channel 1 Errors", st->error_count[1]);
	scdc_print_dec(m, "Channel 2 Errors", st->error_count[2]);
	if (frl_lanes == 4)
		scdc_print_dec(m, "Lane 3 Errors", st->error_count[3]);
	if (frl_lanes > 0)
		scdc_print_dec(m, "Reed-Solomon Corrections", st->rs_corrections);

	kfree(st);

	return 0;

err_free_state:
	kfree(st);
err_unlock:
	mutex_unlock(&connector->dev->mode_config.mutex);
err_conn_put:
	drm_connector_put(connector);

	return ret;
}
DEFINE_SHOW_ATTRIBUTE(scdc_status);

/**
 * drm_scdc_debugfs_init - Initialize scdc files in connector debugfs
 * @connector: pointer to &struct drm_connector to operate on
 * @root: debugfs &struct dentry for the debugfs root of @connector
 *
 * Creates SCDC-related debugfs files for @connector. Must be called after
 * @root is already created.
 */
void drm_scdc_debugfs_init(struct drm_connector *connector, struct dentry *root)
{
	if (!root || !connector)
		return;

	debugfs_create_file("scdc_status", 0444, root, connector, &scdc_status_fops);
}
EXPORT_SYMBOL(drm_scdc_debugfs_init);

static int drm_scdc_frl_config_to_rate(u8 config, u8 *rate_per_lane, u8 *lanes)
{
	switch (config) {
	case SCDC_FRL_RATE_12GBPS_4LANE:
		*rate_per_lane = 12;
		*lanes = 4;
		return 0;
	case SCDC_FRL_RATE_10GBPS_4LANE:
		*rate_per_lane = 10;
		*lanes = 4;
		return 0;
	case SCDC_FRL_RATE_8GBPS_4LANE:
		*rate_per_lane = 8;
		*lanes = 4;
		return 0;
	case SCDC_FRL_RATE_6GBPS_4LANE:
		*rate_per_lane = 6;
		*lanes = 4;
		return 0;
	case SCDC_FRL_RATE_6GBPS_3LANE:
		*rate_per_lane = 6;
		*lanes = 3;
		return 0;
	case SCDC_FRL_RATE_3GBPS_3LANE:
		*rate_per_lane = 3;
		*lanes = 3;
		return 0;
	case SCDC_FRL_RATE_DISABLE:
		*rate_per_lane = 0;
		*lanes = 0;
		return 0;
	default:
		return -EINVAL;
	}
}

static int drm_scdc_frl_rate_to_config(u8 rate_per_lane, u8 lanes)
{
	if (lanes != 0 && lanes != 3 && lanes != 4)
		return -EINVAL;

	switch (rate_per_lane * lanes) {
	case 48:
		return SCDC_FRL_RATE_12GBPS_4LANE;
	case 40:
		return SCDC_FRL_RATE_10GBPS_4LANE;
	case 32:
		return SCDC_FRL_RATE_8GBPS_4LANE;
	case 24:
		return SCDC_FRL_RATE_6GBPS_4LANE;
	case 18:
		return SCDC_FRL_RATE_6GBPS_3LANE;
	case 9:
		return SCDC_FRL_RATE_3GBPS_3LANE;
	case 0:
		return SCDC_FRL_RATE_DISABLE;
	default:
		return -EINVAL;
	}
}

/**
 * drm_scdc_set_frl - set FRL rate and FFE
 * @connector: connector
 * @rate_per_lane: FRL rate for a single lane (Gbps)
 * @lanes: FRL lane count (3 or 4)
 * @max_ffe_level: max TxFFE level for indicated FRL Rate (0..3)
 *
 * Writes over SCDC the FRL config register over SCDC channel, and sets
 * FRL_Rate according to rate_per_lane x lanes, as well as FFE_levels
 * according to max_ffe_level.
 *
 * Returns:
 * True if write is successful, false otherwise.
 */
bool drm_scdc_set_frl(struct drm_connector *connector,
		      u8 rate_per_lane, u8 lanes, u8 max_ffe_level)
{
	u8 config;
	int ret;

	ret = drm_scdc_frl_rate_to_config(rate_per_lane, lanes);
	if (ret < 0 || max_ffe_level > 3) {
		drm_scdc_dbg(connector, "Invalid FRL config: rate=%ux%u ffe=%u\n",
			     rate_per_lane, lanes, max_ffe_level);
		return false;
	}

	config = FIELD_PREP(SCDC_FRL_RATE_MASK, ret) |
		 FIELD_PREP(SCDC_FFE_LEVELS_MASK, max_ffe_level);

	ret = drm_scdc_writeb(connector->ddc, SCDC_CONFIG_1, config);
	if (ret < 0) {
		drm_scdc_dbg(connector, "Failed to set FRL: %d\n", ret);
		return false;
	}

	return true;
}
EXPORT_SYMBOL(drm_scdc_set_frl);

/**
 * drm_scdc_calc_lower_frl - compute a reduced bandwidth FRL rate
 * @in_rate_per_lane: input FRL rate for a single lane (Gbps)
 * @in_lanes: input FRL lane count (3 or 4)
 * @out_rate_per_lane: output FRL rate for a single lane (Gbps)
 * @out_lanes: output FRL lane count (3 or 4)
 *
 * Determinates the FRL rate configuration with the highest bandwidth that is
 * still lower than the bandwidth corresponding to the given input configuration.
 * The resulting configuration is stored in out_rate_per_lane and out_lanes.
 *
 * Returns:
 * True if computation was successful, false otherwise.
 */
bool drm_scdc_calc_lower_frl(u8 in_rate_per_lane, u8 in_lanes,
			     u8 *out_rate_per_lane, u8 *out_lanes)
{
	int ret;

	ret = drm_scdc_frl_rate_to_config(in_rate_per_lane, in_lanes);
	if (ret < 0)
		return false;

	ret--;

	if (ret <= SCDC_FRL_RATE_DISABLE)
		return false;

	ret = drm_scdc_frl_config_to_rate(ret, out_rate_per_lane, out_lanes);

	return ret == 0;
}
EXPORT_SYMBOL(drm_scdc_calc_lower_frl);

/**
 * drm_scdc_get_frl_ltp_request - read LTP requested by the Sink for FRL lanes
 * @connector: connector
 * @ln0: output LTP request for FRL lane 0
 * @ln1: output LTP request for FRL lane 1
 * @ln2: output LTP request for FRL lane 2
 * @ln3: output LTP request for FRL lane 3
 *
 * Reads over SCDC the Link Training Pattern (LTP) requested by the Sink for
 * each of the four FRL lanes and stores the values in ln0..3.
 *
 * Returns:
 * True on success, false otherwise.
 */
bool drm_scdc_get_frl_ltp_request(struct drm_connector *connector,
				  u8 *ln0, u8 *ln1, u8 *ln2, u8 *ln3)
{
	u8 status;
	int ret;

	ret = drm_scdc_readb(connector->ddc, SCDC_STATUS_FLAGS_1, &status);
	if (ret < 0) {
		drm_scdc_dbg(connector, "Failed to read LTP{0,1}: %d\n", ret);
		return false;
	}

	*ln0 = FIELD_GET(SCDC_FRL_LN0_LTP_REQ_MASK, status);
	*ln1 = FIELD_GET(SCDC_FRL_LN1_LTP_REQ_MASK, status);

	ret = drm_scdc_readb(connector->ddc, SCDC_STATUS_FLAGS_2, &status);
	if (ret < 0) {
		drm_scdc_dbg(connector, "Failed to read LTP{2,3}: %d\n", ret);
		return false;
	}

	*ln2 = FIELD_GET(SCDC_FRL_LN2_LTP_REQ_MASK, status);
	*ln3 = FIELD_GET(SCDC_FRL_LN3_LTP_REQ_MASK, status);

	return true;
}
EXPORT_SYMBOL(drm_scdc_get_frl_ltp_request);
