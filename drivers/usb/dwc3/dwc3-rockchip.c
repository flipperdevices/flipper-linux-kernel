// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, Collabora Ltd. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/phy/phy.h>
#include <linux/pm_runtime.h>
#include "glue.h"
#include "io.h"

struct dwc3_rockchip;

/**
 * struct dwc3_rk_phy_nb - wrapper for PHY notifier block
 * @nb: notifier block
 * @dwc: back-pointer to the DWC3 controller
 * @port_index: USB3 port index this notifier is registered for
 */
struct dwc3_rk_phy_nb {
	struct notifier_block	nb;
	struct dwc3_rockchip	*dwc_rk;
	u8			port_index;
};

struct dwc3_rockchip {
	struct dwc3		dwc;
	struct dwc3_rk_phy_nb	usb3_phy_nb[DWC3_USB3_MAX_PORTS];
	u8			phy_reset_active;
	enum usb_role		role;
};

static void dwc3_rockchip_vbus_handler(struct dwc3 *dwc, bool present)
{
	if (!dwc->gadget || !dwc->gadget_driver)
		return;

	usb_udc_vbus_handler(dwc->gadget, present);
}

static int dwc3_usb3_phy_notify(struct notifier_block *nb,
				unsigned long action, void *data)
{
	struct dwc3_rk_phy_nb *pnb = container_of(nb, struct dwc3_rk_phy_nb, nb);
	struct dwc3_rockchip *dwc_rk = pnb->dwc_rk;
	struct dwc3 *dwc = &dwc_rk->dwc;
	int port = pnb->port_index;
	unsigned long flags;
	u32 reg;
	int ret;

	switch (action) {
	case PHY_NOTIFY_PRE_RESET:
		/*
		 * If already suspended, the resume path will reinit GUSB3PIPECTL
		 * via dwc3_core_init(). A forced resume is not possible as that
		 * would call phy_init() resulting in a deadlock. Due to the
		 * phy_init() in the resume path there is also no need to block
		 * async RPM resume on our side, since the PHY synchronizes it
		 * for us.
		 *
		 * pm_runtime_get_if_active() returns 0 when suspended (skip),
		 * 1 when active (ref held), or -EINVAL when PM is disabled
		 * (device always active). In the -EINVAL case PM ref counting
		 * is a no-op, so the unconditional put in POST_RESET is safe.
		 */
		ret = pm_runtime_get_if_active(dwc->dev);
		if (!ret)
			return NOTIFY_OK;

		dwc3_rockchip_vbus_handler(dwc, false);

		/*
		 * Assert USB3 PHY soft reset within DWC3 before the external
		 * PHY resets. This disconnects the PIPE interface, preventing
		 * the DWC3 from interfering with PHY reinitialization and
		 * avoiding LCPLL lock failures.
		 */
		spin_lock_irqsave(&dwc->lock, flags);
		dwc_rk->phy_reset_active |= BIT(port);
		reg = dwc3_readl(dwc, DWC3_GUSB3PIPECTL(port));
		reg |= DWC3_GUSB3PIPECTL_PHYSOFTRST;
		dwc3_writel(dwc, DWC3_GUSB3PIPECTL(port), reg);
		spin_unlock_irqrestore(&dwc->lock, flags);

		break;

	case PHY_NOTIFY_POST_RESET:
		spin_lock_irqsave(&dwc->lock, flags);
		if (!(dwc_rk->phy_reset_active & BIT(port))) {
			spin_unlock_irqrestore(&dwc->lock, flags);
			return NOTIFY_OK;
		}

		dwc_rk->phy_reset_active &= ~BIT(port);

		/*
		 * Deassert PHY soft reset to reconnect the PIPE interface
		 * after PHY reinitialization.
		 */
		reg = dwc3_readl(dwc, DWC3_GUSB3PIPECTL(port));
		reg &= ~DWC3_GUSB3PIPECTL_PHYSOFTRST;
		dwc3_writel(dwc, DWC3_GUSB3PIPECTL(port), reg);
		spin_unlock_irqrestore(&dwc->lock, flags);

		dwc3_rockchip_vbus_handler(dwc, dwc_rk->role == USB_ROLE_DEVICE);

		pm_runtime_put_autosuspend(dwc->dev);
		break;
	}

	return NOTIFY_OK;
}

static void dwc3_rk_phy_unregister_notifiers(void *data)
{
	struct dwc3_rockchip *dwc_rk = data;
	struct dwc3 *dwc = &dwc_rk->dwc;
	int i;

	for (i = 0; i < dwc->num_usb3_ports; i++)
		phy_unregister_notifier(dwc->usb3_generic_phy[i],
					&dwc_rk->usb3_phy_nb[i].nb);

	/* Release any PM references from in-flight resets */
	for (i = 0; i < dwc->num_usb3_ports; i++) {
		if (dwc_rk->phy_reset_active & BIT(i))
			pm_runtime_put_autosuspend(dwc->dev);
	}
	dwc_rk->phy_reset_active = 0;
}

