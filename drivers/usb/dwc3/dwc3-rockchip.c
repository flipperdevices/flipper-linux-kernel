// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, Collabora Ltd. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include "glue.h"

struct dwc3_rockchip {
	struct dwc3		dwc;
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
	dwc_rk->dwc.glue_ops = NULL;

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
