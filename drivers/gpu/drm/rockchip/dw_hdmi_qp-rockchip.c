// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2021-2022 Rockchip Electronics Co., Ltd.
 * Copyright (c) 2024-2026 Collabora Ltd.
 *
 * Author: Algea Cao <algea.cao@rock-chips.com>
 * Author: Cristian Ciocaltea <cristian.ciocaltea@collabora.com>
 */

#include <linux/clk.h>
#include <linux/gpio/consumer.h>
#include <linux/hw_bitfield.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-hdmi.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#include <drm/bridge/dw_hdmi_qp.h>
#include <drm/display/drm_hdmi_helper.h>
#include <drm/drm_bridge_connector.h>
#include <drm/drm_managed.h>
#include <drm/drm_of.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

#include "rockchip_drm_drv.h"

#define RK3576_IOC_MISC_CON0		0xa400
#define RK3576_HDMI_HPD_INT_MSK		BIT(2)
#define RK3576_HDMI_HPD_INT_CLR		BIT(1)

#define RK3576_IOC_HDMI_HPD_STATUS	0xa440
#define RK3576_HDMI_LEVEL_INT		BIT(3)

#define RK3576_VO0_GRF_SOC_CON1		0x0004
#define RK3576_HDMI_FRL_MOD		BIT(0)
#define RK3576_HDMI_HDCP14_MEM_EN	BIT(15)

#define RK3576_VO0_GRF_SOC_CON8		0x0020
#define RK3576_COLOR_DEPTH_MASK		GENMASK(11, 8)
#define RK3576_8BPC			0x0
#define RK3576_10BPC			0x6
#define RK3576_COLOR_FORMAT_MASK	GENMASK(7, 4)
#define RK3576_RGB			0x9
#define RK3576_YUV422			0x1
#define RK3576_YUV444			0x2
#define RK3576_YUV420			0x3
#define RK3576_CECIN_MASK		BIT(3)

#define RK3576_VO0_GRF_SOC_CON14	0x0038
#define RK3576_I2S_SEL_MASK		BIT(0)
#define RK3576_SPDIF_SEL_MASK		BIT(1)
#define HDCP0_P1_GPIO_IN		BIT(2)
#define RK3576_SCLIN_MASK		BIT(4)
#define RK3576_SDAIN_MASK		BIT(5)
#define RK3576_HDMI_GRANT_SEL		BIT(6)

#define RK3588_GRF_SOC_CON2		0x0308
#define RK3588_HDMI0_HPD_INT_MSK	BIT(13)
#define RK3588_HDMI0_HPD_INT_CLR	BIT(12)
#define RK3588_HDMI1_HPD_INT_MSK	BIT(15)
#define RK3588_HDMI1_HPD_INT_CLR	BIT(14)

#define RK3588_GRF_SOC_CON7		0x031c
#define RK3588_HPD_HDMI0_IO_EN_MASK	BIT(12)
#define RK3588_HPD_HDMI1_IO_EN_MASK	BIT(13)
#define RK3588_GRF_SOC_STATUS1		0x0384
#define RK3588_HDMI0_LEVEL_INT		BIT(16)
#define RK3588_HDMI1_LEVEL_INT		BIT(24)

#define RK3588_GRF_VO1_CON3		0x000c
#define RK3588_GRF_VO1_CON4		0x0010
#define RK3588_HDMI21_MASK		BIT(0)

#define RK3588_GRF_VO1_CON6		0x0018
#define RK3588_COLOR_DEPTH_MASK		GENMASK(7, 4)
#define RK3588_8BPC			0x0
#define RK3588_10BPC			0x6
#define RK3588_COLOR_FORMAT_MASK	GENMASK(3, 0)
#define RK3588_RGB			0x0
#define RK3588_YUV420			0x3
#define RK3588_SCLIN_MASK		BIT(9)
#define RK3588_SDAIN_MASK		BIT(10)
#define RK3588_MODE_MASK		BIT(11)
#define RK3588_I2S_SEL_MASK		BIT(13)

#define RK3588_GRF_VO1_CON7		0x001c
#define RK3588_GRF_VO1_CON9		0x0024
#define RK3588_HDMI0_GRANT_SEL		BIT(10)
#define RK3588_HDMI1_GRANT_SEL		BIT(12)

#define HOTPLUG_DEBOUNCE_MS		150
#define MAX_HDMI_PORT_NUM		2
#define HDMI20_MAX_TMDS_RATE		600000000
#define HDMI21_MAX_FRL_LANE_RATE	12
#define HDMI21_MAX_FRL_LANE_NUM		4
#define HDMI21_MIN_FRL_LANE_RATE	3
#define HDMI21_MIN_FRL_LANE_NUM		3

