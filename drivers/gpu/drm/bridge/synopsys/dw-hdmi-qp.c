// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2021-2022 Rockchip Electronics Co., Ltd.
 * Copyright (c) 2024 Collabora Ltd.
 * Copyright (c) 2025 Amazon.com, Inc. or its affiliates.
 *
 * Author: Algea Cao <algea.cao@rock-chips.com>
 * Author: Cristian Ciocaltea <cristian.ciocaltea@collabora.com>
 */
#include <linux/completion.h>
#include <linux/hdmi.h>
#include <linux/export.h>
#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/workqueue.h>

#include <drm/bridge/dw_hdmi_qp.h>
#include <drm/display/drm_hdmi_helper.h>
#include <drm/display/drm_hdmi_cec_helper.h>
#include <drm/display/drm_hdmi_state_helper.h>
#include <drm/display/drm_scdc_helper.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_bridge_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_edid.h>
#include <drm/drm_modes.h>
#include <drm/drm_print.h>

#include <media/cec.h>

#include <sound/hdmi-codec.h>

#include "dw-hdmi-qp.h"

#define DDC_CI_ADDR		0x37
#define DDC_SEGMENT_ADDR	0x30

#define HDMI14_MAX_TMDSCLK	340000000
#define HDMI20_MAX_TMDSRATE	600000000

#define SCDC_MAX_SOURCE_VERSION	0x1
#define SCRAMB_POLL_DELAY_MS	3000

/*
 * Recommended N and Expected CTS Values in FRL Mode.
 */
static const struct dw_hdmi_qp_audio_frl_n {
	unsigned int r_bit;
	unsigned int n_32k;
	unsigned int n_44k1;
	unsigned int n_48k;
} common_frl_n_table[] = {
	{ .r_bit = 3,  .n_32k = 4224, .n_44k1 = 5292, .n_48k = 5760, },
	{ .r_bit = 6,  .n_32k = 4032, .n_44k1 = 5292, .n_48k = 6048, },
	{ .r_bit = 8,  .n_32k = 4032, .n_44k1 = 3969, .n_48k = 6048, },
	{ .r_bit = 10, .n_32k = 3456, .n_44k1 = 3969, .n_48k = 5184, },
	{ .r_bit = 12, .n_32k = 3072, .n_44k1 = 3969, .n_48k = 4752, },
};

/*
 * Unless otherwise noted, entries in this table are 100% optimization.
 * Values can be obtained from dw_hdmi_qp_compute_n() but that function is
 * slow so we pre-compute values we expect to see.
 *
 * The values for TMDS 25175, 25200, 27000, 54000, 74250 and 148500 kHz are
 * the recommended N values specified in the Audio chapter of the HDMI
 * specification.
 */
static const struct dw_hdmi_audio_tmds_n {
	unsigned long tmds;
	unsigned int n_32k;
	unsigned int n_44k1;
	unsigned int n_48k;
} common_tmds_n_table[] = {
	{ .tmds = 25175000,  .n_32k = 4576,  .n_44k1 = 7007,  .n_48k = 6864, },
	{ .tmds = 25200000,  .n_32k = 4096,  .n_44k1 = 6272,  .n_48k = 6144, },
	{ .tmds = 27000000,  .n_32k = 4096,  .n_44k1 = 6272,  .n_48k = 6144, },
	{ .tmds = 28320000,  .n_32k = 4096,  .n_44k1 = 5586,  .n_48k = 6144, },
	{ .tmds = 30240000,  .n_32k = 4096,  .n_44k1 = 5642,  .n_48k = 6144, },
	{ .tmds = 31500000,  .n_32k = 4096,  .n_44k1 = 5600,  .n_48k = 6144, },
	{ .tmds = 32000000,  .n_32k = 4096,  .n_44k1 = 5733,  .n_48k = 6144, },
	{ .tmds = 33750000,  .n_32k = 4096,  .n_44k1 = 6272,  .n_48k = 6144, },
	{ .tmds = 36000000,  .n_32k = 4096,  .n_44k1 = 5684,  .n_48k = 6144, },
	{ .tmds = 40000000,  .n_32k = 4096,  .n_44k1 = 5733,  .n_48k = 6144, },
	{ .tmds = 49500000,  .n_32k = 4096,  .n_44k1 = 5488,  .n_48k = 6144, },
	{ .tmds = 50000000,  .n_32k = 4096,  .n_44k1 = 5292,  .n_48k = 6144, },
	{ .tmds = 54000000,  .n_32k = 4096,  .n_44k1 = 6272,  .n_48k = 6144, },
	{ .tmds = 65000000,  .n_32k = 4096,  .n_44k1 = 7056,  .n_48k = 6144, },
	{ .tmds = 68250000,  .n_32k = 4096,  .n_44k1 = 5376,  .n_48k = 6144, },
	{ .tmds = 71000000,  .n_32k = 4096,  .n_44k1 = 7056,  .n_48k = 6144, },
	{ .tmds = 72000000,  .n_32k = 4096,  .n_44k1 = 5635,  .n_48k = 6144, },
	{ .tmds = 73250000,  .n_32k = 11648, .n_44k1 = 14112, .n_48k = 6144, },
	{ .tmds = 74250000,  .n_32k = 4096,  .n_44k1 = 6272,  .n_48k = 6144, },
	{ .tmds = 75000000,  .n_32k = 4096,  .n_44k1 = 5880,  .n_48k = 6144, },
	{ .tmds = 78750000,  .n_32k = 4096,  .n_44k1 = 5600,  .n_48k = 6144, },
	{ .tmds = 78800000,  .n_32k = 4096,  .n_44k1 = 5292,  .n_48k = 6144, },
	{ .tmds = 79500000,  .n_32k = 4096,  .n_44k1 = 4704,  .n_48k = 6144, },
	{ .tmds = 83500000,  .n_32k = 4096,  .n_44k1 = 7056,  .n_48k = 6144, },
	{ .tmds = 85500000,  .n_32k = 4096,  .n_44k1 = 5488,  .n_48k = 6144, },
	{ .tmds = 88750000,  .n_32k = 4096,  .n_44k1 = 14112, .n_48k = 6144, },
	{ .tmds = 97750000,  .n_32k = 4096,  .n_44k1 = 14112, .n_48k = 6144, },
	{ .tmds = 101000000, .n_32k = 4096,  .n_44k1 = 7056,  .n_48k = 6144, },
	{ .tmds = 106500000, .n_32k = 4096,  .n_44k1 = 4704,  .n_48k = 6144, },
	{ .tmds = 108000000, .n_32k = 4096,  .n_44k1 = 5684,  .n_48k = 6144, },
	{ .tmds = 115500000, .n_32k = 4096,  .n_44k1 = 5712,  .n_48k = 6144, },
	{ .tmds = 119000000, .n_32k = 4096,  .n_44k1 = 5544,  .n_48k = 6144, },
	{ .tmds = 135000000, .n_32k = 4096,  .n_44k1 = 5488,  .n_48k = 6144, },
	{ .tmds = 146250000, .n_32k = 11648, .n_44k1 = 6272,  .n_48k = 6144, },
	{ .tmds = 148500000, .n_32k = 4096,  .n_44k1 = 6272,  .n_48k = 6144, },
	{ .tmds = 154000000, .n_32k = 4096,  .n_44k1 = 5544,  .n_48k = 6144, },
	{ .tmds = 162000000, .n_32k = 4096,  .n_44k1 = 5684,  .n_48k = 6144, },

	/* For 297 MHz+ HDMI spec have some other rule for setting N */
	{ .tmds = 297000000, .n_32k = 3073,  .n_44k1 = 4704,  .n_48k = 5120, },
	{ .tmds = 594000000, .n_32k = 3073,  .n_44k1 = 9408,  .n_48k = 10240,},
};

/*
 * These are the CTS values as recommended in the Audio chapter of the HDMI
 * specification.
 */
static const struct dw_hdmi_audio_tmds_cts {
	unsigned long tmds;
	unsigned int cts_32k;
	unsigned int cts_44k1;
	unsigned int cts_48k;
} common_tmds_cts_table[] = {
	{ .tmds = 25175000,  .cts_32k = 28125,  .cts_44k1 = 31250,  .cts_48k = 28125,  },
	{ .tmds = 25200000,  .cts_32k = 25200,  .cts_44k1 = 28000,  .cts_48k = 25200,  },
	{ .tmds = 27000000,  .cts_32k = 27000,  .cts_44k1 = 30000,  .cts_48k = 27000,  },
	{ .tmds = 54000000,  .cts_32k = 54000,  .cts_44k1 = 60000,  .cts_48k = 54000,  },
	{ .tmds = 74250000,  .cts_32k = 74250,  .cts_44k1 = 82500,  .cts_48k = 74250,  },
	{ .tmds = 148500000, .cts_32k = 148500, .cts_44k1 = 165000, .cts_48k = 148500, },
};

struct dw_hdmi_qp_i2c {
	struct i2c_adapter	adap;

	struct mutex		lock;	/* used to serialize data transfers */
	struct completion	cmp;
	u8			stat;

	u8			slave_reg;
	bool			is_regaddr;
	bool			is_segment;
};

#ifdef CONFIG_DRM_DW_HDMI_QP_CEC
struct dw_hdmi_qp_cec {
	struct drm_connector *connector;
	int irq;
	u32 addresses;
	struct cec_msg rx_msg;
	u8 tx_status;
	bool tx_done;
	bool rx_done;
};
#endif

struct dw_hdmi_qp {
	struct drm_bridge bridge;

	struct device *dev;
	struct dw_hdmi_qp_i2c *i2c;

#ifdef CONFIG_DRM_DW_HDMI_QP_CEC
	struct dw_hdmi_qp_cec *cec;
#endif

	struct {
		const struct dw_hdmi_qp_phy_ops *ops;
		void *data;
	} phy;

	unsigned long ref_clk_rate;

	struct drm_connector *curr_conn;
	struct delayed_work scramb_work;
	bool scramb_enabled;

	struct work_struct flt_work;
	bool flt_no_timeout;

	struct regmap *regm;
	int main_irq;

	unsigned long long tmds_char_rate;
	bool no_hpd;
};

static void dw_hdmi_qp_write(struct dw_hdmi_qp *hdmi, unsigned int val,
			     int offset)
{
	regmap_write(hdmi->regm, offset, val);
}

static unsigned int dw_hdmi_qp_read(struct dw_hdmi_qp *hdmi, int offset)
{
	unsigned int val = 0;

	regmap_read(hdmi->regm, offset, &val);

	return val;
}

static void dw_hdmi_qp_mod(struct dw_hdmi_qp *hdmi, unsigned int data,
			   unsigned int mask, unsigned int reg)
{
	regmap_update_bits(hdmi->regm, reg, mask, data);
}

static struct dw_hdmi_qp *dw_hdmi_qp_from_bridge(struct drm_bridge *bridge)
{
	return container_of(bridge, struct dw_hdmi_qp, bridge);
}

