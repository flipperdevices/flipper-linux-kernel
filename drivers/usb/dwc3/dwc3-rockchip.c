// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, Collabora Ltd. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/phy/rockchip-usbdp.h>
#include <linux/pm_runtime.h>
#include "glue.h"
#include "io.h"

struct dwc3_rockchip {
	struct dwc3		dwc;
	u8			phy_reset_active;
	u8			phy_reset_pm_ref;
	struct task_struct	*pm_task;
};

static void dwc3_rockchip_phy_pre_reset(struct dwc3_rockchip *dwc_rk, int port)
{
	struct dwc3 *dwc = &dwc_rk->dwc;
	bool pm_ref = false;
	u32 reg;
	int ret;

	if (dwc_rk->pm_task != current) {
		ret = pm_runtime_get_if_active(dwc->dev);
		if (!ret)
			return;

		pm_ref = ret > 0;
	}

	/*
	 * Assert USB3 PHY soft reset within DWC3 before the external
	 * PHY resets. This disconnects the PIPE interface, preventing
	 * the DWC3 from interfering with PHY reinitialization and
	 * avoiding LCPLL lock failures.
	 */
	scoped_guard(spinlock_irqsave, &dwc->lock) {
		dwc_rk->phy_reset_active |= BIT(port);
		if (pm_ref)
			dwc_rk->phy_reset_pm_ref |= BIT(port);
		reg = dwc3_readl(dwc, DWC3_GUSB3PIPECTL(port));
		reg |= DWC3_GUSB3PIPECTL_PHYSOFTRST;
		dwc3_writel(dwc, DWC3_GUSB3PIPECTL(port), reg);
	}
}

static void dwc3_rockchip_phy_post_reset(struct dwc3_rockchip *dwc_rk, int port)
{
	struct dwc3 *dwc = &dwc_rk->dwc;
	bool pm_ref;
	u32 reg;

	scoped_guard(spinlock_irqsave, &dwc->lock) {
		if (!(dwc_rk->phy_reset_active & BIT(port)))
			return;

		dwc_rk->phy_reset_active &= ~BIT(port);
		pm_ref = dwc_rk->phy_reset_pm_ref & BIT(port);
		dwc_rk->phy_reset_pm_ref &= ~BIT(port);

		reg = dwc3_readl(dwc, DWC3_GUSB3PIPECTL(port));
		reg &= ~DWC3_GUSB3PIPECTL_PHYSOFTRST;
		dwc3_writel(dwc, DWC3_GUSB3PIPECTL(port), reg);
	}

	if (pm_ref)
		pm_runtime_put(dwc->dev);
}

static void dwc3_rockchip_phy_reset(void *data, int port, bool pre)
{
	struct dwc3_rockchip *dwc_rk = data;

	if (pre)
		dwc3_rockchip_phy_pre_reset(dwc_rk, port);
	else
		dwc3_rockchip_phy_post_reset(dwc_rk, port);
}

static void dwc3_rk_phy_unregister_notifiers(void *data)
{
	struct dwc3_rockchip *dwc_rk = data;
	struct dwc3 *dwc = &dwc_rk->dwc;
	int i;

	for (i = 0; i < dwc->num_usb3_ports; i++) {
		struct phy *phy = dwc->usb3_generic_phy[i];

		if (phy && !strcmp(dev_driver_string(phy->dev.parent), "rockchip-usbdp-phy"))
			rk_udphy_unregister_reset_handler(phy);
	}

	WARN_ON(dwc_rk->phy_reset_active || dwc_rk->phy_reset_pm_ref);
}

static int dwc3_rk_phy_register_reset_handler(struct dwc3 *dwc)
{
	struct dwc3_rockchip *dwc_rk = container_of(dwc, struct dwc3_rockchip, dwc);
	int i;

	for (i = 0; i < dwc->num_usb3_ports; i++) {
		struct phy *phy = dwc->usb3_generic_phy[i];

		if (phy && !strcmp(dev_driver_string(phy->dev.parent), "rockchip-usbdp-phy"))
			rk_udphy_register_reset_handler(phy, dwc3_rockchip_phy_reset, dwc_rk, i);
	}

	return devm_add_action_or_reset(dwc->dev, dwc3_rk_phy_unregister_notifiers, dwc_rk);
}

static struct dwc3_glue_ops dwc3_rockchip_glue_ops = {
	.post_phy_registration = dwc3_rk_phy_register_reset_handler,
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

static int dwc3_rockchip_runtime_suspend(struct device *dev)
{
	struct dwc3 *dwc = dev_get_drvdata(dev);
	struct dwc3_rockchip *dwc_rk = container_of(dwc, struct dwc3_rockchip, dwc);
	int ret;

	dwc_rk->pm_task = current;
	ret = dwc3_runtime_suspend(dwc);
	dwc_rk->pm_task = NULL;

	return ret;
}

static int dwc3_rockchip_runtime_resume(struct device *dev)
{
	struct dwc3 *dwc = dev_get_drvdata(dev);
	struct dwc3_rockchip *dwc_rk = container_of(dwc, struct dwc3_rockchip, dwc);
	int ret;

	dwc_rk->pm_task = current;
	ret = dwc3_runtime_resume(dwc);
	dwc_rk->pm_task = NULL;

	return ret;
}

static int dwc3_rockchip_runtime_idle(struct device *dev)
{
	return dwc3_runtime_idle(dev_get_drvdata(dev));
}

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

static const struct dev_pm_ops dwc3_rockchip_dev_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(dwc3_rockchip_suspend, dwc3_rockchip_resume)
	RUNTIME_PM_OPS(dwc3_rockchip_runtime_suspend, dwc3_rockchip_runtime_resume,
		       dwc3_rockchip_runtime_idle)
	.complete = pm_sleep_ptr(dwc3_rockchip_complete),
	.prepare = pm_sleep_ptr(dwc3_rockchip_prepare),
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