struct rockchip_hdmi_qp {
	struct device *dev;
	struct regmap *regmap;
	struct regmap *vo_regmap;
	struct rockchip_encoder encoder;
	struct drm_connector *connector;
	struct dw_hdmi_qp *hdmi;
	struct phy *phy;
	struct dw_hdmi_qp_link_cfg link_cfg;
	struct gpio_desc *frl_enable_gpio;
	struct delayed_work hpd_work;
	int port_id;
	const struct rockchip_hdmi_qp_ctrl_ops *ctrl_ops;
};

struct rockchip_hdmi_qp_ctrl_ops {
	void (*io_init)(struct rockchip_hdmi_qp *hdmi);
	void (*enc_init)(struct rockchip_hdmi_qp *hdmi, struct rockchip_crtc_state *state);
	irqreturn_t (*irq_callback)(int irq, void *dev_id);
	irqreturn_t (*hardirq_callback)(int irq, void *dev_id);
};

static struct rockchip_hdmi_qp *to_rockchip_hdmi_qp(struct drm_encoder *encoder)
{
	struct rockchip_encoder *rkencoder = to_rockchip_encoder(encoder);

	return container_of(rkencoder, struct rockchip_hdmi_qp, encoder);
}

static void dw_hdmi_qp_rockchip_encoder_enable(struct drm_encoder *encoder)
{
	struct rockchip_hdmi_qp *hdmi = to_rockchip_hdmi_qp(encoder);
	const struct dw_hdmi_qp_link_cfg *lcfg = &hdmi->link_cfg;
	struct drm_crtc *crtc = encoder->crtc;
	struct rockchip_crtc_state *rks;

	gpiod_set_value_cansleep(hdmi->frl_enable_gpio, lcfg->frl_enabled);

	if (!crtc || !crtc->state)
		return;

	rks = to_rockchip_crtc_state(crtc->state);

	if (hdmi->ctrl_ops->enc_init)
		hdmi->ctrl_ops->enc_init(hdmi, rks);

	dev_dbg(hdmi->dev, "%s port=%d tmds=%llu frl=%ux%u bpc=%u/%u\n",
		__func__, hdmi->port_id, lcfg->tmds_char_rate,
		lcfg->frl_rate_per_lane, lcfg->frl_lanes,
		rks->output_bpc, lcfg->bpc);
}

static int
dw_hdmi_qp_rockchip_encoder_atomic_check(struct drm_encoder *encoder,
					 struct drm_crtc_state *crtc_state,
					 struct drm_connector_state *conn_state)
{
	const struct drm_display_info *info = &conn_state->connector->display_info;
	struct rockchip_crtc_state *s = to_rockchip_crtc_state(crtc_state);
	struct rockchip_hdmi_qp *hdmi = to_rockchip_hdmi_qp(encoder);
	struct dw_hdmi_qp_link_cfg *lcfg = &hdmi->link_cfg;
	union phy_configure_opts phy_cfg = {};
	enum phy_hdmi_mode mode;
	int ret;

	if (lcfg->tmds_char_rate == conn_state->hdmi.tmds_char_rate &&
	    s->output_bpc == conn_state->hdmi.output_bpc)
		return 0;

	phy_cfg.hdmi.bpc = conn_state->hdmi.output_bpc;

	if (conn_state->hdmi.tmds_char_rate > HDMI20_MAX_TMDS_RATE ||
	    (info->max_tmds_clock &&
	     conn_state->hdmi.tmds_char_rate > info->max_tmds_clock * 1000)) {
		mode = PHY_HDMI_MODE_FRL;

		if (info->hdmi.max_frl_rate_per_lane > lcfg->max_frl_rate_per_lane)
			phy_cfg.hdmi.frl.rate_per_lane = lcfg->max_frl_rate_per_lane;
		else
			phy_cfg.hdmi.frl.rate_per_lane = info->hdmi.max_frl_rate_per_lane;

		if (info->hdmi.max_lanes > lcfg->max_frl_lanes)
			phy_cfg.hdmi.frl.lanes = lcfg->max_frl_lanes;
		else
			phy_cfg.hdmi.frl.lanes = info->hdmi.max_lanes;
	} else {
		mode = PHY_HDMI_MODE_TMDS;

		phy_cfg.hdmi.tmds_char_rate = conn_state->hdmi.tmds_char_rate;
	}

