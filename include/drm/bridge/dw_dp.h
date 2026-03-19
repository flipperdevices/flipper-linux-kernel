/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2025 Rockchip Electronics Co., Ltd.
 */

#ifndef __DW_DP__
#define __DW_DP__

#include <linux/device.h>

struct drm_encoder;
struct dw_dp;

enum {
	DW_DP_MP_SINGLE_PIXEL,
	DW_DP_MP_DUAL_PIXEL,
	DW_DP_MP_QUAD_PIXEL,
};

struct dw_dp_plat_data {
	u32 max_link_rate;
	u8 pixel_mode;
	void *data;
	void (*hpd_sw_sel)(void *data, bool hpd);
	void (*hpd_sw_cfg)(void *data, bool hpd);
};

int dw_dp_bind(struct dw_dp *dp, struct drm_encoder *encoder);
void dw_dp_unbind(struct dw_dp *dp);

struct dw_dp *dw_dp_probe(struct platform_device *pdev, const struct dw_dp_plat_data *plat_data);

int dw_dp_runtime_suspend(struct dw_dp *dp);
int dw_dp_runtime_resume(struct dw_dp *dp);
#endif /* __DW_DP__ */