static void dw_hdmi_qp_set_cts_n(struct dw_hdmi_qp *hdmi, unsigned int cts,
				 unsigned int n)
{
	/* Set N */
	dw_hdmi_qp_mod(hdmi, n, AUDPKT_ACR_N_VALUE, AUDPKT_ACR_CONTROL0);

	/* Set CTS */
	if (cts)
		dw_hdmi_qp_mod(hdmi, AUDPKT_ACR_CTS_OVR_EN, AUDPKT_ACR_CTS_OVR_EN_MSK,
			       AUDPKT_ACR_CONTROL1);
	else
		dw_hdmi_qp_mod(hdmi, 0, AUDPKT_ACR_CTS_OVR_EN_MSK,
			       AUDPKT_ACR_CONTROL1);

	dw_hdmi_qp_mod(hdmi, AUDPKT_ACR_CTS_OVR_VAL(cts), AUDPKT_ACR_CTS_OVR_VAL_MSK,
		       AUDPKT_ACR_CONTROL1);
}

static int dw_hdmi_qp_match_frl_n_table(struct dw_hdmi_qp *hdmi,
					unsigned long r_bit,
					unsigned long freq)
{
	const struct dw_hdmi_qp_audio_frl_n *frl_n = NULL;
	int i, n;

	for (i = 0; i < ARRAY_SIZE(common_frl_n_table); i++) {
		if (r_bit == common_frl_n_table[i].r_bit) {
			frl_n = &common_frl_n_table[i];
			break;
		}
	}

	if (!frl_n) {
		dev_err(hdmi->dev, "Unexpected FRL Rbit: %lu Gbps\n", r_bit);
		return 0;
	}

	switch (freq) {
	case 32000:
	case 64000:
	case 128000:
		n = (freq / 32000) * frl_n->n_32k;
		break;
	case 44100:
	case 88200:
	case 176400:
		n = (freq / 44100) * frl_n->n_44k1;
		break;
	case 48000:
	case 96000:
	case 192000:
		n = (freq / 48000) * frl_n->n_48k;
		break;
	default:
		dev_err(hdmi->dev, "Unexpected FRL freq: %lu Hz\n", freq);
		n = 0;
	}

	return n;
}

static int dw_hdmi_qp_match_tmds_n_table(struct dw_hdmi_qp *hdmi,
					 unsigned long pixel_clk,
					 unsigned long freq)
{
	const struct dw_hdmi_audio_tmds_n *tmds_n = NULL;
	int i;

	for (i = 0; i < ARRAY_SIZE(common_tmds_n_table); i++) {
		if (pixel_clk == common_tmds_n_table[i].tmds) {
			tmds_n = &common_tmds_n_table[i];
			break;
		}
	}

	if (!tmds_n)
		return -ENOENT;

	switch (freq) {
	case 32000:
		return tmds_n->n_32k;
	case 44100:
	case 88200:
	case 176400:
		return (freq / 44100) * tmds_n->n_44k1;
	case 48000:
	case 96000:
	case 192000:
		return (freq / 48000) * tmds_n->n_48k;
	default:
		return -ENOENT;
	}
}

static u32 dw_hdmi_qp_audio_math_diff(unsigned int freq, unsigned int n,
				      unsigned int pixel_clk)
{
	u64 cts = mul_u32_u32(pixel_clk, n);

	return do_div(cts, 128 * freq);
}

static unsigned int dw_hdmi_qp_compute_n(struct dw_hdmi_qp *hdmi,
					 unsigned long pixel_clk,
					 unsigned long freq)
{
	unsigned int min_n = DIV_ROUND_UP((128 * freq), 1500);
	unsigned int max_n = (128 * freq) / 300;
	unsigned int ideal_n = (128 * freq) / 1000;
	unsigned int best_n_distance = ideal_n;
	unsigned int best_n = 0;
	u64 best_diff = U64_MAX;
	int n;

	/* If the ideal N could satisfy the audio math, then just take it */
	if (dw_hdmi_qp_audio_math_diff(freq, ideal_n, pixel_clk) == 0)
		return ideal_n;

	for (n = min_n; n <= max_n; n++) {
		u64 diff = dw_hdmi_qp_audio_math_diff(freq, n, pixel_clk);

		if (diff < best_diff ||
		    (diff == best_diff && abs(n - ideal_n) < best_n_distance)) {
			best_n = n;
			best_diff = diff;
			best_n_distance = abs(best_n - ideal_n);
		}

		/*
		 * The best N already satisfy the audio math, and also be
		 * the closest value to ideal N, so just cut the loop.
		 */
		if (best_diff == 0 && (abs(n - ideal_n) > best_n_distance))
			break;
	}

	return best_n;
}

static unsigned int dw_hdmi_qp_find_n(struct dw_hdmi_qp *hdmi, unsigned long pixel_clk,
				      unsigned long sample_rate)
{
	const struct dw_hdmi_qp_link_cfg *link_cfg;
	int ret;

	link_cfg = hdmi->phy.ops->get_link_cfg(hdmi, hdmi->phy.data);
	if (link_cfg->frl_enabled)
		return dw_hdmi_qp_match_frl_n_table(hdmi,
						    link_cfg->frl_rate_per_lane,
						    sample_rate);

	ret = dw_hdmi_qp_match_tmds_n_table(hdmi, pixel_clk, sample_rate);
	if (ret > 0)
		return ret;

	dev_warn(hdmi->dev, "Rate %lu missing; compute N dynamically\n",
		 pixel_clk);

	return dw_hdmi_qp_compute_n(hdmi, pixel_clk, sample_rate);
}

static unsigned int dw_hdmi_qp_find_cts(struct dw_hdmi_qp *hdmi, unsigned long pixel_clk,
					unsigned long sample_rate)
{
	const struct dw_hdmi_audio_tmds_cts *tmds_cts = NULL;
	int i;

	for (i = 0; i < ARRAY_SIZE(common_tmds_cts_table); i++) {
		if (pixel_clk == common_tmds_cts_table[i].tmds) {
			tmds_cts = &common_tmds_cts_table[i];
			break;
		}
	}

	if (!tmds_cts)
		return 0;

	switch (sample_rate) {
	case 32000:
		return tmds_cts->cts_32k;
	case 44100:
	case 88200:
	case 176400:
		return tmds_cts->cts_44k1;
	case 48000:
	case 96000:
	case 192000:
		return tmds_cts->cts_48k;
	default:
		return -ENOENT;
	}
}

static void dw_hdmi_qp_set_audio_interface(struct dw_hdmi_qp *hdmi,
					   struct hdmi_codec_daifmt *fmt,
					   struct hdmi_codec_params *hparms)
{
	u32 conf0 = 0;

	/* Reset the audio data path of the AVP */
	dw_hdmi_qp_write(hdmi, AVP_DATAPATH_PACKET_AUDIO_SWINIT_P, GLOBAL_SWRESET_REQUEST);

	/* Disable AUDS, ACR, AUDI */
	dw_hdmi_qp_mod(hdmi, 0,
		       PKTSCHED_ACR_TX_EN | PKTSCHED_AUDS_TX_EN | PKTSCHED_AUDI_TX_EN,
		       PKTSCHED_PKT_EN);

	/* Clear the audio FIFO */
	dw_hdmi_qp_write(hdmi, AUDIO_FIFO_CLR_P, AUDIO_INTERFACE_CONTROL0);

	/* Select I2S interface as the audio source */
	dw_hdmi_qp_mod(hdmi, AUD_IF_I2S, AUD_IF_SEL_MSK, AUDIO_INTERFACE_CONFIG0);

	/* Enable the active i2s lanes */
	switch (hparms->channels) {
	case 7 ... 8:
		conf0 |= I2S_LINES_EN(3);
		fallthrough;
	case 5 ... 6:
		conf0 |= I2S_LINES_EN(2);
		fallthrough;
	case 3 ... 4:
		conf0 |= I2S_LINES_EN(1);
		fallthrough;
	default:
		conf0 |= I2S_LINES_EN(0);
		break;
	}

	dw_hdmi_qp_mod(hdmi, conf0, I2S_LINES_EN_MSK, AUDIO_INTERFACE_CONFIG0);

	/*
	 * Enable bpcuv generated internally for L-PCM, or received
	 * from stream for NLPCM/HBR.
	 */
	switch (fmt->bit_fmt) {
	case SNDRV_PCM_FORMAT_IEC958_SUBFRAME_LE:
		conf0 = (hparms->channels == 8) ? AUD_HBR : AUD_ASP;
		conf0 |= I2S_BPCUV_RCV_EN;
		break;
	default:
		conf0 = AUD_ASP | I2S_BPCUV_RCV_DIS;
		break;
	}

	dw_hdmi_qp_mod(hdmi, conf0, I2S_BPCUV_RCV_MSK | AUD_FORMAT_MSK,
		       AUDIO_INTERFACE_CONFIG0);

	/* Enable audio FIFO auto clear when overflow */
	dw_hdmi_qp_mod(hdmi, AUD_FIFO_INIT_ON_OVF_EN, AUD_FIFO_INIT_ON_OVF_MSK,
		       AUDIO_INTERFACE_CONFIG0);
}

/*
 * When transmitting IEC60958 linear PCM audio, these registers allow to
 * configure the channel status information of all the channel status
 * bits in the IEC60958 frame. For the moment this configuration is only
 * used when the I2S audio interface, General Purpose Audio (GPA),
 * or AHB audio DMA (AHBAUDDMA) interface is active
 * (for S/PDIF interface this information comes from the stream).
 */
static void dw_hdmi_qp_set_channel_status(struct dw_hdmi_qp *hdmi,
					  u8 *channel_status, bool ref2stream)
{
	/*
	 * AUDPKT_CHSTATUS_OVR0: { RSV, RSV, CS1, CS0 }
	 * AUDPKT_CHSTATUS_OVR1: { CS6, CS5, CS4, CS3 }
	 *
	 *      |  7  |  6  |  5  |  4  |  3  |  2  |  1  |  0  |
	 * CS0: |   Mode    |        d        |  c  |  b  |  a  |
	 * CS1: |               Category Code                   |
	 * CS2: |    Channel Number     |     Source Number     |
	 * CS3: |    Clock Accuracy     |     Sample Freq       |
	 * CS4: |    Ori Sample Freq    |     Word Length       |
	 * CS5: |                                   |   CGMS-A  |
	 * CS6~CS23: Reserved
	 *
	 * a: use of channel status block
	 * b: linear PCM identification: 0 for lpcm, 1 for nlpcm
	 * c: copyright information
	 * d: additional format information
	 */

	if (ref2stream)
		channel_status[0] |= IEC958_AES0_NONAUDIO;

	if ((dw_hdmi_qp_read(hdmi, AUDIO_INTERFACE_CONFIG0) & GENMASK(25, 24)) == AUD_HBR) {
		/* fixup cs for HBR */
		channel_status[3] = (channel_status[3] & 0xf0) | IEC958_AES3_CON_FS_768000;
		channel_status[4] = (channel_status[4] & 0x0f) | IEC958_AES4_CON_ORIGFS_NOTID;
	}

	dw_hdmi_qp_write(hdmi, channel_status[0] | (channel_status[1] << 8),
			 AUDPKT_CHSTATUS_OVR0);

	regmap_bulk_write(hdmi->regm, AUDPKT_CHSTATUS_OVR1, &channel_status[3], 1);

	if (ref2stream)
		dw_hdmi_qp_mod(hdmi, 0,
			       AUDPKT_PBIT_FORCE_EN_MASK | AUDPKT_CHSTATUS_OVR_EN_MASK,
			       AUDPKT_CONTROL0);
	else
		dw_hdmi_qp_mod(hdmi, AUDPKT_PBIT_FORCE_EN | AUDPKT_CHSTATUS_OVR_EN,
			       AUDPKT_PBIT_FORCE_EN_MASK | AUDPKT_CHSTATUS_OVR_EN_MASK,
			       AUDPKT_CONTROL0);
}