	ret = phy_set_mode_ext(hdmi->phy, PHY_MODE_HDMI, mode);
	if (ret) {
		dev_err(hdmi->dev, "Failed to switch phy mode: %d\n", ret);
		return ret;
	}

	ret = phy_configure(hdmi->phy, &phy_cfg);
	if (ret) {
		dev_err(hdmi->dev, "Failed to configure phy: %d\n", ret);
		return ret;
	}

	lcfg->tmds_char_rate = conn_state->hdmi.tmds_char_rate;

	if (mode == PHY_HDMI_MODE_FRL) {
		lcfg->frl_enabled = true;
		lcfg->frl_rate_per_lane = phy_cfg.hdmi.frl.rate_per_lane;
		lcfg->frl_lanes = phy_cfg.hdmi.frl.lanes;
	} else {
		lcfg->frl_enabled = false;
		lcfg->frl_rate_per_lane = 0;
		lcfg->frl_lanes = 0;
	}

	lcfg->bpc = phy_cfg.hdmi.bpc;

	s->output_mode = ROCKCHIP_OUT_MODE_AAAA;
	s->output_type = DRM_MODE_CONNECTOR_HDMIA;
	s->output_bpc = conn_state->hdmi.output_bpc;
	s->frl_enabled = lcfg->frl_enabled;

	return 0;
}

static const struct
drm_encoder_helper_funcs dw_hdmi_qp_rockchip_encoder_helper_funcs = {
	.enable		= dw_hdmi_qp_rockchip_encoder_enable,
	.atomic_check	= dw_hdmi_qp_rockchip_encoder_atomic_check,
};

static int dw_hdmi_qp_rk3588_phy_init(struct dw_hdmi_qp *dw_hdmi, void *data)
{
	struct rockchip_hdmi_qp *hdmi = (struct rockchip_hdmi_qp *)data;

	return phy_power_on(hdmi->phy);
}

static void dw_hdmi_qp_rk3588_phy_disable(struct dw_hdmi_qp *dw_hdmi,
					  void *data)
{
	struct rockchip_hdmi_qp *hdmi = (struct rockchip_hdmi_qp *)data;

	phy_power_off(hdmi->phy);
}

static enum drm_connector_status
dw_hdmi_qp_rk3588_read_hpd(struct dw_hdmi_qp *dw_hdmi, void *data)
{
	struct rockchip_hdmi_qp *hdmi = (struct rockchip_hdmi_qp *)data;
	u32 val;

	regmap_read(hdmi->regmap, RK3588_GRF_SOC_STATUS1, &val);
	val &= hdmi->port_id ? RK3588_HDMI1_LEVEL_INT : RK3588_HDMI0_LEVEL_INT;

	return val ? connector_status_connected : connector_status_disconnected;
}

static void dw_hdmi_qp_rk3588_setup_hpd(struct dw_hdmi_qp *dw_hdmi, void *data)
{
	struct rockchip_hdmi_qp *hdmi = (struct rockchip_hdmi_qp *)data;
	u32 val;

	if (hdmi->port_id)
		val = (FIELD_PREP_WM16(RK3588_HDMI1_HPD_INT_CLR, 1) |
		       FIELD_PREP_WM16(RK3588_HDMI1_HPD_INT_MSK, 0));
	else
		val = (FIELD_PREP_WM16(RK3588_HDMI0_HPD_INT_CLR, 1) |
		       FIELD_PREP_WM16(RK3588_HDMI0_HPD_INT_MSK, 0));

	regmap_write(hdmi->regmap, RK3588_GRF_SOC_CON2, val);
}

static const struct dw_hdmi_qp_link_cfg *
dw_hdmi_qp_rk3588_get_link_cfg(struct dw_hdmi_qp *dw_hdmi, void *data)
{
	struct rockchip_hdmi_qp *hdmi = (struct rockchip_hdmi_qp *)data;

	return &hdmi->link_cfg;
}

static int dw_hdmi_qp_rk3588_set_frl_rate(struct dw_hdmi_qp *dw_hdmi, void *data,
					  u8 rate_per_lane, u8 lanes)
{
	struct rockchip_hdmi_qp *hdmi = (struct rockchip_hdmi_qp *)data;
	union phy_configure_opts phy_cfg = {};
	int ret;

	if (!hdmi->link_cfg.frl_enabled || !rate_per_lane || !lanes)
		return -EINVAL;

	phy_cfg.hdmi.frl.rate_per_lane = rate_per_lane;
	phy_cfg.hdmi.frl.lanes = lanes;
	phy_cfg.hdmi.bpc = hdmi->link_cfg.bpc;

	ret = phy_configure(hdmi->phy, &phy_cfg);
	if (ret) {
		dev_err(hdmi->dev, "Failed to set PHY FRL rate: %d\n", ret);
	} else {
		hdmi->link_cfg.frl_rate_per_lane = rate_per_lane;
		hdmi->link_cfg.frl_lanes = lanes;
	}

	return ret;
}