static int dwc3_rk_phy_register_notifiers(struct dwc3 *dwc)
{
	struct dwc3_rockchip *dwc_rk = container_of(dwc, struct dwc3_rockchip, dwc);
	int i;

	for (i = 0; i < dwc->num_usb3_ports; i++) {
		dwc_rk->usb3_phy_nb[i].nb.notifier_call = dwc3_usb3_phy_notify;
		dwc_rk->usb3_phy_nb[i].dwc_rk = dwc_rk;
		dwc_rk->usb3_phy_nb[i].port_index = i;
		phy_register_notifier(dwc->usb3_generic_phy[i],
				      &dwc_rk->usb3_phy_nb[i].nb);
	}

	return devm_add_action_or_reset(dwc->dev, dwc3_rk_phy_unregister_notifiers, dwc_rk);
}

static void dwc3_rockchip_set_role(struct dwc3 *dwc, enum usb_role role)
{
	struct dwc3_rockchip *dwc_rk = container_of(dwc, struct dwc3_rockchip, dwc);

	dwc_rk->role = role;
	dwc3_rockchip_vbus_handler(dwc, role == USB_ROLE_DEVICE);
}

static struct dwc3_glue_ops dwc3_rockchip_glue_ops = {
	.pre_set_role = dwc3_rockchip_set_role,
	.post_phy_registration = dwc3_rk_phy_register_notifiers,
};

static int dwc3_rockchip_probe(struct platform_device *pdev)
{
	struct dwc3_probe_data probe_data = {};
	struct resource *res;
	struct dwc3_rockchip *dwc_rk;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "missing memory resource\n");
		return -ENODEV;
	}

	dwc_rk = devm_kzalloc(&pdev->dev, sizeof(*dwc_rk), GFP_KERNEL);
	if (!dwc_rk)
		return -ENOMEM;

	dwc_rk->dwc.dev = &pdev->dev;
	dwc_rk->dwc.glue_ops = &dwc3_rockchip_glue_ops;

	probe_data.dwc = &dwc_rk->dwc;
	probe_data.res = res;
	probe_data.properties = DWC3_DEFAULT_PROPERTIES;

	return dwc3_core_probe(&probe_data);
}

static void dwc3_rockchip_remove(struct platform_device *pdev)
{
	dwc3_core_remove(platform_get_drvdata(pdev));
}

#ifdef CONFIG_PM
static int dwc3_rockchip_runtime_suspend(struct device *dev)
{
	return dwc3_runtime_suspend(dev_get_drvdata(dev));
}

static int dwc3_rockchip_runtime_resume(struct device *dev)
{
	return dwc3_runtime_resume(dev_get_drvdata(dev));
}

static int dwc3_rockchip_runtime_idle(struct device *dev)
{
	return dwc3_runtime_idle(dev_get_drvdata(dev));
}
#endif

#ifdef CONFIG_PM_SLEEP
static int dwc3_rockchip_suspend(struct device *dev)
{
	return dwc3_pm_suspend(dev_get_drvdata(dev));
}

static int dwc3_rockchip_resume(struct device *dev)
{
	return dwc3_pm_resume(dev_get_drvdata(dev));
}

static void dwc3_rockchip_complete(struct device *dev)
{
	dwc3_pm_complete(dev_get_drvdata(dev));
}

static int dwc3_rockchip_prepare(struct device *dev)
{
	return dwc3_pm_prepare(dev_get_drvdata(dev));
}
#endif

static const struct dev_pm_ops dwc3_rockchip_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(dwc3_rockchip_suspend, dwc3_rockchip_resume)
	.complete = dwc3_rockchip_complete,
	.prepare = dwc3_rockchip_prepare,
	/*
	 * Runtime suspend halts the controller on disconnection. It relies on
	 * platforms with custom connection notification to start the controller
	 * again.
	 */
	SET_RUNTIME_PM_OPS(dwc3_rockchip_runtime_suspend, dwc3_rockchip_runtime_resume,
			   dwc3_rockchip_runtime_idle)
};

static const struct of_device_id dwc3_rockchip_of_match[] = {
	{ .compatible = "rockchip,rk3588-dwc3" },
	{ .compatible = "rockchip,rk3576-dwc3" },
	{ }
};
MODULE_DEVICE_TABLE(of, dwc3_rockchip_of_match);

static struct platform_driver dwc3_rockchip_driver = {
	.probe		= dwc3_rockchip_probe,
	.remove		= dwc3_rockchip_remove,
	.driver		= {
		.name	= "dwc3-rockchip",
		.pm	= pm_ptr(&dwc3_rockchip_dev_pm_ops),
		.of_match_table	= dwc3_rockchip_of_match,
	},
};

module_platform_driver(dwc3_rockchip_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DesignWare DWC3 Rockchip Glue Driver");
