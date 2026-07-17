// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 * Author: Chaoyi Chen <chaoyi.chen@rock-chips.com>
 */
#include <linux/of.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_dp.h>

#include <drm/bridge/aux-bridge.h>

static int drm_typec_bus_event(struct notifier_block *nb,
			       unsigned long action, void *data)
{
	struct device *dev = (struct device *)data;
	struct typec_altmode *alt = to_typec_altmode(dev);

	if (action != BUS_NOTIFY_ADD_DEVICE)
		return NOTIFY_OK;

	/*
	 * alt->dev.parent->parent : USB-C controller device
	 * alt->dev.parent         : USB-C connector device
	 */
	if (is_typec_port_altmode(&alt->dev) && alt->svid == USB_TYPEC_DP_SID)
		drm_dp_hpd_bridge_register(alt->dev.parent->parent,
					   to_of_node(alt->dev.parent->fwnode));

	return NOTIFY_OK;
}

static struct notifier_block drm_typec_event_nb = {
	.notifier_call = drm_typec_bus_event,
};

static void drm_aux_hpd_typec_dp_bridge_module_exit(void)
{
	bus_unregister_notifier(&typec_bus, &drm_typec_event_nb);
}

static int __init drm_aux_hpd_typec_dp_bridge_module_init(void)
{
	bus_register_notifier(&typec_bus, &drm_typec_event_nb);

	return 0;
}

module_init(drm_aux_hpd_typec_dp_bridge_module_init);
module_exit(drm_aux_hpd_typec_dp_bridge_module_exit);

MODULE_AUTHOR("Chaoyi Chen <chaoyi.chen@rock-chips.com>");
MODULE_DESCRIPTION("DRM TYPEC DP HPD BRIDGE");
MODULE_LICENSE("GPL");