static const struct dw_hdmi_qp_phy_ops rk3588_hdmi_phy_ops = {
	.init		= dw_hdmi_qp_rk3588_phy_init,
	.disable	= dw_hdmi_qp_rk3588_phy_disable,
	.read_hpd	= dw_hdmi_qp_rk3588_read_hpd,
	.setup_hpd	= dw_hdmi_qp_rk3588_setup_hpd,
	.get_link_cfg	= dw_hdmi_qp_rk3588_get_link_cfg,
	.set_frl_rate	= dw_hdmi_qp_rk3588_set_frl_rate,
};

static enum drm_connector_status
dw_hdmi_qp_rk3576_read_hpd(struct dw_hdmi_qp *dw_hdmi, void *data)
{
	struct rockchip_hdmi_qp *hdmi = (struct rockchip_hdmi_qp *)data;
	u32 val;

	regmap_read(hdmi->regmap, RK3576_IOC_HDMI_HPD_STATUS, &val);

	return val & RK3576_HDMI_LEVEL_INT ?
		connector_status_connected : connector_status_disconnected;
}

static void dw_hdmi_qp_rk3576_setup_hpd(struct dw_hdmi_qp *dw_hdmi, void *data)
{
	struct rockchip_hdmi_qp *hdmi = (struct rockchip_hdmi_qp *)data;
	u32 val;

	val = (FIELD_PREP_WM16(RK3576_HDMI_HPD_INT_CLR, 1) |
	       FIELD_PREP_WM16(RK3576_HDMI_HPD_INT_MSK, 0));

	regmap_write(hdmi->regmap, RK3576_IOC_MISC_CON0, val);
	regmap_write(hdmi->regmap, 0xa404, 0xffff0102);
}

static const struct dw_hdmi_qp_phy_ops rk3576_hdmi_phy_ops = {
	.init		= dw_hdmi_qp_rk3588_phy_init,
	.disable	= dw_hdmi_qp_rk3588_phy_disable,
	.read_hpd	= dw_hdmi_qp_rk3576_read_hpd,
	.setup_hpd	= dw_hdmi_qp_rk3576_setup_hpd,
	.get_link_cfg	= dw_hdmi_qp_rk3588_get_link_cfg,
	.set_frl_rate	= dw_hdmi_qp_rk3588_set_frl_rate,
};

static void dw_hdmi_qp_rk3588_hpd_work(struct work_struct *work)
{
	struct rockchip_hdmi_qp *hdmi = container_of(work,
						     struct rockchip_hdmi_qp,
						     hpd_work.work);
	bool changed;

	if (hdmi->connector) {
		changed = drm_connector_helper_hpd_irq_event(hdmi->connector);
		if (changed)
			dev_dbg(hdmi->dev, "connector status changed\n");
	}
}

static irqreturn_t dw_hdmi_qp_rk3576_hardirq(int irq, void *dev_id)
{
	struct rockchip_hdmi_qp *hdmi = dev_id;
	u32 intr_stat, val;

	regmap_read(hdmi->regmap, RK3576_IOC_HDMI_HPD_STATUS, &intr_stat);
	if (intr_stat) {
		val = FIELD_PREP_WM16(RK3576_HDMI_HPD_INT_MSK, 1);

		regmap_write(hdmi->regmap, RK3576_IOC_MISC_CON0, val);
		return IRQ_WAKE_THREAD;
	}

	return IRQ_NONE;
}

static irqreturn_t dw_hdmi_qp_rk3576_irq(int irq, void *dev_id)
{
	struct rockchip_hdmi_qp *hdmi = dev_id;
	u32 val;

	val = FIELD_PREP_WM16(RK3576_HDMI_HPD_INT_CLR, 1);
	regmap_write(hdmi->regmap, RK3576_IOC_MISC_CON0, val);
	mod_delayed_work(system_percpu_wq, &hdmi->hpd_work,
			 msecs_to_jiffies(HOTPLUG_DEBOUNCE_MS));

	val = FIELD_PREP_WM16(RK3576_HDMI_HPD_INT_MSK, 0);
	regmap_write(hdmi->regmap, RK3576_IOC_MISC_CON0, val);

	return IRQ_HANDLED;
}