static void dw_hdmi_qp_set_sample_rate(struct dw_hdmi_qp *hdmi, unsigned long long tmds_char_rate,
				       unsigned int sample_rate)
{
	unsigned int n, cts;

	n = dw_hdmi_qp_find_n(hdmi, tmds_char_rate, sample_rate);
	cts = dw_hdmi_qp_find_cts(hdmi, tmds_char_rate, sample_rate);

	dw_hdmi_qp_set_cts_n(hdmi, cts, n);
}

static int dw_hdmi_qp_audio_enable(struct drm_bridge *bridge,
				   struct drm_connector *connector)
{
	struct dw_hdmi_qp *hdmi = dw_hdmi_qp_from_bridge(bridge);

	if (!hdmi->tmds_char_rate)
		return -EOPNOTSUPP;

	dw_hdmi_qp_mod(hdmi, 0, AVP_DATAPATH_PACKET_AUDIO_SWDISABLE, GLOBAL_SWDISABLE);

	return 0;
}

static int dw_hdmi_qp_audio_prepare(struct drm_bridge *bridge,
				    struct drm_connector *connector,
				    struct hdmi_codec_daifmt *fmt,
				    struct hdmi_codec_params *hparms)
{
	struct dw_hdmi_qp *hdmi = dw_hdmi_qp_from_bridge(bridge);
	bool ref2stream = false;

	if (!hdmi->tmds_char_rate)
		return -EOPNOTSUPP;

	if (fmt->bit_clk_provider | fmt->frame_clk_provider) {
		dev_err(hdmi->dev, "unsupported clock settings\n");
		return -EINVAL;
	}

	if (fmt->bit_fmt == SNDRV_PCM_FORMAT_IEC958_SUBFRAME_LE)
		ref2stream = true;

	dw_hdmi_qp_set_audio_interface(hdmi, fmt, hparms);
	dw_hdmi_qp_set_sample_rate(hdmi, hdmi->tmds_char_rate, hparms->sample_rate);
	dw_hdmi_qp_set_channel_status(hdmi, hparms->iec.status, ref2stream);
	drm_atomic_helper_connector_hdmi_update_audio_infoframe(connector, &hparms->cea);

	return 0;
}

static void dw_hdmi_qp_audio_disable_regs(struct dw_hdmi_qp *hdmi)
{
	/*
	 * Keep ACR, AUDI, AUDS packet always on to make SINK device
	 * active for better compatibility and user experience.
	 *
	 * This also fix POP sound on some SINK devices which wakeup
	 * from suspend to active.
	 */
	dw_hdmi_qp_mod(hdmi, I2S_BPCUV_RCV_DIS, I2S_BPCUV_RCV_MSK,
		       AUDIO_INTERFACE_CONFIG0);
	dw_hdmi_qp_mod(hdmi, AUDPKT_PBIT_FORCE_EN | AUDPKT_CHSTATUS_OVR_EN,
		       AUDPKT_PBIT_FORCE_EN_MASK | AUDPKT_CHSTATUS_OVR_EN_MASK,
		       AUDPKT_CONTROL0);

	dw_hdmi_qp_mod(hdmi, AVP_DATAPATH_PACKET_AUDIO_SWDISABLE,
		       AVP_DATAPATH_PACKET_AUDIO_SWDISABLE, GLOBAL_SWDISABLE);
}

static void dw_hdmi_qp_audio_disable(struct drm_bridge *bridge,
				     struct drm_connector *connector)
{
	struct dw_hdmi_qp *hdmi = dw_hdmi_qp_from_bridge(bridge);

	drm_atomic_helper_connector_hdmi_clear_audio_infoframe(connector);

	if (hdmi->tmds_char_rate)
		dw_hdmi_qp_audio_disable_regs(hdmi);
}

static int dw_hdmi_qp_i2c_read(struct dw_hdmi_qp *hdmi,
			       unsigned char *buf, unsigned int length)
{
	struct dw_hdmi_qp_i2c *i2c = hdmi->i2c;
	int stat;

	if (!i2c->is_regaddr) {
		dev_dbg(hdmi->dev, "set read register address to 0\n");
		i2c->slave_reg = 0x00;
		i2c->is_regaddr = true;
	}

	while (length--) {
		reinit_completion(&i2c->cmp);

		dw_hdmi_qp_mod(hdmi, i2c->slave_reg++ << 12, I2CM_ADDR,
			       I2CM_INTERFACE_CONTROL0);

		if (i2c->is_segment)
			dw_hdmi_qp_mod(hdmi, I2CM_EXT_READ, I2CM_WR_MASK,
				       I2CM_INTERFACE_CONTROL0);
		else
			dw_hdmi_qp_mod(hdmi, I2CM_FM_READ, I2CM_WR_MASK,
				       I2CM_INTERFACE_CONTROL0);

		stat = wait_for_completion_timeout(&i2c->cmp, HZ / 10);
		if (!stat) {
			if (hdmi->no_hpd)
				dev_dbg_ratelimited(hdmi->dev,
						    "i2c read timed out\n");
			else
				dev_err_ratelimited(hdmi->dev, "i2c read timed out\n");
			dw_hdmi_qp_write(hdmi, 0x01, I2CM_CONTROL0);
			return -EAGAIN;
		}

		/* Check for error condition on the bus */
		if (i2c->stat & I2CM_NACK_RCVD_IRQ) {
			if (hdmi->no_hpd)
				dev_dbg_ratelimited(hdmi->dev,
						    "i2c read error\n");
			else
				dev_err_ratelimited(hdmi->dev, "i2c read error\n");
			dw_hdmi_qp_write(hdmi, 0x01, I2CM_CONTROL0);
			return -EIO;
		}

		*buf++ = dw_hdmi_qp_read(hdmi, I2CM_INTERFACE_RDDATA_0_3) & 0xff;
		dw_hdmi_qp_mod(hdmi, 0, I2CM_WR_MASK, I2CM_INTERFACE_CONTROL0);
	}

	i2c->is_segment = false;

	return 0;
}

static int dw_hdmi_qp_i2c_write(struct dw_hdmi_qp *hdmi,
				unsigned char *buf, unsigned int length)
{
	struct dw_hdmi_qp_i2c *i2c = hdmi->i2c;
	int stat;

	if (!i2c->is_regaddr) {
		/* Use the first write byte as register address */
		i2c->slave_reg = buf[0];
		length--;
		buf++;
		i2c->is_regaddr = true;
	}

	while (length--) {
		reinit_completion(&i2c->cmp);

		dw_hdmi_qp_write(hdmi, *buf++, I2CM_INTERFACE_WRDATA_0_3);
		dw_hdmi_qp_mod(hdmi, i2c->slave_reg++ << 12, I2CM_ADDR,
			       I2CM_INTERFACE_CONTROL0);
		dw_hdmi_qp_mod(hdmi, I2CM_FM_WRITE, I2CM_WR_MASK,
			       I2CM_INTERFACE_CONTROL0);

		stat = wait_for_completion_timeout(&i2c->cmp, HZ / 10);
		if (!stat) {
			dev_err(hdmi->dev, "i2c write time out!\n");
			dw_hdmi_qp_write(hdmi, 0x01, I2CM_CONTROL0);
			return -EAGAIN;
		}

		/* Check for error condition on the bus */
		if (i2c->stat & I2CM_NACK_RCVD_IRQ) {
			dev_err(hdmi->dev, "i2c write nack!\n");
			dw_hdmi_qp_write(hdmi, 0x01, I2CM_CONTROL0);
			return -EIO;
		}

		dw_hdmi_qp_mod(hdmi, 0, I2CM_WR_MASK, I2CM_INTERFACE_CONTROL0);
	}

	return 0;
}

static int dw_hdmi_qp_i2c_xfer(struct i2c_adapter *adap,
			       struct i2c_msg *msgs, int num)
{
	struct dw_hdmi_qp *hdmi = i2c_get_adapdata(adap);
	struct dw_hdmi_qp_i2c *i2c = hdmi->i2c;
	u8 addr = msgs[0].addr;
	int i, ret = 0;

	if (addr == DDC_CI_ADDR)
		/*
		 * The internal I2C controller does not support the multi-byte
		 * read and write operations needed for DDC/CI.
		 * FIXME: Blacklist the DDC/CI address until we filter out
		 * unsupported I2C operations.
		 */
		return -EOPNOTSUPP;

	for (i = 0; i < num; i++) {
		if (msgs[i].len == 0) {
			dev_err(hdmi->dev,
				"unsupported transfer %d/%d, no data\n",
				i + 1, num);
			return -EOPNOTSUPP;
		}
	}

	guard(mutex)(&i2c->lock);

	/* Unmute DONE and ERROR interrupts */
	dw_hdmi_qp_mod(hdmi, I2CM_NACK_RCVD_MASK_N | I2CM_OP_DONE_MASK_N,
		       I2CM_NACK_RCVD_MASK_N | I2CM_OP_DONE_MASK_N,
		       MAINUNIT_1_INT_MASK_N);

	/* Set slave device address taken from the first I2C message */
	if (addr == DDC_SEGMENT_ADDR && msgs[0].len == 1)
		addr = DDC_ADDR;

	dw_hdmi_qp_mod(hdmi, addr << 5, I2CM_SLVADDR, I2CM_INTERFACE_CONTROL0);

	/* Set slave device register address on transfer */
	i2c->is_regaddr = false;

	/* Set segment pointer for I2C extended read mode operation */
	i2c->is_segment = false;

	for (i = 0; i < num; i++) {
		if (msgs[i].addr == DDC_SEGMENT_ADDR && msgs[i].len == 1) {
			i2c->is_segment = true;
			dw_hdmi_qp_mod(hdmi, DDC_SEGMENT_ADDR, I2CM_SEG_ADDR,
				       I2CM_INTERFACE_CONTROL1);
			dw_hdmi_qp_mod(hdmi, *msgs[i].buf << 7, I2CM_SEG_PTR,
				       I2CM_INTERFACE_CONTROL1);
		} else {
			if (msgs[i].flags & I2C_M_RD)
				ret = dw_hdmi_qp_i2c_read(hdmi, msgs[i].buf,
							  msgs[i].len);
			else
				ret = dw_hdmi_qp_i2c_write(hdmi, msgs[i].buf,
							   msgs[i].len);
		}
		if (ret < 0)
			break;
	}

	if (!ret)
		ret = num;

	/* Mute DONE and ERROR interrupts */
	dw_hdmi_qp_mod(hdmi, 0, I2CM_OP_DONE_MASK_N | I2CM_NACK_RCVD_MASK_N,
		       MAINUNIT_1_INT_MASK_N);

	return ret;
}

static u32 dw_hdmi_qp_i2c_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm dw_hdmi_qp_algorithm = {
	.master_xfer	= dw_hdmi_qp_i2c_xfer,
	.functionality	= dw_hdmi_qp_i2c_func,
};

