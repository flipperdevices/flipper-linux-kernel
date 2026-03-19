// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 Rockchip Electronics Co., Ltd.
 *
 * Author: Zhang Yubing <yubing.zhang@rock-chips.com>
 * Author: Andy Yan <andy.yan@rock-chips.com>
 */

#include <linux/component.h>
#include <linux/hw_bitfield.h>
#include <linux/media-bus-format.h>
#include <linux/mfd/syscon.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/videodev2.h>

#include <drm/bridge/dw_dp.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_bridge_connector.h>
#include <drm/drm_managed.h>
#include <drm/drm_of.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

#include "rockchip_drm_drv.h"

#define ROCKCHIP_MAX_CTRLS 2

#define ROCKCHIP_VO_GRF_DP_SINK_HPD_SEL BIT(10)
#define ROCKCHIP_VO_GRF_DP_SINK_HPD_CFG BIT(11)

struct rockchip_dw_dp_plat_data {
	u8 num_ctrls;
	u64 ctrl_ids[ROCKCHIP_MAX_CTRLS];
	u32 max_link_rate;
	u8 pixel_mode;
	u32 hpd_reg[ROCKCHIP_MAX_CTRLS];
};

struct rockchip_dw_dp {
	struct dw_dp *base;
	struct device *dev;
	const struct rockchip_dw_dp_plat_data *pdata;
	struct regmap *vo_grf;
	struct rockchip_encoder *encoder;
	int id;
};

static void dw_dp_rockchip_hpd_sw_sel(void *data, bool force_hpd_from_sw)
{
	struct rockchip_dw_dp *dp = data;
	u32 hpd_reg = dp->pdata->hpd_reg[dp->id];

	regmap_write(dp->vo_grf, hpd_reg,
		     FIELD_PREP_WM16(ROCKCHIP_VO_GRF_DP_SINK_HPD_SEL, force_hpd_from_sw));
}

static void dw_dp_rockchip_hpd_sw_cfg(void *data, bool hpd)
{
	struct rockchip_dw_dp *dp = data;
	u32 hpd_reg = dp->pdata->hpd_reg[dp->id];

	dev_dbg(dp->dev, "Force HPD connected=%s\n", str_yes_no(hpd));

	regmap_write(dp->vo_grf, hpd_reg,
		     FIELD_PREP_WM16(ROCKCHIP_VO_GRF_DP_SINK_HPD_CFG, hpd));
}

static int dw_dp_encoder_atomic_check(struct drm_encoder *encoder,
				      struct drm_crtc_state *crtc_state,
				      struct drm_connector_state *conn_state)
{
	struct rockchip_crtc_state *s = to_rockchip_crtc_state(crtc_state);
	struct drm_atomic_commit *state = conn_state->state;
	struct drm_display_info *di = &conn_state->connector->display_info;
	struct drm_bridge *bridge  = drm_bridge_chain_get_first_bridge(encoder);
	struct drm_bridge_state *bridge_state = drm_atomic_get_new_bridge_state(state, bridge);
	u32 bus_format = bridge_state->input_bus_cfg.format;

	switch (bus_format) {
	case MEDIA_BUS_FMT_UYYVYY10_0_5X30:
	case MEDIA_BUS_FMT_UYYVYY8_0_5X24:
		s->output_mode = ROCKCHIP_OUT_MODE_YUV420;
		break;
	case MEDIA_BUS_FMT_YUYV10_1X20:
	case MEDIA_BUS_FMT_YUYV8_1X16:
		s->output_mode = ROCKCHIP_OUT_MODE_S888_DUMMY;
		break;
	case MEDIA_BUS_FMT_RGB101010_1X30:
	case MEDIA_BUS_FMT_RGB888_1X24:
	case MEDIA_BUS_FMT_RGB666_1X24_CPADHI:
	case MEDIA_BUS_FMT_YUV10_1X30:
	case MEDIA_BUS_FMT_YUV8_1X24:
	default:
		s->output_mode = ROCKCHIP_OUT_MODE_AAAA;
		break;
	}

	s->output_type = DRM_MODE_CONNECTOR_DisplayPort;
	s->bus_format = bus_format;
	s->bus_flags = di->bus_flags;
	s->color_space = V4L2_COLORSPACE_DEFAULT;

	return 0;
}

static const struct drm_encoder_helper_funcs dw_dp_encoder_helper_funcs = {
	.atomic_check		= dw_dp_encoder_atomic_check,
};

static struct regmap *dw_dp_rockchip_get_vo_grf(struct rockchip_dw_dp *dp)
{
	struct device_node *np = dev_of_node(dp->dev);
	struct of_phandle_args args;
	struct regmap *regmap;
	int ret;

	ret = of_parse_phandle_with_args(np, "phys", "#phy-cells", 0, &args);
	if (ret)
		return ERR_PTR(-ENODEV);

	/*
	 * Limit this workaround to RK3576 and RK3588, potential future platforms
	 * reusing the driver should just add a VO GRF phandle in the DisplayPort
	 * controller DT node.
	 */
	if (!of_device_is_compatible(args.np, "rockchip,rk3576-usbdp-phy") &&
	    !of_device_is_compatible(args.np, "rockchip,rk3588-usbdp-phy")) {
		regmap = ERR_PTR(-ENODEV);
		goto out_put_node;
	}