static irqreturn_t dw_hdmi_qp_rk3588_hardirq(int irq, void *dev_id)
{
	struct rockchip_hdmi_qp *hdmi = dev_id;
	u32 intr_stat, val;

	regmap_read(hdmi->regmap, RK3588_GRF_SOC_STATUS1, &intr_stat);

	if (intr_stat) {
		if (hdmi->port_id)
			val = FIELD_PREP_WM16(RK3588_HDMI1_HPD_INT_MSK, 1);
		else
			val = FIELD_PREP_WM16(RK3588_HDMI0_HPD_INT_MSK, 1);
		regmap_write(hdmi->regmap, RK3588_GRF_SOC_CON2, val);
		return IRQ_WAKE_THREAD;
	}

	return IRQ_NONE;
}

static irqreturn_t dw_hdmi_qp_rk3588_irq(int irq, void *dev_id)
{
	struct rockchip_hdmi_qp *hdmi = dev_id;
	u32 val;

	if (hdmi->port_id)
		val = FIELD_PREP_WM16(RK3588_HDMI1_HPD_INT_CLR, 1);
	else
		val = FIELD_PREP_WM16(RK3588_HDMI0_HPD_INT_CLR, 1);
	regmap_write(hdmi->regmap, RK3588_GRF_SOC_CON2, val);

	mod_delayed_work(system_percpu_wq, &hdmi->hpd_work,
			 msecs_to_jiffies(HOTPLUG_DEBOUNCE_MS));

	if (hdmi->port_id)
		val |= FIELD_PREP_WM16(RK3588_HDMI1_HPD_INT_MSK, 0);
	else
		val |= FIELD_PREP_WM16(RK3588_HDMI0_HPD_INT_MSK, 0);
	regmap_write(hdmi->regmap, RK3588_GRF_SOC_CON2, val);

	return IRQ_HANDLED;
}

static void dw_hdmi_qp_rk3576_io_init(struct rockchip_hdmi_qp *hdmi)
{
	u32 val;

	val = FIELD_PREP_WM16(RK3576_SCLIN_MASK, 1) |
	      FIELD_PREP_WM16(RK3576_SDAIN_MASK, 1) |
	      FIELD_PREP_WM16(RK3576_HDMI_GRANT_SEL, 1) |
	      FIELD_PREP_WM16(RK3576_I2S_SEL_MASK, 1);

	regmap_write(hdmi->vo_regmap, RK3576_VO0_GRF_SOC_CON14, val);

	val = FIELD_PREP_WM16(RK3576_HDMI_HPD_INT_MSK, 0);
	regmap_write(hdmi->regmap, RK3576_IOC_MISC_CON0, val);
}

static void dw_hdmi_qp_rk3588_io_init(struct rockchip_hdmi_qp *hdmi)
{
	u32 val;

	val = FIELD_PREP_WM16(RK3588_SCLIN_MASK, 1) |
	      FIELD_PREP_WM16(RK3588_SDAIN_MASK, 1) |
	      FIELD_PREP_WM16(RK3588_MODE_MASK, 1) |
	      FIELD_PREP_WM16(RK3588_I2S_SEL_MASK, 1);
	regmap_write(hdmi->vo_regmap,
		     hdmi->port_id ? RK3588_GRF_VO1_CON6 : RK3588_GRF_VO1_CON3,
		     val);

	val = FIELD_PREP_WM16(RK3588_HPD_HDMI0_IO_EN_MASK, 1) |
	      FIELD_PREP_WM16(RK3588_HPD_HDMI1_IO_EN_MASK, 1);
	regmap_write(hdmi->regmap, RK3588_GRF_SOC_CON7, val);

	if (hdmi->port_id)
		val = FIELD_PREP_WM16(RK3588_HDMI1_GRANT_SEL, 1);
	else
		val = FIELD_PREP_WM16(RK3588_HDMI0_GRANT_SEL, 1);
	regmap_write(hdmi->vo_regmap, RK3588_GRF_VO1_CON9, val);

	if (hdmi->port_id)
		val = FIELD_PREP_WM16(RK3588_HDMI1_HPD_INT_MSK, 1);
	else
		val = FIELD_PREP_WM16(RK3588_HDMI0_HPD_INT_MSK, 1);
	regmap_write(hdmi->regmap, RK3588_GRF_SOC_CON2, val);
}

static void dw_hdmi_qp_rk3576_enc_init(struct rockchip_hdmi_qp *hdmi,
				       struct rockchip_crtc_state *state)
{
	u32 val;

