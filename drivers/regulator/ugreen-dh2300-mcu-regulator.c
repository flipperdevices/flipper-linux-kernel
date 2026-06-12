// SPDX-License-Identifier: GPL-2.0-only
/*
 * SATA drive-bay power gate for the UGREEN NASync DH2300 embedded controller
 * (HC32F005 MCU).
 *
 * The microcontroller gates the SATA bay power rail through register 0x41.
 * The polarity is inverted: writing 0 enables the rail, writing 1 disables it
 * (the controller latches "off" out of reset).
 */

#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>

#define UGREEN_DH2300_MCU_REG_SATA_POWER	0x41

static const struct regulator_ops ugreen_dh2300_sata_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
};

static const struct regulator_desc ugreen_dh2300_sata_desc = {
	.name = "sata-power",
	.enable_is_inverted = true,
	.enable_mask = 0x01,
	.enable_reg = UGREEN_DH2300_MCU_REG_SATA_POWER,
	.supply_name = "vin",
	.ops = &ugreen_dh2300_sata_ops,
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
};

static int ugreen_dh2300_mcu_regulator_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regulator_config config = { };
	struct regulator_dev *rdev;
	struct device_node *np;

	np = of_get_child_by_name(dev->parent->of_node, "regulator");
	if (!np)
		return dev_err_probe(dev, -ENODEV,
				     "missing regulator child node\n");

	config.dev = dev;
	config.of_node = np;
	config.regmap = dev_get_regmap(dev->parent, NULL);
	if (!config.regmap) {
		of_node_put(np);
		return dev_err_probe(dev, -ENODEV,
				     "no regmap available from parent\n");
	}

	config.init_data = of_get_regulator_init_data(dev, np,
						      &ugreen_dh2300_sata_desc);

	rdev = devm_regulator_register(dev, &ugreen_dh2300_sata_desc, &config);
	of_node_put(np);
	if (IS_ERR(rdev))
		return dev_err_probe(dev, PTR_ERR(rdev),
				     "failed to register regulator\n");

	return 0;
}

static struct platform_driver ugreen_dh2300_mcu_regulator_driver = {
	.driver = {
		.name = "ugreen-dh2300-mcu-regulator",
	},
	.probe = ugreen_dh2300_mcu_regulator_probe,
};
module_platform_driver(ugreen_dh2300_mcu_regulator_driver);

MODULE_DESCRIPTION("UGREEN NASync DH2300 MCU SATA power regulator");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:ugreen-dh2300-mcu-regulator");