	regmap = syscon_regmap_lookup_by_phandle(args.np, "rockchip,vo-grf");

out_put_node:
	of_node_put(args.np);
	return regmap;
}

static int dw_dp_rockchip_bind(struct device *dev, struct device *master, void *data)
{
	struct rockchip_dw_dp *dp = dev_get_drvdata(dev);
	struct drm_device *drm_dev = data;
	struct drm_encoder *encoder;
	struct drm_connector *connector;
	int ret;

	dp->encoder = drmm_kzalloc(drm_dev, sizeof(*dp->encoder), GFP_KERNEL);
	if (!dp->encoder)
		return -ENOMEM;

	encoder = &dp->encoder->encoder;
	encoder->possible_crtcs = drm_of_find_possible_crtcs(drm_dev, dev->of_node);
	rockchip_drm_encoder_set_crtc_endpoint_id(dp->encoder, dev->of_node, 0, 0);

	ret = drmm_encoder_init(drm_dev, encoder, NULL, DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		return ret;
	drm_encoder_helper_add(encoder, &dw_dp_encoder_helper_funcs);

	ret = dw_dp_bind(dp->base, encoder);
	if (ret)
		return dev_err_probe(dev, ret, "failed to bind DW-DP bridge\n");

	connector = drm_bridge_connector_init(drm_dev, encoder);
	if (IS_ERR(connector)) {
		dw_dp_unbind(dp->base);
		return dev_err_probe(dev, PTR_ERR(connector),
				     "Failed to init bridge connector\n");
	}

	return 0;
}

static void dw_dp_rockchip_unbind(struct device *dev, struct device *master,
				  void *data)
{
	struct rockchip_dw_dp *dp = dev_get_drvdata(dev);

	dw_dp_unbind(dp->base);
}

static const struct component_ops dw_dp_rockchip_component_ops = {
	.bind = dw_dp_rockchip_bind,
	.unbind = dw_dp_rockchip_unbind,
};

static int dw_dp_rockchip_probe(struct platform_device *pdev)
{
	const struct rockchip_dw_dp_plat_data *plat_data_const;
	struct dw_dp_plat_data *plat_data;
	struct device *dev = &pdev->dev;
	struct rockchip_dw_dp *dp;
	struct resource *res;
	int id, ret;

	plat_data_const = device_get_match_data(dev);
	if (!plat_data_const)
		return -ENODEV;

	plat_data = devm_kzalloc(dev, sizeof(*plat_data), GFP_KERNEL);
	if (!plat_data)
		return -ENOMEM;

	dp = devm_kzalloc(dev, sizeof(*dp), GFP_KERNEL);
	if (!dp)
		return -ENOMEM;
	platform_set_drvdata(pdev, dp);
	dp->dev = dev;
	dp->pdata = plat_data_const;

	res = platform_get_mem_or_io(pdev, 0);
	if (!res)
		return -ENODEV;

	/* find the DisplayPort ID from the io address */
	dp->id = -ENODEV;
	for (id = 0; id < plat_data_const->num_ctrls; id++) {
		if (res->start == plat_data_const->ctrl_ids[id]) {
			dp->id = id;
			break;
		}
	}

	if (dp->id < 0)
		return dp->id;

	dp->vo_grf = dw_dp_rockchip_get_vo_grf(dp);
	if (IS_ERR(dp->vo_grf))
		return PTR_ERR(dp->vo_grf);

	plat_data->max_link_rate = plat_data_const->max_link_rate;
	plat_data->pixel_mode = plat_data_const->pixel_mode;
	plat_data->hpd_sw_sel = dw_dp_rockchip_hpd_sw_sel;
	plat_data->hpd_sw_cfg = dw_dp_rockchip_hpd_sw_cfg;
	plat_data->data = dp;

	dp->base = dw_dp_alloc(pdev, plat_data);
	if (IS_ERR(dp->base))
		return PTR_ERR(dp->base);

	ret = dw_dp_probe(dp->base);
	if (ret)
		return ret;

	return component_add(&pdev->dev, &dw_dp_rockchip_component_ops);
}

static void dw_dp_rockchip_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &dw_dp_rockchip_component_ops);
}

static const struct rockchip_dw_dp_plat_data rk3588_dp_plat_data = {
	.num_ctrls = 2,
	.ctrl_ids = {0xfde50000, 0xfde60000},
	.max_link_rate = 810000,
	.pixel_mode = DW_DP_MP_QUAD_PIXEL,
	.hpd_reg = {0x0000, 0x0008},
};

static const struct rockchip_dw_dp_plat_data rk3576_dp_plat_data = {
	.num_ctrls = 1,
	.ctrl_ids = {0x27e40000},
	.max_link_rate = 810000,
	.pixel_mode = DW_DP_MP_DUAL_PIXEL,
	.hpd_reg = {0x0000},
};

static const struct of_device_id dw_dp_of_match[] = {
	{
		.compatible = "rockchip,rk3588-dp",
		.data = &rk3588_dp_plat_data,
	}, {
		.compatible = "rockchip,rk3576-dp",
		.data = &rk3576_dp_plat_data,
	},
	{}
};
MODULE_DEVICE_TABLE(of, dw_dp_of_match);

struct platform_driver dw_dp_driver = {
	.probe	= dw_dp_rockchip_probe,
	.remove = dw_dp_rockchip_remove,
	.driver = {
		.name = "dw-dp",
		.of_match_table = dw_dp_of_match,
	},
};
