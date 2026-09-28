/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Rockchip USBDP Combo PHY with Samsung IP block driver
 *
 * Copyright (C) 2026 Collabora Ltd
 */

#ifndef __ROCKCHIP_USBDP
#define __ROCKCHIP_USBDP

#include <linux/phy/phy.h>

#if IS_ENABLED(CONFIG_PHY_ROCKCHIP_USBDP)

void rk_udphy_unregister_reset_handler(struct phy *phy);
void rk_udphy_register_reset_handler(struct phy *phy,
				     void (*reset_handler)(void *data, int port, bool pre),
				     void *data, int port);

#else

static inline void rk_udphy_unregister_reset_handler(struct phy *phy)
{ }
static inline void rk_udphy_register_reset_handler(struct phy *phy,
					    void (*reset_handler)(void *data, int port, bool pre),
					    void *data, int port)
{ }

#endif

#endif