static struct i2c_adapter *dw_hdmi_qp_i2c_adapter(struct dw_hdmi_qp *hdmi)
{
	struct dw_hdmi_qp_i2c *i2c;
	struct i2c_adapter *adap;
	int ret;

	i2c = devm_kzalloc(hdmi->dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return ERR_PTR(-ENOMEM);

	mutex_init(&i2c->lock);
	init_completion(&i2c->cmp);

	adap = &i2c->adap;
	adap->owner = THIS_MODULE;
	adap->dev.parent = hdmi->dev;
	adap->algo = &dw_hdmi_qp_algorithm;
	strscpy(adap->name, "DesignWare HDMI QP", sizeof(adap->name));

	i2c_set_adapdata(adap, hdmi);

	ret = devm_i2c_add_adapter(hdmi->dev, adap);
	if (ret) {
		dev_warn(hdmi->dev, "cannot add %s I2C adapter\n", adap->name);
		devm_kfree(hdmi->dev, i2c);
		return ERR_PTR(ret);
	}

	hdmi->i2c = i2c;
	dev_info(hdmi->dev, "registered %s I2C bus driver\n", adap->name);

	return adap;
}

enum dw_hdmi_qp_frl_lts {
	LTS1,	/* Read EDID */
	LTS2,	/* Prepare for FRL */
	LTS3,	/* Training in progress */
	LTS4,	/* Update FRL rate */
	LTSP,	/* Training passed */
	LTSL,	/* Legacy TMDS */
	LTSU,	/* Undefined */
};

/*
 * Check sink version and FLT no-timeout mode.
 */
static int dw_hdmi_qp_frl_lts1(struct dw_hdmi_qp *hdmi)
{
	int ret;
	u8 val;

	if (!hdmi->tmds_char_rate) {
		dev_dbg(hdmi->dev, "lts1: hdmi disabled\n");
		return LTSL;
	}

	dw_hdmi_qp_mod(hdmi, AVP_DATAPATH_VIDEO_SWDISABLE,
		       AVP_DATAPATH_VIDEO_SWDISABLE, GLOBAL_SWDISABLE);

	/* Reset AVP data path */
	dw_hdmi_qp_write(hdmi, AVP_DATAPATH_SWINIT_P, GLOBAL_SWRESET_REQUEST);

	ret = drm_scdc_readb(hdmi->bridge.ddc, SCDC_SINK_VERSION, &val);
	if (ret) {
		dev_err(hdmi->dev, "lts1: SCDC read failed\n");
		return LTSL;
	}

	if (!val) {
		dev_warn(hdmi->dev, "lts1: SCDC sink version is zero\n");
		return LTSL;
	}

	ret = drm_scdc_writeb(hdmi->bridge.ddc, SCDC_SOURCE_VERSION, 1);
	if (ret) {
		dev_err(hdmi->dev, "lts1: SCDC write failed\n");
		return LTSL;
	}

	ret = drm_scdc_readb(hdmi->bridge.ddc, SCDC_SOURCE_TEST_CONFIG, &val);
	if (ret) {
		dev_err(hdmi->dev, "lts1: SCDC read failed\n");
		return LTSL;
	}

	hdmi->flt_no_timeout = !!(val & SCDC_FLT_NO_TIMEOUT);
	dev_dbg(hdmi->dev, "lts1: flt_no_timeout=%d\n", hdmi->flt_no_timeout);

	return LTS2;
}

/*
 * Check if sink is ready to training. Set FRL rate & max FFE level.
 */
static int dw_hdmi_qp_frl_lts2(struct dw_hdmi_qp *hdmi)
{
	const struct dw_hdmi_qp_link_cfg *link_cfg;
	int ret, i;
	u8 val;

	for (i = 0; i < 20; i++) {
		ret = drm_scdc_readb(hdmi->bridge.ddc, SCDC_STATUS_FLAGS_0, &val);
		if (ret) {
			dev_err(hdmi->dev, "lts2: SCDC read failed\n");
			return LTSL;
		}

		if (val & SCDC_FLT_READY) {
			link_cfg = hdmi->phy.ops->get_link_cfg(hdmi, hdmi->phy.data);
			val = hdmi->phy.ops->set_ffe_level ? link_cfg->max_ffe_level : 0;

			dev_dbg(hdmi->dev, "lts2: set rate=%ux%u maxffe=%u\n",
				link_cfg->frl_rate_per_lane, link_cfg->frl_lanes, val);

			ret = drm_scdc_set_frl(hdmi->curr_conn, link_cfg->frl_rate_per_lane,
					       link_cfg->frl_lanes, val);
			if (ret)
				ret = drm_scdc_writeb(hdmi->bridge.ddc, SCDC_CONFIG_0, 0);
			else
				ret = -EIO;

			if (ret) {
				dev_err(hdmi->dev, "lts2: SCDC write failed\n");
				return LTSL;
			}

			return LTS3;
		}

		msleep(20);
	}

	dev_err(hdmi->dev, "lts2: sink flt not ready\n");
	return LTSL;
}

/*
 * Conduct link training for the specified FRL rate.
 */
static int dw_hdmi_qp_frl_lts3(struct dw_hdmi_qp *hdmi)
{
	u8 val, ffe_lv = 0;
	int i, ret;

	/* Set 2s timeout */
	i = 4000;

	while (i-- > 0 || hdmi->flt_no_timeout) {
		/* Poll FLT_update flag every 2 ms or less */
		usleep_range(400, 500);

		if (!hdmi->tmds_char_rate) {
			dev_dbg(hdmi->dev, "lts3: hdmi disabled\n");
			return LTSL;
		}

		ret = drm_scdc_readb(hdmi->bridge.ddc, SCDC_UPDATE_0, &val);
		if (ret) {
			dev_err(hdmi->dev, "lts3: SCDC read failed\n");
			return LTSL;
		}

		if (val & SCDC_SOURCE_TEST_UPDATE) {
			u8 test_cfg;

			ret = drm_scdc_readb(hdmi->bridge.ddc, SCDC_SOURCE_TEST_CONFIG,
					     &test_cfg);
			if (ret) {
				dev_err(hdmi->dev, "lts3: SCDC read failed\n");
				return LTSL;
			}

			if (hdmi->flt_no_timeout && !(test_cfg & SCDC_FLT_NO_TIMEOUT)) {
				dev_dbg(hdmi->dev, "lts3: exit test mode\n");
				hdmi->flt_no_timeout = false;
			} else if (!hdmi->flt_no_timeout && (test_cfg & SCDC_FLT_NO_TIMEOUT)) {
				dev_dbg(hdmi->dev, "lts3: enter test mode\n");
				hdmi->flt_no_timeout = true;
			}

			/* Clear SCDC_SOURCE_TEST_UPDATE flag */
			ret = drm_scdc_writeb(hdmi->bridge.ddc, SCDC_UPDATE_0,
					      SCDC_SOURCE_TEST_UPDATE);
			if (ret) {
				dev_err(hdmi->dev, "lts3: SCDC write failed\n");
				return LTSL;
			}
		}

		if (val & SCDC_FLT_UPDATE) {
			u8 ln0, ln1, ln2, ln3;
			u32 flt_cfg;

			ret = drm_scdc_get_frl_ltp_request(hdmi->curr_conn,
							   &ln0, &ln1, &ln2, &ln3);
			if (!ret) {
				dev_err(hdmi->dev, "lts3: SCDC read failed\n");
				return LTSL;
			}

			dev_dbg(hdmi->dev, "lts3: ln0=0x%x ln1=0x%x ln2=0x%x ln3=0x%x\n",
				ln0, ln1, ln2, ln3);

			if (!ln0 && !ln1 && !ln2 && !ln3) {
				dw_hdmi_qp_write(hdmi, 0, FLT_CONFIG1);
				return LTSP;
			}

			if (ln0 == 0xf && ln1 == 0xf && ln2 == 0xf && ln3 == 0xf) {
				dev_dbg(hdmi->dev, "lts3: rate change request\n");
				return LTS4;
			}

			if (ln0 == 0xe || ln1 == 0xe || ln2 == 0xe || ln3 == 0xe) {
				if (!hdmi->phy.ops->set_ffe_level) {
					dev_err(hdmi->dev, "lts3: ffe level update not expected\n");
					return LTSL;
				}

				if (ffe_lv >= 3) {
					dev_err(hdmi->dev, "lts3: ffe level limit reached\n");
					return LTSL;
				}

				ffe_lv++;
				dev_dbg(hdmi->dev, "lts3: ffe level up %d\n", ffe_lv);

				ret = hdmi->phy.ops->set_ffe_level(hdmi, hdmi->phy.data, ffe_lv);
				if (ret)
					return LTSL;
			} else {
				flt_cfg = (ln3 << 16) | (ln2 << 12) | (ln1 << 8) |
					  (ln0 << 4) | 0xf;

				/* Support HFR1-10; send old LTP if ln0..3 == 0x3 */
				if (!hdmi->flt_no_timeout && flt_cfg == 0x3333f)
					flt_cfg = dw_hdmi_qp_read(hdmi, FLT_CONFIG1);

				dw_hdmi_qp_write(hdmi, flt_cfg, FLT_CONFIG1);
			}

			/* Clear FLT_update flag */
			ret = drm_scdc_writeb(hdmi->bridge.ddc, SCDC_UPDATE_0,
					      SCDC_FLT_UPDATE);
			if (ret) {
				dev_err(hdmi->dev, "lts3: SCDC write failed\n");
				return LTSL;
			}
		}
	}

	dev_err(hdmi->dev, "lts3: timed out\n");
	return LTSL;
}

/*
 * Handle FRL rate change request from sink.
 */
static int dw_hdmi_qp_frl_lts4(struct dw_hdmi_qp *hdmi)
{
	const struct dw_hdmi_qp_link_cfg *link_cfg;
	u8 try_rate_per_lane, try_lanes, max_ffe_level;
	int ret;

	link_cfg = hdmi->phy.ops->get_link_cfg(hdmi, hdmi->phy.data);

	/* Choose a reduced bandwidth FRL rate */
	ret = drm_scdc_calc_lower_frl(link_cfg->frl_rate_per_lane, link_cfg->frl_lanes,
				      &try_rate_per_lane, &try_lanes);
	if (!ret) {
		dev_err(hdmi->dev, "lts4: failed to compute lower frl\n");
		return LTSL;
	}

	if (try_rate_per_lane < link_cfg->min_frl_rate_per_lane ||
	    try_lanes < link_cfg->min_frl_lanes) {
		dev_err(hdmi->dev, "lts4: unsupported %ux%u rate\n",
			try_rate_per_lane, try_lanes);
		return LTSL;
	}

	dev_dbg(hdmi->dev, "lts4: switching to %ux%u\n", try_rate_per_lane, try_lanes);

	ret = drm_scdc_writeb(hdmi->bridge.ddc, SCDC_UPDATE_0, SCDC_FLT_UPDATE);
	if (ret) {
		dev_err(hdmi->dev, "lts4: SCDC write failed\n");
		return LTSL;
	}

	/* Disable phy */
	hdmi->phy.ops->disable(hdmi, hdmi->phy.data);

	ret = hdmi->phy.ops->set_frl_rate(hdmi, hdmi->phy.data,
					  try_rate_per_lane, try_lanes);
	if (ret) {
		dev_err(hdmi->dev, "lts4: failed to set phy link rate\n");
		return LTSL;
	}

	/* Enable phy */
	hdmi->phy.ops->init(hdmi, hdmi->phy.data);

	max_ffe_level = hdmi->phy.ops->set_ffe_level ? link_cfg->max_ffe_level : 0;

	/* Set new rate */
	ret = drm_scdc_set_frl(hdmi->curr_conn, try_rate_per_lane, try_lanes,
			       max_ffe_level);
	if (ret)
		ret = drm_scdc_writeb(hdmi->bridge.ddc, SCDC_UPDATE_0,
				      SCDC_FLT_UPDATE);
	else
		ret = -EIO;

	if (ret) {
		dev_err(hdmi->dev, "lts4: SCDC write failed\n");
		return LTSL;
	}

	return LTS3;
}

/*
 * Training passed, wait for further changes.
 */
static int dw_hdmi_qp_frl_ltsp(struct dw_hdmi_qp *hdmi)
{
	int i, ret;
	u8 val;

	ret = drm_scdc_writeb(hdmi->bridge.ddc, SCDC_UPDATE_0, SCDC_FLT_UPDATE);
	if (ret) {
		dev_err(hdmi->dev, "ltsp: SCDC write failed\n");
		return LTSL;
	}

	/* Set 2s timeout */
	i = 4000;

	while (i--) {
		/* Poll Update Flags every 2 ms or less */
		usleep_range(400, 500);

		if (!hdmi->tmds_char_rate) {
			dev_dbg(hdmi->dev, "ltsp: hdmi disabled\n");
			return LTSL;
		}

		ret = drm_scdc_readb(hdmi->bridge.ddc, SCDC_UPDATE_0, &val);
		if (ret) {
			dev_err(hdmi->dev, "ltsp: SCDC read failed\n");
			return LTSL;
		}

		if (val & SCDC_FRL_START) {
			dw_hdmi_qp_mod(hdmi, 0, AVP_DATAPATH_VIDEO_SWDISABLE,
				       GLOBAL_SWDISABLE);

			ret = drm_scdc_writeb(hdmi->bridge.ddc, SCDC_UPDATE_0,
					      SCDC_FRL_START);
			if (ret) {
				dev_err(hdmi->dev, "ltsp: SCDC write failed\n");
				return LTSL;
			}

			dw_hdmi_qp_write(hdmi, PKTSCHED_GCP_CLEAR_AVMUTE,
					 PKTSCHED_PKT_CONTROL0);
			dw_hdmi_qp_mod(hdmi, PKTSCHED_GCP_TX_EN, PKTSCHED_GCP_TX_EN,
				       PKTSCHED_PKT_EN);

			dev_dbg(hdmi->dev, "ltsp: flt success\n");
			break;
		}

		if (val & SCDC_FLT_UPDATE) {
			dw_hdmi_qp_mod(hdmi, AVP_DATAPATH_VIDEO_SWDISABLE,
				       AVP_DATAPATH_VIDEO_SWDISABLE, GLOBAL_SWDISABLE);

			ret = drm_scdc_writeb(hdmi->bridge.ddc, SCDC_UPDATE_0,
					      SCDC_FLT_UPDATE);
			if (ret) {
				dev_err(hdmi->dev, "ltsp: SCDC write failed\n");
				return LTSL;
			}

			return LTS3;
		}
	}

	if (i < 0) {
		dev_err(hdmi->dev, "ltsp: timed out\n");
		return LTSL;
	}

	/* Ensure FLT_update flag is polled at least once every 250 ms. */
	i = 5;

	while (true) {
		msleep(20);

		if (!hdmi->tmds_char_rate) {
			dev_dbg(hdmi->dev, "ltsp: hdmi disabled\n");
			break;
		}

		if (i) {
			i--;
			continue;
		}

		ret = drm_scdc_readb(hdmi->bridge.ddc, SCDC_UPDATE_0, &val);
		if (ret) {
			dev_err(hdmi->dev, "ltsp: SCDC read failed\n");
			break;
		}

		if (val & SCDC_FLT_UPDATE) {
			dw_hdmi_qp_write(hdmi, PKTSCHED_GCP_SET_AVMUTE,
					 PKTSCHED_PKT_CONTROL0);
			dw_hdmi_qp_mod(hdmi, PKTSCHED_GCP_TX_EN, PKTSCHED_GCP_TX_EN,
				       PKTSCHED_PKT_EN);

			msleep(50);
			dw_hdmi_qp_mod(hdmi, AVP_DATAPATH_VIDEO_SWDISABLE,
				       AVP_DATAPATH_VIDEO_SWDISABLE, GLOBAL_SWDISABLE);

			ret = drm_scdc_writeb(hdmi->bridge.ddc, SCDC_UPDATE_0,
					      SCDC_FLT_UPDATE);
			if (ret) {
				dev_err(hdmi->dev, "ltsp: SCDC write failed\n");
				break;
			}

			return LTS2;
		}

		i = 5;
	}

	return LTSL;
}

/*
 * Exit frl mode, i.e. for training failures or hdmi disabled.
 */
static int dw_hdmi_qp_frl_ltsl(struct dw_hdmi_qp *hdmi)
{
	enum drm_connector_status status;

	status = hdmi->phy.ops->read_hpd(hdmi, hdmi->phy.data);

	dev_dbg(hdmi->dev, "ltsl: conn_stat=%d\n", status);

	if (status != connector_status_disconnected) {
		drm_scdc_set_frl(hdmi->curr_conn, 0, 0, 0);
		drm_scdc_writeb(hdmi->bridge.ddc, SCDC_UPDATE_0, SCDC_FLT_UPDATE);
	}

	dw_hdmi_qp_mod(hdmi, 0, AVP_DATAPATH_VIDEO_SWDISABLE, GLOBAL_SWDISABLE);

	return LTSU;
}

static void dw_hdmi_qp_flt_work(struct work_struct *work)
{
	struct dw_hdmi_qp *hdmi = container_of(work, struct dw_hdmi_qp, flt_work);
	enum dw_hdmi_qp_frl_lts state = LTS1;

	while (true) {
		switch (state) {
		case LTS1:
			state = dw_hdmi_qp_frl_lts1(hdmi);
			break;
		case LTS2:
			state = dw_hdmi_qp_frl_lts2(hdmi);
			break;
		case LTS3:
			state = dw_hdmi_qp_frl_lts3(hdmi);
			break;
		case LTS4:
			state = dw_hdmi_qp_frl_lts4(hdmi);
			break;
		case LTSP:
			state = dw_hdmi_qp_frl_ltsp(hdmi);
			break;
		case LTSL:
			state = dw_hdmi_qp_frl_ltsl(hdmi);
			break;
		case LTSU:
			return;
		default:
			dev_err(hdmi->dev, "unexpected flt state: %d\n", state);
			return;
		}
	}
}

static bool dw_hdmi_qp_supports_scrambling(struct drm_display_info *display)
{
	if (!display->is_hdmi)
		return false;

	return display->hdmi.scdc.supported &&
		display->hdmi.scdc.scrambling.supported;
}

static int dw_hdmi_qp_set_scramb(struct dw_hdmi_qp *hdmi)
{
	bool done;

	dev_dbg(hdmi->dev, "set scrambling\n");

	done = drm_scdc_set_high_tmds_clock_ratio(hdmi->curr_conn, true);
	if (!done)
		return -EIO;

	done = drm_scdc_set_scrambling(hdmi->curr_conn, true);
	if (!done) {
		drm_scdc_set_high_tmds_clock_ratio(hdmi->curr_conn, false);
		return -EIO;
	}

	schedule_delayed_work(&hdmi->scramb_work,
			      msecs_to_jiffies(SCRAMB_POLL_DELAY_MS));
	return 0;
}

static void dw_hdmi_qp_scramb_work(struct work_struct *work)
{
	struct dw_hdmi_qp *hdmi = container_of(to_delayed_work(work),
					       struct dw_hdmi_qp,
					       scramb_work);
	if (READ_ONCE(hdmi->scramb_enabled) &&
	    !drm_scdc_get_scrambling_status(hdmi->curr_conn))
		dw_hdmi_qp_set_scramb(hdmi);
}

static void dw_hdmi_qp_enable_scramb(struct dw_hdmi_qp *hdmi)
{
	int ret;
	u8 ver;

	if (!dw_hdmi_qp_supports_scrambling(&hdmi->curr_conn->display_info))
		return;

	ret = drm_scdc_readb(hdmi->bridge.ddc, SCDC_SINK_VERSION, &ver);
	if (ret) {
		dev_err(hdmi->dev, "Failed to read SCDC_SINK_VERSION: %d\n", ret);
		return;
	}

	ret = drm_scdc_writeb(hdmi->bridge.ddc, SCDC_SOURCE_VERSION,
			      min_t(u8, ver, SCDC_MAX_SOURCE_VERSION));
	if (ret) {
		dev_err(hdmi->dev, "Failed to write SCDC_SOURCE_VERSION: %d\n", ret);
		return;
	}

	WRITE_ONCE(hdmi->scramb_enabled, true);

	ret = dw_hdmi_qp_set_scramb(hdmi);
	if (ret) {
		hdmi->scramb_enabled = false;
		return;
	}

	dw_hdmi_qp_write(hdmi, 1, SCRAMB_CONFIG0);

	/* Wait at least 1 ms before resuming TMDS transmission */
	usleep_range(1000, 5000);
}

static void dw_hdmi_qp_disable_scramb(struct dw_hdmi_qp *hdmi)
{
	if (!hdmi->scramb_enabled)
		return;

	dev_dbg(hdmi->dev, "disable scrambling\n");

	WRITE_ONCE(hdmi->scramb_enabled, false);
	cancel_delayed_work_sync(&hdmi->scramb_work);

	dw_hdmi_qp_write(hdmi, 0, SCRAMB_CONFIG0);

	if (hdmi->curr_conn->status == connector_status_connected) {
		drm_scdc_set_scrambling(hdmi->curr_conn, false);
		drm_scdc_set_high_tmds_clock_ratio(hdmi->curr_conn, false);
	}
}

static void dw_hdmi_qp_bridge_atomic_enable(struct drm_bridge *bridge,
					    struct drm_atomic_state *state)
{
	struct dw_hdmi_qp *hdmi = bridge->driver_private;
	const struct drm_connector_state *conn_state;
	const struct dw_hdmi_qp_link_cfg *link_cfg;
	const struct drm_display_mode *mode;
	struct drm_crtc_state *crtc_state;
	unsigned int op_mode;

