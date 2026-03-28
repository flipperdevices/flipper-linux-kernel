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
#include <linux/export.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/delay.h>

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
ssize_t drm_scdc_read(struct i2c_adapter *adapter, u8 offset, void *buffer,
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
ssize_t drm_scdc_write(struct i2c_adapter *adapter, u8 offset,
		       const void *buffer, size_t size)
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
		drm_dbg_kms(connector->dev,
			    "[CONNECTOR:%d:%s] Failed to read scrambling status: %d\n",
			    connector->base.id, connector->name, ret);
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
		drm_dbg_kms(connector->dev,
			    "[CONNECTOR:%d:%s] Failed to read TMDS config: %d\n",
			    connector->base.id, connector->name, ret);
		return false;
	}

	if (enable)
		config |= SCDC_SCRAMBLING_ENABLE;
	else
		config &= ~SCDC_SCRAMBLING_ENABLE;

	ret = drm_scdc_writeb(connector->ddc, SCDC_TMDS_CONFIG, config);
	if (ret < 0) {
		drm_dbg_kms(connector->dev,
			    "[CONNECTOR:%d:%s] Failed to enable scrambling: %d\n",
			    connector->base.id, connector->name, ret);
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
		drm_dbg_kms(connector->dev,
			    "[CONNECTOR:%d:%s] Failed to read TMDS config: %d\n",
			    connector->base.id, connector->name, ret);
		return false;
	}

	if (set)
		config |= SCDC_TMDS_BIT_CLOCK_RATIO_BY_40;
	else
		config &= ~SCDC_TMDS_BIT_CLOCK_RATIO_BY_40;

	ret = drm_scdc_writeb(connector->ddc, SCDC_TMDS_CONFIG, config);
	if (ret < 0) {
		drm_dbg_kms(connector->dev,
			    "[CONNECTOR:%d:%s] Failed to set TMDS clock ratio: %d\n",
			    connector->base.id, connector->name, ret);
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
		drm_dbg_kms(connector->dev,
			    "[CONNECTOR:%d:%s] Invalid FRL config: rate=%ux%u ffe=%u\n",
			    connector->base.id, connector->name, rate_per_lane,
			    lanes, max_ffe_level);
		return false;
	}

	config = FIELD_PREP(SCDC_FRL_RATE_MASK, ret) |
		 FIELD_PREP(SCDC_FFE_LEVELS_MASK, max_ffe_level);

	ret = drm_scdc_writeb(connector->ddc, SCDC_CONFIG_1, config);
	if (ret < 0) {
		drm_dbg_kms(connector->dev,
			    "[CONNECTOR:%d:%s] Failed to set FRL: %d\n",
			    connector->base.id, connector->name, ret);
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
		drm_dbg_kms(connector->dev,
			    "[CONNECTOR:%d:%s] Failed to read LTP{0,1}: %d\n",
			    connector->base.id, connector->name, ret);
		return false;
	}

	*ln0 = FIELD_GET(SCDC_FRL_LN0_LTP_REQ_MASK, status);
	*ln1 = FIELD_GET(SCDC_FRL_LN1_LTP_REQ_MASK, status);

	ret = drm_scdc_readb(connector->ddc, SCDC_STATUS_FLAGS_2, &status);
	if (ret < 0) {
		drm_dbg_kms(connector->dev,
			    "[CONNECTOR:%d:%s] Failed to read LTP{2,3}: %d\n",
			    connector->base.id, connector->name, ret);
		return false;
	}

	*ln2 = FIELD_GET(SCDC_FRL_LN2_LTP_REQ_MASK, status);
	*ln3 = FIELD_GET(SCDC_FRL_LN3_LTP_REQ_MASK, status);

	return true;
}
EXPORT_SYMBOL(drm_scdc_get_frl_ltp_request);
