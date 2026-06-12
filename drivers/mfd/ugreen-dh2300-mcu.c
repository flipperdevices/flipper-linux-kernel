// SPDX-License-Identifier: GPL-2.0-only
/*
 * Core driver for the UGREEN NASync DH2300 embedded controller (HC32F005 MCU).
 *
 * The microcontroller sits on I2C and exposes an 8-bit register map. It is a
 * multi-function device: SATA drive-bay power gate, hardware watchdog and
 * possibly other functions
 */

#include <linux/i2c.h>
#include <linux/mfd/core.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regmap.h>

#define UGREEN_DH2300_MCU_REG_MAX	0x94

static const struct regmap_config ugreen_dh2300_mcu_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = UGREEN_DH2300_MCU_REG_MAX,
};

static const struct mfd_cell ugreen_dh2300_mcu_cells[] = {
	{ .name = "ugreen-dh2300-mcu-regulator" },
};

static int ugreen_dh2300_mcu_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regmap *regmap;

	regmap = devm_regmap_init_i2c(client, &ugreen_dh2300_mcu_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "failed to initialise regmap\n");

	return devm_mfd_add_devices(dev, PLATFORM_DEVID_AUTO,
				    ugreen_dh2300_mcu_cells,
				    ARRAY_SIZE(ugreen_dh2300_mcu_cells),
				    NULL, 0, NULL);
}

static const struct of_device_id ugreen_dh2300_mcu_of_match[] = {
	{ .compatible = "ugreen,dh2300-mcu" },
	{ }
};
MODULE_DEVICE_TABLE(of, ugreen_dh2300_mcu_of_match);

static struct i2c_driver ugreen_dh2300_mcu_driver = {
	.driver = {
		.name = "ugreen-dh2300-mcu",
		.of_match_table = ugreen_dh2300_mcu_of_match,
	},
	.probe = ugreen_dh2300_mcu_probe,
};
module_i2c_driver(ugreen_dh2300_mcu_driver);

MODULE_DESCRIPTION("UGREEN NASync DH2300 embedded controller core driver");
MODULE_LICENSE("GPL");