	hdmi->curr_conn = drm_atomic_get_new_connector_for_encoder(state,
								   bridge->encoder);
	if (WARN_ON(!hdmi->curr_conn))
		return;

	conn_state = drm_atomic_get_new_connector_state(state, hdmi->curr_conn);
	if (WARN_ON(!conn_state))
		return;

	link_cfg = hdmi->phy.ops->get_link_cfg(hdmi, hdmi->phy.data);

	if (hdmi->curr_conn->display_info.is_hdmi) {
		crtc_state = drm_atomic_get_new_crtc_state(state, conn_state->crtc);
		mode = &crtc_state->mode;
		dev_dbg(hdmi->dev, "%s mode=%ux%u@%uHz fmt=%s rate=%llu bpc=%u\n", __func__,
			mode->hdisplay, mode->vdisplay, drm_mode_vrefresh(mode),
			drm_hdmi_connector_get_output_format_name(conn_state->hdmi.output_format),
			conn_state->hdmi.tmds_char_rate, conn_state->hdmi.output_bpc);

		op_mode = 0;
		hdmi->tmds_char_rate = conn_state->hdmi.tmds_char_rate;

		if (!link_cfg->frl_enabled &&
		    conn_state->hdmi.tmds_char_rate > HDMI14_MAX_TMDSCLK)
			dw_hdmi_qp_enable_scramb(hdmi);
	} else {
		dev_dbg(hdmi->dev, "%s mode=DVI\n", __func__);
		op_mode = OPMODE_DVI;
	}