	val = FIELD_PREP_WM16(RK3576_HDMI_FRL_MOD, hdmi->link_cfg.frl_enabled);
	regmap_write(hdmi->vo_regmap, RK3576_VO0_GRF_SOC_CON1, val);

	if (state->output_bpc == 10)
		val = FIELD_PREP_WM16(RK3576_COLOR_DEPTH_MASK, RK3576_10BPC);
	else
		val = FIELD_PREP_WM16(RK3576_COLOR_DEPTH_MASK, RK3576_8BPC);

	regmap_write(hdmi->vo_regmap, RK3576_VO0_GRF_SOC_CON8, val);
}

static void dw_hdmi_qp_rk3588_enc_init(struct rockchip_hdmi_qp *hdmi,
				       struct rockchip_crtc_state *state)
{
	u32 val;

	val = FIELD_PREP_WM16(RK3588_HDMI21_MASK, hdmi->link_cfg.frl_enabled);
	regmap_write(hdmi->vo_regmap,
		     hdmi->port_id ? RK3588_GRF_VO1_CON7 : RK3588_GRF_VO1_CON4,
		     val);

	if (state->output_bpc == 10)
		val = FIELD_PREP_WM16(RK3588_COLOR_DEPTH_MASK, RK3588_10BPC);
	else
		val = FIELD_PREP_WM16(RK3588_COLOR_DEPTH_MASK, RK3588_8BPC);

	regmap_write(hdmi->vo_regmap,
		     hdmi->port_id ? RK3588_GRF_VO1_CON6 : RK3588_GRF_VO1_CON3,
		     val);
}

static const struct rockchip_hdmi_qp_ctrl_ops rk3576_hdmi_ctrl_ops = {
	.io_init		= dw_hdmi_qp_rk3576_io_init,
	.enc_init		= dw_hdmi_qp_rk3576_enc_init,
	.irq_callback		= dw_hdmi_qp_rk3576_irq,
	.hardirq_callback	= dw_hdmi_qp_rk3576_hardirq,
};

static const struct rockchip_hdmi_qp_ctrl_ops rk3588_hdmi_ctrl_ops = {
	.io_init		= dw_hdmi_qp_rk3588_io_init,
	.enc_init		= dw_hdmi_qp_rk3588_enc_init,
	.irq_callback		= dw_hdmi_qp_rk3588_irq,
	.hardirq_callback	= dw_hdmi_qp_rk3588_hardirq,
};

struct rockchip_hdmi_qp_cfg {
	unsigned int num_ports;
	unsigned int port_ids[MAX_HDMI_PORT_NUM];
	const struct rockchip_hdmi_qp_ctrl_ops *ctrl_ops;
	const struct dw_hdmi_qp_phy_ops *phy_ops;
	u8 max_frl_rate_per_lane;
	u8 max_frl_lanes;
	u8 min_frl_rate_per_lane;
	u8 min_frl_lanes;
};

static const struct rockchip_hdmi_qp_cfg rk3576_hdmi_cfg = {
	.num_ports = 1,
	.port_ids = {
		0x27da0000,
	},
	.ctrl_ops = &rk3576_hdmi_ctrl_ops,
	.phy_ops = &rk3576_hdmi_phy_ops,
};

static const struct rockchip_hdmi_qp_cfg rk3588_hdmi_cfg = {
	.num_ports = 2,
	.port_ids = {
		0xfde80000,
		0xfdea0000,
	},
	.ctrl_ops = &rk3588_hdmi_ctrl_ops,
	.phy_ops = &rk3588_hdmi_phy_ops,
	/* FIXME: Intermittent screen flicker if rate exceeds 40 Gbps */
	.max_frl_rate_per_lane = 10,
	.max_frl_lanes = 4,
};

static const struct of_device_id dw_hdmi_qp_rockchip_dt_ids[] = {
	{
		.compatible = "rockchip,rk3576-dw-hdmi-qp",
		.data = &rk3576_hdmi_cfg
	}, {
		.compatible = "rockchip,rk3588-dw-hdmi-qp",
		.data = &rk3588_hdmi_cfg
	},
	{},
};
MODULE_DEVICE_TABLE(of, dw_hdmi_qp_rockchip_dt_ids);

