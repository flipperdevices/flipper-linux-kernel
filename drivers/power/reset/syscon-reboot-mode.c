// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2016, Fuzhou Rockchip Electronics Co., Ltd
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/mfd/syscon.h>
#include <linux/reboot-mode.h>

struct syscon_reboot_mode {
	struct regmap *map;
	struct reboot_mode_driver reboot;
	u32 offset;
	u32 mask;
	struct regulator_bulk_data *supplies;
	int num_supplies;
};

static int syscon_reboot_mode_write(struct reboot_mode_driver *reboot,
				    unsigned int magic)
{
	struct syscon_reboot_mode *syscon_rbm;
	int ret;

	syscon_rbm = container_of(reboot, struct syscon_reboot_mode, reboot);

	/*
	 * Whatever acts on the mode (e.g. boot ROM) runs before the operating
	 * system, and may need supplies that the running system had powered
	 * down. Enable them here, and deliberately leave them enabled: the
	 * system is on its way down, and what runs next may not know how to
	 * turn them on.
	 */
	if (syscon_rbm->num_supplies) {
		ret = regulator_bulk_enable(syscon_rbm->num_supplies,
					    syscon_rbm->supplies);
		if (ret < 0)
			dev_err(reboot->dev, "enabling reboot mode supplies failed\n");
	}

	ret = regmap_update_bits(syscon_rbm->map, syscon_rbm->offset,
				 syscon_rbm->mask, magic);
	if (ret < 0)
		dev_err(reboot->dev, "update reboot mode bits failed\n");

	return ret;
}

static void syscon_reboot_mode_put_supplies(void *data)
{
	struct syscon_reboot_mode *syscon_rbm = data;

	regulator_bulk_free(syscon_rbm->num_supplies, syscon_rbm->supplies);
	kfree(syscon_rbm->supplies);
}

static int syscon_reboot_mode_probe(struct platform_device *pdev)
{
	int ret;
	struct syscon_reboot_mode *syscon_rbm;

	syscon_rbm = devm_kzalloc(&pdev->dev, sizeof(*syscon_rbm), GFP_KERNEL);
	if (!syscon_rbm)
		return -ENOMEM;

	syscon_rbm->reboot.dev = &pdev->dev;
	syscon_rbm->reboot.write = syscon_reboot_mode_write;
	syscon_rbm->mask = 0xffffffff;

	syscon_rbm->map = syscon_node_to_regmap(pdev->dev.parent->of_node);
	if (IS_ERR(syscon_rbm->map))
		return PTR_ERR(syscon_rbm->map);

	if (of_property_read_u32(pdev->dev.of_node, "offset",
	    &syscon_rbm->offset))
		return -EINVAL;

	of_property_read_u32(pdev->dev.of_node, "mask", &syscon_rbm->mask);

	ret = of_regulator_bulk_get_all(&pdev->dev, pdev->dev.of_node,
					&syscon_rbm->supplies);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret,
				     "can't get reboot mode supplies\n");

	syscon_rbm->num_supplies = ret;
	if (syscon_rbm->num_supplies) {
		ret = devm_add_action_or_reset(&pdev->dev,
					       syscon_reboot_mode_put_supplies,
					       syscon_rbm);
		if (ret)
			return ret;
	}

	ret = devm_reboot_mode_register(&pdev->dev, &syscon_rbm->reboot);
	if (ret)
		dev_err(&pdev->dev, "can't register reboot mode\n");

	return ret;
}

static const struct of_device_id syscon_reboot_mode_of_match[] = {
	{ .compatible = "syscon-reboot-mode" },
	{}
};
MODULE_DEVICE_TABLE(of, syscon_reboot_mode_of_match);

static struct platform_driver syscon_reboot_mode_driver = {
	.probe = syscon_reboot_mode_probe,
	.driver = {
		.name = "syscon-reboot-mode",
		.of_match_table = syscon_reboot_mode_of_match,
	},
};
module_platform_driver(syscon_reboot_mode_driver);

MODULE_AUTHOR("Andy Yan <andy.yan@rock-chips.com");
MODULE_DESCRIPTION("SYSCON reboot mode driver");
MODULE_LICENSE("GPL v2");