	hdmi->phy.ops->init(hdmi, hdmi->phy.data);

	dw_hdmi_qp_mod(hdmi, HDCP2_BYPASS, HDCP2_BYPASS, HDCP2LOGIC_CONFIG0);
	dw_hdmi_qp_mod(hdmi, op_mode, OPMODE_DVI, LINK_CONFIG0);

	dw_hdmi_qp_mod(hdmi,
		       link_cfg->frl_enabled && link_cfg->frl_lanes == 4 ? OPMODE_FRL_4LANES : 0,
		       OPMODE_FRL_4LANES, LINK_CONFIG0);
	dw_hdmi_qp_mod(hdmi, link_cfg->frl_enabled, OPMODE_FRL, LINK_CONFIG0);

	drm_atomic_helper_connector_hdmi_update_infoframes(hdmi->curr_conn, state);

	if (link_cfg->frl_enabled) {
		/* Ensure phy output is stable before starting FLT */
		msleep(50);
		schedule_work(&hdmi->flt_work);
	} else {
		dw_hdmi_qp_write(hdmi, PKTSCHED_GCP_CLEAR_AVMUTE, PKTSCHED_PKT_CONTROL0);
		dw_hdmi_qp_mod(hdmi, PKTSCHED_GCP_TX_EN, PKTSCHED_GCP_TX_EN, PKTSCHED_PKT_EN);
	}
}

static void dw_hdmi_qp_bridge_atomic_disable(struct drm_bridge *bridge,
					     struct drm_atomic_state *state)
{
	struct dw_hdmi_qp *hdmi = bridge->driver_private;

	hdmi->tmds_char_rate = 0;

	dw_hdmi_qp_write(hdmi, PKTSCHED_GCP_SET_AVMUTE, PKTSCHED_PKT_CONTROL0);
	msleep(50);

	dw_hdmi_qp_disable_scramb(hdmi);
	cancel_work_sync(&hdmi->flt_work);

	hdmi->curr_conn = NULL;
	hdmi->phy.ops->disable(hdmi, hdmi->phy.data);
}

static int dw_hdmi_qp_reset_crtc(struct dw_hdmi_qp *hdmi,
				 struct drm_connector *connector,
				 struct drm_modeset_acquire_ctx *ctx)
{
	u8 config;
	int ret;

	if (hdmi->scramb_enabled) {
		ret = drm_scdc_readb(hdmi->bridge.ddc, SCDC_TMDS_CONFIG, &config);
		if (ret < 0) {
			dev_err(hdmi->dev, "Failed to read TMDS config: %d\n", ret);
			return ret;
		}

		if (config & SCDC_SCRAMBLING_ENABLE)
			return 0;
	} else {
		ret = drm_scdc_readb(hdmi->bridge.ddc, SCDC_CONFIG_1, &config);
		if (ret < 0) {
			dev_err(hdmi->dev, "Failed to read FRL config: %d\n", ret);
			return ret;
		}

		if (config & SCDC_FRL_RATE_MASK && work_busy(&hdmi->flt_work))
			return 0;
	}

	drm_atomic_helper_connector_hdmi_hotplug(connector,
						 connector_status_connected);
	/*
	 * Conform to HDMI 2.0 spec by ensuring scrambled data is not sent
	 * before configuring the sink scrambling, as well as suspending any
	 * TMDS transmission while changing the TMDS clock rate in the sink.
	 */

	dev_dbg(hdmi->dev, "resetting crtc\n");

	return drm_bridge_helper_reset_crtc(&hdmi->bridge, ctx);
}

static int dw_hdmi_qp_bridge_detect_ctx(struct drm_bridge *bridge,
					struct drm_connector *connector,
					struct drm_modeset_acquire_ctx *ctx)
{
	struct dw_hdmi_qp *hdmi = bridge->driver_private;
	const struct dw_hdmi_qp_link_cfg *link_cfg;
	enum drm_connector_status status;
	const struct drm_edid *drm_edid;
	bool frl_active;
	int ret;

	if (hdmi->no_hpd) {
		drm_edid = drm_edid_read_ddc(connector, bridge->ddc);
		if (drm_edid)
			return connector_status_connected;
		else
			return connector_status_disconnected;
	}

	status = hdmi->phy.ops->read_hpd(hdmi, hdmi->phy.data);

	link_cfg = hdmi->phy.ops->get_link_cfg(hdmi, hdmi->phy.data);
	frl_active = link_cfg->frl_enabled && hdmi->tmds_char_rate;

	dev_dbg(hdmi->dev, "%s status=%d frl=%u scramb=%u\n", __func__,
		status, frl_active, hdmi->scramb_enabled);

	if (status == connector_status_connected &&
	    (frl_active || hdmi->scramb_enabled)) {
		ret = dw_hdmi_qp_reset_crtc(hdmi, connector, ctx);
		if (ret == -EDEADLK)
			return ret;
		if (ret < 0)
			status = connector_status_unknown;
	}

	return status;
}

static const struct drm_edid *
dw_hdmi_qp_bridge_edid_read(struct drm_bridge *bridge,
			    struct drm_connector *connector)
{
	struct dw_hdmi_qp *hdmi = bridge->driver_private;
	const struct drm_edid *drm_edid;

	drm_edid = drm_edid_read_ddc(connector, bridge->ddc);
	if (!drm_edid)
		dev_dbg(hdmi->dev, "failed to get edid\n");