static int dw_hdmi_qp_rockchip_bind(struct device *dev, struct device *master,
				    void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct dw_hdmi_qp_plat_data plat_data = {};
	const struct rockchip_hdmi_qp_cfg *cfg;
	struct dw_hdmi_qp_link_cfg *lcfg;
	struct drm_device *drm = data;
	struct drm_encoder *encoder;
	struct rockchip_hdmi_qp *hdmi;
	struct resource *res;
	struct clk_bulk_data *clks;
	struct clk *ref_clk;
	int ret, irq, i;

	if (!dev->of_node)
		return -ENODEV;

	hdmi = drmm_kzalloc(drm, sizeof(*hdmi), GFP_KERNEL);
	if (!hdmi)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	cfg = of_device_get_match_data(dev);
	if (!cfg)
		return -ENODEV;

	if (!cfg->ctrl_ops || !cfg->ctrl_ops->io_init ||
	    !cfg->ctrl_ops->irq_callback || !cfg->ctrl_ops->hardirq_callback)
		return dev_err_probe(dev, -ENODEV, "Missing platform ctrl ops\n");

	hdmi->ctrl_ops = cfg->ctrl_ops;
	hdmi->dev = dev;
	hdmi->port_id = -ENODEV;

	/* Identify port ID by matching base IO address */
	for (i = 0; i < cfg->num_ports; i++) {
		if (res->start == cfg->port_ids[i]) {
			hdmi->port_id = i;
			break;
		}
	}
	if (hdmi->port_id < 0)
		return dev_err_probe(dev, hdmi->port_id,
				     "Failed to match HDMI port ID\n");

	plat_data.phy_ops = cfg->phy_ops;
	plat_data.phy_data = hdmi;
	plat_data.max_bpc = 10;

	encoder = &hdmi->encoder.encoder;
	encoder->possible_crtcs = drm_of_find_possible_crtcs(drm, dev->of_node);

	rockchip_drm_encoder_set_crtc_endpoint_id(&hdmi->encoder,
						  dev->of_node, 0, 0);
	/*
	 * If we failed to find the CRTC(s) which this encoder is
	 * supposed to be connected to, it's because the CRTC has
	 * not been registered yet.  Defer probing, and hope that
	 * the required CRTC is added later.
	 */
	if (encoder->possible_crtcs == 0)
		return -EPROBE_DEFER;

	hdmi->regmap = syscon_regmap_lookup_by_phandle(dev->of_node,
						       "rockchip,grf");
	if (IS_ERR(hdmi->regmap))
		return dev_err_probe(dev, PTR_ERR(hdmi->regmap),
				     "Unable to get rockchip,grf\n");

	hdmi->vo_regmap = syscon_regmap_lookup_by_phandle(dev->of_node,
							  "rockchip,vo-grf");
	if (IS_ERR(hdmi->vo_regmap))
		return dev_err_probe(dev, PTR_ERR(hdmi->vo_regmap),
				     "Unable to get rockchip,vo-grf\n");

	ret = devm_clk_bulk_get_all_enabled(dev, &clks);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get clocks\n");

	ref_clk = clk_get(dev, "ref");
	if (IS_ERR(ref_clk))
		return dev_err_probe(dev, PTR_ERR(ref_clk),
				     "Failed to get ref clock\n");

	plat_data.ref_clk_rate = clk_get_rate(ref_clk);
	clk_put(ref_clk);

	hdmi->frl_enable_gpio = devm_gpiod_get_optional(dev, "frl-enable",
							GPIOD_OUT_LOW);
	if (IS_ERR(hdmi->frl_enable_gpio))
		return dev_err_probe(dev, PTR_ERR(hdmi->frl_enable_gpio),
				     "Failed to request FRL enable GPIO\n");

	hdmi->phy = devm_of_phy_get_by_index(dev, dev->of_node, 0);
	if (IS_ERR(hdmi->phy))
		return dev_err_probe(dev, PTR_ERR(hdmi->phy), "Failed to get phy\n");

	lcfg = &hdmi->link_cfg;
	lcfg->max_frl_rate_per_lane = cfg->max_frl_rate_per_lane ?: HDMI21_MAX_FRL_LANE_RATE;
	lcfg->max_frl_lanes = cfg->max_frl_lanes ?: HDMI21_MAX_FRL_LANE_NUM;
	lcfg->min_frl_rate_per_lane = cfg->min_frl_rate_per_lane ?: HDMI21_MIN_FRL_LANE_RATE;
	lcfg->min_frl_lanes = cfg->min_frl_lanes ?: HDMI21_MIN_FRL_LANE_NUM;

	if (lcfg->max_frl_rate_per_lane < lcfg->min_frl_rate_per_lane ||
	    lcfg->max_frl_lanes < lcfg->min_frl_lanes ||
	    lcfg->max_frl_rate_per_lane > HDMI21_MAX_FRL_LANE_RATE ||
	    lcfg->max_frl_rate_per_lane < HDMI21_MIN_FRL_LANE_RATE ||
	    lcfg->max_frl_lanes > HDMI21_MAX_FRL_LANE_NUM ||
	    lcfg->max_frl_lanes < HDMI21_MIN_FRL_LANE_NUM ||
	    lcfg->min_frl_rate_per_lane > HDMI21_MAX_FRL_LANE_RATE ||
	    lcfg->min_frl_rate_per_lane < HDMI21_MIN_FRL_LANE_RATE ||
	    lcfg->min_frl_lanes > HDMI21_MAX_FRL_LANE_NUM ||
	    lcfg->min_frl_lanes < HDMI21_MIN_FRL_LANE_NUM)
		return dev_err_probe(hdmi->dev, -EINVAL, "Invalid FRL config\n");

	cfg->ctrl_ops->io_init(hdmi);

	INIT_DELAYED_WORK(&hdmi->hpd_work, dw_hdmi_qp_rk3588_hpd_work);

	plat_data.main_irq = platform_get_irq_byname(pdev, "main");
	if (plat_data.main_irq < 0)
		return plat_data.main_irq;

	plat_data.cec_irq = platform_get_irq_byname(pdev, "cec");
	if (plat_data.cec_irq < 0)
		return plat_data.cec_irq;

	irq = platform_get_irq_byname(pdev, "hpd");
	if (irq < 0)
		return irq;

	drm_encoder_helper_add(encoder, &dw_hdmi_qp_rockchip_encoder_helper_funcs);
	ret = drmm_encoder_init(drm, encoder, NULL, DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to init encoder\n");

	platform_set_drvdata(pdev, hdmi);

	hdmi->hdmi = dw_hdmi_qp_bind(pdev, encoder, &plat_data);
	if (IS_ERR(hdmi->hdmi))
		return dev_err_probe(dev, PTR_ERR(hdmi->hdmi),
				     "Failed to bind dw-hdmi-qp\n");

	hdmi->connector = drm_bridge_connector_init(drm, encoder);
	if (IS_ERR(hdmi->connector))
		return dev_err_probe(dev, PTR_ERR(hdmi->connector),
				     "Failed to init bridge connector\n");

	ret = drm_connector_attach_encoder(hdmi->connector, encoder);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to attach connector\n");

	return devm_request_threaded_irq(dev, irq,
					 cfg->ctrl_ops->hardirq_callback,
					 cfg->ctrl_ops->irq_callback,
					 IRQF_SHARED, "dw-hdmi-qp-hpd",
					 hdmi);
}

static void dw_hdmi_qp_rockchip_unbind(struct device *dev,
				       struct device *master,
				       void *data)
{
	struct rockchip_hdmi_qp *hdmi = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&hdmi->hpd_work);

	hdmi->connector = NULL;
}