	return drm_edid;
}

static enum drm_mode_status
dw_hdmi_qp_bridge_tmds_char_rate_valid(const struct drm_bridge *bridge,
				       const struct drm_display_mode *mode,
				       unsigned long long rate)
{
	struct dw_hdmi_qp *hdmi = bridge->driver_private;

	if (hdmi->no_hpd && rate > HDMI14_MAX_TMDSCLK) {
		dev_dbg(hdmi->dev, "Unsupported TMDS char rate in no_hpd mode: %lld\n", rate);
		return MODE_CLOCK_HIGH;
	}

	if (rate > HDMI20_MAX_TMDSRATE) {
		dev_dbg(hdmi->dev, "Unsupported TMDS char rate: %lld\n", rate);
		return MODE_CLOCK_HIGH;
	}

	return MODE_OK;
}

static enum drm_mode_status
dw_hdmi_qp_bridge_frl_rate_valid(const struct drm_bridge *bridge,
				 const struct drm_display_mode *mode,
				 unsigned long long frl_data_rate,
				 unsigned long long frl_max_link_rate)
{
	struct dw_hdmi_qp *hdmi = bridge->driver_private;
	const struct dw_hdmi_qp_link_cfg *link_cfg;
	unsigned long long frl_bw;

	if (!hdmi->phy.ops->set_frl_rate) {
		dev_dbg(hdmi->dev, "Unsupported FRL rate: %lld\n", frl_data_rate);
		return MODE_CLOCK_HIGH;
	}

	link_cfg = hdmi->phy.ops->get_link_cfg(hdmi, hdmi->phy.data);

	frl_bw = link_cfg->max_frl_rate_per_lane *
		 link_cfg->max_frl_lanes * 1000000000ULL;
	if (frl_bw < frl_data_rate) {
		dev_dbg(hdmi->dev, "Unsupported FRL data rate: req=%llu max=%llu\n",
			frl_data_rate, frl_bw);
		return MODE_CLOCK_HIGH;
	}

	frl_bw = link_cfg->min_frl_rate_per_lane *
		 link_cfg->min_frl_lanes * 1000000000ULL;
	if (frl_bw > frl_max_link_rate) {
		dev_dbg(hdmi->dev, "Unsupported FRL link rate: req=%llu min=%llu\n",
			frl_max_link_rate, frl_bw);
		return MODE_CLOCK_LOW;
	}

	return MODE_OK;
}

static int dw_hdmi_qp_bridge_clear_avi_infoframe(struct drm_bridge *bridge)
{
	struct dw_hdmi_qp *hdmi = bridge->driver_private;

	dw_hdmi_qp_mod(hdmi, 0, PKTSCHED_AVI_TX_EN, PKTSCHED_PKT_EN);

	return 0;
}

static int dw_hdmi_qp_bridge_clear_hdmi_infoframe(struct drm_bridge *bridge)
{
	struct dw_hdmi_qp *hdmi = bridge->driver_private;

	dw_hdmi_qp_mod(hdmi, 0, PKTSCHED_VSI_TX_EN, PKTSCHED_PKT_EN);

	return 0;
}

static int dw_hdmi_qp_bridge_clear_hdr_drm_infoframe(struct drm_bridge *bridge)
{
	struct dw_hdmi_qp *hdmi = bridge->driver_private;

	dw_hdmi_qp_mod(hdmi, 0, PKTSCHED_DRMI_TX_EN, PKTSCHED_PKT_EN);

	return 0;
}

static int dw_hdmi_qp_bridge_clear_spd_infoframe(struct drm_bridge *bridge)
{
	struct dw_hdmi_qp *hdmi = bridge->driver_private;

	dw_hdmi_qp_mod(hdmi, 0, PKTSCHED_SPDI_TX_EN, PKTSCHED_PKT_EN);

	return 0;
}

static int dw_hdmi_qp_bridge_clear_audio_infoframe(struct drm_bridge *bridge)
{
	struct dw_hdmi_qp *hdmi = bridge->driver_private;

	dw_hdmi_qp_mod(hdmi, 0,
		       PKTSCHED_ACR_TX_EN |
		       PKTSCHED_AUDS_TX_EN |
		       PKTSCHED_AUDI_TX_EN,
		       PKTSCHED_PKT_EN);

	return 0;
}

static void dw_hdmi_qp_write_pkt(struct dw_hdmi_qp *hdmi, const u8 *buffer,
				 size_t start, size_t len, unsigned int reg)
{
	u32 val = 0;
	size_t i;

	for (i = start; i < start + len; i++)
		val |= buffer[i] << ((i % 4) * BITS_PER_BYTE);

	dw_hdmi_qp_write(hdmi, val, reg);
}

static void dw_hdmi_qp_write_infoframe(struct dw_hdmi_qp *hdmi, const u8 *buffer,
				       size_t len, unsigned int reg)
{
	size_t i;

	/* InfoFrame packet header */
	dw_hdmi_qp_write_pkt(hdmi, buffer, 1, 2, reg);

	/* InfoFrame packet body */
	for (i = 0; i < len - 3; i += 4)
		dw_hdmi_qp_write_pkt(hdmi, buffer + 3, i, min(len - i - 3, 4),
				     reg + i + 4);
}

static int dw_hdmi_qp_bridge_write_avi_infoframe(struct drm_bridge *bridge,
						 const u8 *buffer, size_t len)
{
	struct dw_hdmi_qp *hdmi = bridge->driver_private;

	dw_hdmi_qp_bridge_clear_avi_infoframe(bridge);

	dw_hdmi_qp_write_infoframe(hdmi, buffer, len, PKT_AVI_CONTENTS0);

	dw_hdmi_qp_mod(hdmi, 0, PKTSCHED_AVI_FIELDRATE, PKTSCHED_PKT_CONFIG1);
	dw_hdmi_qp_mod(hdmi, PKTSCHED_AVI_TX_EN, PKTSCHED_AVI_TX_EN,
		       PKTSCHED_PKT_EN);

	return 0;
}

static int dw_hdmi_qp_bridge_write_hdmi_infoframe(struct drm_bridge *bridge,
						  const u8 *buffer, size_t len)
{
	struct dw_hdmi_qp *hdmi = bridge->driver_private;

	dw_hdmi_qp_bridge_clear_hdmi_infoframe(bridge);

	dw_hdmi_qp_write_infoframe(hdmi, buffer, len, PKT_VSI_CONTENTS0);

	dw_hdmi_qp_mod(hdmi, 0, PKTSCHED_VSI_FIELDRATE, PKTSCHED_PKT_CONFIG1);
	dw_hdmi_qp_mod(hdmi, PKTSCHED_VSI_TX_EN, PKTSCHED_VSI_TX_EN,
		       PKTSCHED_PKT_EN);

	return 0;
}

static int dw_hdmi_qp_bridge_write_hdr_drm_infoframe(struct drm_bridge *bridge,
						     const u8 *buffer, size_t len)
{
	struct dw_hdmi_qp *hdmi = bridge->driver_private;

	dw_hdmi_qp_bridge_clear_hdr_drm_infoframe(bridge);

	dw_hdmi_qp_write_infoframe(hdmi, buffer, len, PKT_DRMI_CONTENTS0);

	dw_hdmi_qp_mod(hdmi, 0, PKTSCHED_DRMI_FIELDRATE, PKTSCHED_PKT_CONFIG1);
	dw_hdmi_qp_mod(hdmi, PKTSCHED_DRMI_TX_EN, PKTSCHED_DRMI_TX_EN,
		       PKTSCHED_PKT_EN);

	return 0;
}

static int dw_hdmi_qp_bridge_write_spd_infoframe(struct drm_bridge *bridge,
						 const u8 *buffer, size_t len)
{
	struct dw_hdmi_qp *hdmi = bridge->driver_private;

	dw_hdmi_qp_bridge_clear_spd_infoframe(bridge);

	dw_hdmi_qp_write_infoframe(hdmi, buffer, len, PKT_SPDI_CONTENTS0);

	dw_hdmi_qp_mod(hdmi, PKTSCHED_SPDI_TX_EN, PKTSCHED_SPDI_TX_EN,
		       PKTSCHED_PKT_EN);

	return 0;
}

static int dw_hdmi_qp_bridge_write_audio_infoframe(struct drm_bridge *bridge,
						   const u8 *buffer, size_t len)
{
	struct dw_hdmi_qp *hdmi = bridge->driver_private;

	dw_hdmi_qp_bridge_clear_audio_infoframe(bridge);

	/*
	 * AUDI_CONTENTS0: { RSV, HB2, HB1, RSV }
	 * AUDI_CONTENTS1: { PB3, PB2, PB1, PB0 }
	 * AUDI_CONTENTS2: { PB7, PB6, PB5, PB4 }
	 *
	 * PB0: CheckSum
	 * PB1: | CT3    | CT2  | CT1  | CT0  | F13  | CC2 | CC1 | CC0 |
	 * PB2: | F27    | F26  | F25  | SF2  | SF1  | SF0 | SS1 | SS0 |
	 * PB3: | F37    | F36  | F35  | F34  | F33  | F32 | F31 | F30 |
	 * PB4: | CA7    | CA6  | CA5  | CA4  | CA3  | CA2 | CA1 | CA0 |
	 * PB5: | DM_INH | LSV3 | LSV2 | LSV1 | LSV0 | F52 | F51 | F50 |
	 * PB6~PB10: Reserved
	 */
	dw_hdmi_qp_write_infoframe(hdmi, buffer, len, PKT_AUDI_CONTENTS0);

	/* Enable ACR, AUDI, AMD */
	dw_hdmi_qp_mod(hdmi,
		       PKTSCHED_ACR_TX_EN | PKTSCHED_AUDI_TX_EN | PKTSCHED_AMD_TX_EN,
		       PKTSCHED_ACR_TX_EN | PKTSCHED_AUDI_TX_EN | PKTSCHED_AMD_TX_EN,
		       PKTSCHED_PKT_EN);

	/* Enable AUDS */
	dw_hdmi_qp_mod(hdmi, PKTSCHED_AUDS_TX_EN, PKTSCHED_AUDS_TX_EN, PKTSCHED_PKT_EN);

	return 0;
}

#ifdef CONFIG_DRM_DW_HDMI_QP_CEC
static irqreturn_t dw_hdmi_qp_cec_hardirq(int irq, void *dev_id)
{
	struct dw_hdmi_qp *hdmi = dev_id;
	struct dw_hdmi_qp_cec *cec = hdmi->cec;
	irqreturn_t ret = IRQ_HANDLED;
	u32 stat;

	stat = dw_hdmi_qp_read(hdmi, CEC_INT_STATUS);
	if (stat == 0)
		return IRQ_NONE;

	dw_hdmi_qp_write(hdmi, stat, CEC_INT_CLEAR);

	if (stat & CEC_STAT_LINE_ERR) {
		cec->tx_status = CEC_TX_STATUS_ERROR;
		cec->tx_done = true;
		ret = IRQ_WAKE_THREAD;
	} else if (stat & CEC_STAT_DONE) {
		cec->tx_status = CEC_TX_STATUS_OK;
		cec->tx_done = true;
		ret = IRQ_WAKE_THREAD;
	} else if (stat & CEC_STAT_NACK) {
		cec->tx_status = CEC_TX_STATUS_NACK;
		cec->tx_done = true;
		ret = IRQ_WAKE_THREAD;
	}

	if (stat & CEC_STAT_EOM) {
		unsigned int len, i, val;

		val = dw_hdmi_qp_read(hdmi, CEC_RX_COUNT_STATUS);
		len = (val & 0xf) + 1;

		if (len > sizeof(cec->rx_msg.msg))
			len = sizeof(cec->rx_msg.msg);

		for (i = 0; i < 4; i++) {
			val = dw_hdmi_qp_read(hdmi, CEC_RX_DATA3_0 + i * 4);
			cec->rx_msg.msg[i * 4] = val & 0xff;
			cec->rx_msg.msg[i * 4 + 1] = (val >> 8) & 0xff;
			cec->rx_msg.msg[i * 4 + 2] = (val >> 16) & 0xff;
			cec->rx_msg.msg[i * 4 + 3] = (val >> 24) & 0xff;
		}

		dw_hdmi_qp_write(hdmi, 1, CEC_LOCK_CONTROL);

		cec->rx_msg.len = len;
		cec->rx_done = true;

		ret = IRQ_WAKE_THREAD;
	}

	return ret;
}

static irqreturn_t dw_hdmi_qp_cec_thread(int irq, void *dev_id)
{
	struct dw_hdmi_qp *hdmi = dev_id;
	struct dw_hdmi_qp_cec *cec = hdmi->cec;

	if (cec->tx_done) {
		cec->tx_done = false;
		drm_connector_hdmi_cec_transmit_attempt_done(cec->connector,
							     cec->tx_status);
	}

	if (cec->rx_done) {
		cec->rx_done = false;
		drm_connector_hdmi_cec_received_msg(cec->connector, &cec->rx_msg);
	}

	return IRQ_HANDLED;
}

static int dw_hdmi_qp_cec_init(struct drm_bridge *bridge,
			       struct drm_connector *connector)
{
	struct dw_hdmi_qp *hdmi = dw_hdmi_qp_from_bridge(bridge);
	struct dw_hdmi_qp_cec *cec = hdmi->cec;

	cec->connector = connector;

	dw_hdmi_qp_write(hdmi, 0, CEC_TX_COUNT);
	dw_hdmi_qp_write(hdmi, ~0, CEC_INT_CLEAR);
	dw_hdmi_qp_write(hdmi, 0, CEC_INT_MASK_N);

	return devm_request_threaded_irq(hdmi->dev, cec->irq,
					 dw_hdmi_qp_cec_hardirq,
					 dw_hdmi_qp_cec_thread, IRQF_SHARED,
					 dev_name(hdmi->dev), hdmi);
}

static int dw_hdmi_qp_cec_log_addr(struct drm_bridge *bridge, u8 logical_addr)
{
	struct dw_hdmi_qp *hdmi = dw_hdmi_qp_from_bridge(bridge);
	struct dw_hdmi_qp_cec *cec = hdmi->cec;

	if (logical_addr == CEC_LOG_ADDR_INVALID)
		cec->addresses = 0;
	else
		cec->addresses |= BIT(logical_addr) | CEC_ADDR_BROADCAST;

	dw_hdmi_qp_write(hdmi, cec->addresses, CEC_ADDR);

	return 0;
}

static int dw_hdmi_qp_cec_enable(struct drm_bridge *bridge, bool enable)
{
	struct dw_hdmi_qp *hdmi = dw_hdmi_qp_from_bridge(bridge);
	unsigned int irqs;
	u32 swdisable;

	if (!enable) {
		dw_hdmi_qp_write(hdmi, 0, CEC_INT_MASK_N);
		dw_hdmi_qp_write(hdmi, ~0, CEC_INT_CLEAR);

		swdisable = dw_hdmi_qp_read(hdmi, GLOBAL_SWDISABLE);
		swdisable = swdisable | CEC_SWDISABLE;
		dw_hdmi_qp_write(hdmi, swdisable, GLOBAL_SWDISABLE);
	} else {
		swdisable = dw_hdmi_qp_read(hdmi, GLOBAL_SWDISABLE);
		swdisable = swdisable & ~CEC_SWDISABLE;
		dw_hdmi_qp_write(hdmi, swdisable, GLOBAL_SWDISABLE);

		dw_hdmi_qp_write(hdmi, ~0, CEC_INT_CLEAR);
		dw_hdmi_qp_write(hdmi, 1, CEC_LOCK_CONTROL);

		dw_hdmi_qp_cec_log_addr(bridge, CEC_LOG_ADDR_INVALID);

		irqs = CEC_STAT_LINE_ERR | CEC_STAT_NACK | CEC_STAT_EOM |
		       CEC_STAT_DONE;
		dw_hdmi_qp_write(hdmi, ~0, CEC_INT_CLEAR);
		dw_hdmi_qp_write(hdmi, irqs, CEC_INT_MASK_N);
	}

	return 0;
}

static int dw_hdmi_qp_cec_transmit(struct drm_bridge *bridge, u8 attempts,
				   u32 signal_free_time, struct cec_msg *msg)
{
	struct dw_hdmi_qp *hdmi = dw_hdmi_qp_from_bridge(bridge);
	unsigned int i;
	u32 val;

	for (i = 0; i < msg->len; i++) {
		if (!(i % 4))
			val = msg->msg[i];
		if ((i % 4) == 1)
			val |= msg->msg[i] << 8;
		if ((i % 4) == 2)
			val |= msg->msg[i] << 16;
		if ((i % 4) == 3)
			val |= msg->msg[i] << 24;

		if (i == (msg->len - 1) || (i % 4) == 3)
			dw_hdmi_qp_write(hdmi, val, CEC_TX_DATA3_0 + (i / 4) * 4);
	}

	dw_hdmi_qp_write(hdmi, msg->len - 1, CEC_TX_COUNT);
	dw_hdmi_qp_write(hdmi, CEC_CTRL_START, CEC_TX_CONTROL);

	return 0;
}
#else
#define dw_hdmi_qp_cec_init NULL
#define dw_hdmi_qp_cec_enable NULL
#define dw_hdmi_qp_cec_log_addr NULL
#define dw_hdmi_qp_cec_transmit NULL
#endif /* CONFIG_DRM_DW_HDMI_QP_CEC */

static const struct drm_bridge_funcs dw_hdmi_qp_bridge_funcs = {
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_reset = drm_atomic_helper_bridge_reset,
	.atomic_enable = dw_hdmi_qp_bridge_atomic_enable,
	.atomic_disable = dw_hdmi_qp_bridge_atomic_disable,
	.detect_ctx = dw_hdmi_qp_bridge_detect_ctx,
	.edid_read = dw_hdmi_qp_bridge_edid_read,
	.hdmi_tmds_char_rate_valid = dw_hdmi_qp_bridge_tmds_char_rate_valid,
	.hdmi_frl_rate_valid = dw_hdmi_qp_bridge_frl_rate_valid,
	.hdmi_clear_avi_infoframe = dw_hdmi_qp_bridge_clear_avi_infoframe,
	.hdmi_write_avi_infoframe = dw_hdmi_qp_bridge_write_avi_infoframe,
	.hdmi_clear_hdmi_infoframe = dw_hdmi_qp_bridge_clear_hdmi_infoframe,
	.hdmi_write_hdmi_infoframe = dw_hdmi_qp_bridge_write_hdmi_infoframe,
	.hdmi_clear_hdr_drm_infoframe = dw_hdmi_qp_bridge_clear_hdr_drm_infoframe,
	.hdmi_write_hdr_drm_infoframe = dw_hdmi_qp_bridge_write_hdr_drm_infoframe,
	.hdmi_clear_spd_infoframe = dw_hdmi_qp_bridge_clear_spd_infoframe,
	.hdmi_write_spd_infoframe = dw_hdmi_qp_bridge_write_spd_infoframe,
	.hdmi_clear_audio_infoframe = dw_hdmi_qp_bridge_clear_audio_infoframe,
	.hdmi_write_audio_infoframe = dw_hdmi_qp_bridge_write_audio_infoframe,
	.hdmi_audio_startup = dw_hdmi_qp_audio_enable,
	.hdmi_audio_shutdown = dw_hdmi_qp_audio_disable,
	.hdmi_audio_prepare = dw_hdmi_qp_audio_prepare,
	.hdmi_cec_init = dw_hdmi_qp_cec_init,
	.hdmi_cec_enable = dw_hdmi_qp_cec_enable,
	.hdmi_cec_log_addr = dw_hdmi_qp_cec_log_addr,
	.hdmi_cec_transmit = dw_hdmi_qp_cec_transmit,
};

static irqreturn_t dw_hdmi_qp_main_hardirq(int irq, void *dev_id)
{
	struct dw_hdmi_qp *hdmi = dev_id;
	struct dw_hdmi_qp_i2c *i2c = hdmi->i2c;
	u32 stat;

	stat = dw_hdmi_qp_read(hdmi, MAINUNIT_1_INT_STATUS);

	i2c->stat = stat & (I2CM_OP_DONE_IRQ | I2CM_READ_REQUEST_IRQ |
			    I2CM_NACK_RCVD_IRQ);

	if (i2c->stat) {
		dw_hdmi_qp_write(hdmi, i2c->stat, MAINUNIT_1_INT_CLEAR);
		complete(&i2c->cmp);
	}

	if (stat)
		return IRQ_HANDLED;

	return IRQ_NONE;
}

static const struct regmap_config dw_hdmi_qp_regmap_config = {
	.reg_bits	= 32,
	.val_bits	= 32,
	.reg_stride	= 4,
	.max_register	= EARCRX_1_INT_FORCE,
};

static void dw_hdmi_qp_init_hw(struct dw_hdmi_qp *hdmi)
{
	dw_hdmi_qp_write(hdmi, 0, MAINUNIT_0_INT_MASK_N);
	dw_hdmi_qp_write(hdmi, 0, MAINUNIT_1_INT_MASK_N);
	dw_hdmi_qp_write(hdmi, hdmi->ref_clk_rate, TIMER_BASE_CONFIG0);

	/* Software reset */
	dw_hdmi_qp_write(hdmi, 0x01, I2CM_CONTROL0);
	dw_hdmi_qp_write(hdmi, 0x085c085c, I2CM_FM_SCL_CONFIG0);
	dw_hdmi_qp_mod(hdmi, 0, I2CM_FM_EN, I2CM_INTERFACE_CONTROL0);

	/* Clear DONE and ERROR interrupts */
	dw_hdmi_qp_write(hdmi, I2CM_OP_DONE_CLEAR | I2CM_NACK_RCVD_CLEAR,
			 MAINUNIT_1_INT_CLEAR);

	if (hdmi->phy.ops->setup_hpd)
		hdmi->phy.ops->setup_hpd(hdmi, hdmi->phy.data);
}

struct dw_hdmi_qp *dw_hdmi_qp_bind(struct platform_device *pdev,
				   struct drm_encoder *encoder,
				   const struct dw_hdmi_qp_plat_data *plat_data)
{
	struct device *dev = &pdev->dev;
	struct dw_hdmi_qp *hdmi;
	void __iomem *regs;
	int ret;

	if (!plat_data->phy_ops || !plat_data->phy_ops->init ||
	    !plat_data->phy_ops->disable || !plat_data->phy_ops->read_hpd ||
	    !plat_data->phy_ops->get_link_cfg) {
		dev_err(dev, "Missing platform PHY ops\n");
		return ERR_PTR(-ENODEV);
	}

	hdmi = devm_drm_bridge_alloc(dev, struct dw_hdmi_qp, bridge,
				     &dw_hdmi_qp_bridge_funcs);
	if (IS_ERR(hdmi))
		return ERR_CAST(hdmi);

	INIT_DELAYED_WORK(&hdmi->scramb_work, dw_hdmi_qp_scramb_work);
	INIT_WORK(&hdmi->flt_work, dw_hdmi_qp_flt_work);

	hdmi->dev = dev;

	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs))
		return ERR_CAST(regs);

	hdmi->regm = devm_regmap_init_mmio(dev, regs, &dw_hdmi_qp_regmap_config);
	if (IS_ERR(hdmi->regm)) {
		dev_err(dev, "Failed to configure regmap\n");
		return ERR_CAST(hdmi->regm);
	}

	hdmi->phy.ops = plat_data->phy_ops;
	hdmi->phy.data = plat_data->phy_data;

	if (plat_data->ref_clk_rate) {
		hdmi->ref_clk_rate = plat_data->ref_clk_rate;
	} else {
		hdmi->ref_clk_rate = 428571429;
		dev_warn(dev, "Set ref_clk_rate to vendor default\n");
	}

	dw_hdmi_qp_init_hw(hdmi);

	hdmi->main_irq = plat_data->main_irq;
	ret = devm_request_threaded_irq(dev, plat_data->main_irq,
					dw_hdmi_qp_main_hardirq, NULL,
					IRQF_SHARED, dev_name(dev), hdmi);
	if (ret)
		return ERR_PTR(ret);

	hdmi->no_hpd = device_property_read_bool(dev, "no-hpd");

	hdmi->bridge.driver_private = hdmi;
	hdmi->bridge.ops = DRM_BRIDGE_OP_DETECT |
			   DRM_BRIDGE_OP_EDID |
			   DRM_BRIDGE_OP_HDMI |
			   DRM_BRIDGE_OP_HDMI_AUDIO |
			   DRM_BRIDGE_OP_HDMI_HDR_DRM_INFOFRAME |
			   DRM_BRIDGE_OP_HDMI_SPD_INFOFRAME;
	if (!hdmi->no_hpd)
		hdmi->bridge.ops |= DRM_BRIDGE_OP_HPD;
	hdmi->bridge.of_node = pdev->dev.of_node;
	hdmi->bridge.type = DRM_MODE_CONNECTOR_HDMIA;
	hdmi->bridge.vendor = "Synopsys";
	hdmi->bridge.product = "DW HDMI QP TX";

	if (plat_data->supported_formats)
		hdmi->bridge.supported_formats = plat_data->supported_formats;

	if (plat_data->max_bpc)
		hdmi->bridge.max_bpc = plat_data->max_bpc;

	hdmi->bridge.ddc = dw_hdmi_qp_i2c_adapter(hdmi);
	if (IS_ERR(hdmi->bridge.ddc))
		return ERR_CAST(hdmi->bridge.ddc);

	hdmi->bridge.hdmi_audio_max_i2s_playback_channels = 8;
	hdmi->bridge.hdmi_audio_dev = dev;
	hdmi->bridge.hdmi_audio_dai_port = 1;

#ifdef CONFIG_DRM_DW_HDMI_QP_CEC
	if (plat_data->cec_irq) {
		hdmi->bridge.ops |= DRM_BRIDGE_OP_HDMI_CEC_ADAPTER;
		hdmi->bridge.hdmi_cec_dev = dev;
		hdmi->bridge.hdmi_cec_adapter_name = dev_name(dev);

		hdmi->cec = devm_kzalloc(hdmi->dev, sizeof(*hdmi->cec), GFP_KERNEL);
		if (!hdmi->cec)
			return ERR_PTR(-ENOMEM);

		hdmi->cec->irq = plat_data->cec_irq;
	} else {
		dev_warn(dev, "Disabled CEC support due to missing IRQ\n");
	}
#endif

	ret = devm_drm_bridge_add(dev, &hdmi->bridge);
	if (ret)
		return ERR_PTR(ret);

	ret = drm_bridge_attach(encoder, &hdmi->bridge, NULL,
				DRM_BRIDGE_ATTACH_NO_CONNECTOR);
	if (ret)
		return ERR_PTR(ret);

	return hdmi;
}
EXPORT_SYMBOL_GPL(dw_hdmi_qp_bind);

void dw_hdmi_qp_suspend(struct device *dev, struct dw_hdmi_qp *hdmi)
{
	disable_irq(hdmi->main_irq);
}
EXPORT_SYMBOL_GPL(dw_hdmi_qp_suspend);

void dw_hdmi_qp_resume(struct device *dev, struct dw_hdmi_qp *hdmi)
{
	dw_hdmi_qp_init_hw(hdmi);
	enable_irq(hdmi->main_irq);
}
EXPORT_SYMBOL_GPL(dw_hdmi_qp_resume);

MODULE_AUTHOR("Algea Cao <algea.cao@rock-chips.com>");
MODULE_AUTHOR("Cristian Ciocaltea <cristian.ciocaltea@collabora.com>");
MODULE_DESCRIPTION("DW HDMI QP transmitter library");
MODULE_LICENSE("GPL");