static const struct component_ops dw_hdmi_qp_rockchip_ops = {
	.bind	= dw_hdmi_qp_rockchip_bind,
	.unbind	= dw_hdmi_qp_rockchip_unbind,
};

static int dw_hdmi_qp_rockchip_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &dw_hdmi_qp_rockchip_ops);
}

static void dw_hdmi_qp_rockchip_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &dw_hdmi_qp_rockchip_ops);
}

static int __maybe_unused dw_hdmi_qp_rockchip_suspend(struct device *dev)
{
	struct rockchip_hdmi_qp *hdmi = dev_get_drvdata(dev);

	dw_hdmi_qp_suspend(dev, hdmi->hdmi);

	return 0;
}

static int __maybe_unused dw_hdmi_qp_rockchip_resume(struct device *dev)
{
	struct rockchip_hdmi_qp *hdmi = dev_get_drvdata(dev);

	hdmi->ctrl_ops->io_init(hdmi);

	dw_hdmi_qp_resume(dev, hdmi->hdmi);

	if (hdmi->connector)
		drm_connector_helper_hpd_irq_event(hdmi->connector);

	return 0;
}

static const struct dev_pm_ops dw_hdmi_qp_rockchip_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(dw_hdmi_qp_rockchip_suspend,
				dw_hdmi_qp_rockchip_resume)
};

struct platform_driver dw_hdmi_qp_rockchip_pltfm_driver = {
	.probe = dw_hdmi_qp_rockchip_probe,
	.remove = dw_hdmi_qp_rockchip_remove,
	.driver = {
		.name = "dwhdmiqp-rockchip",
		.pm = &dw_hdmi_qp_rockchip_pm,
		.of_match_table = dw_hdmi_qp_rockchip_dt_ids,
	},
};
