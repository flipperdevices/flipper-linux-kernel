// SPDX-License-Identifier: GPL-2.0
/*
 * Flipper One MCU interconnect driver
 * Copyright (C) 2026 Flipper FZCO
 */

#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/mfd/core.h>
#include <linux/mfd/flipper-one-mcu.h>
#include <linux/module.h>
#include <linux/regmap.h>

static const struct regmap_range fomcu_writeable_reg_ranges[] = {
	regmap_reg_range(FOMCU_REG_INTMSK_INPUT,
			 FOMCU_REG_INPUT_BTNS - 1),
};

static const struct regmap_access_table fomcu_writeable_regs = {
	.yes_ranges = fomcu_writeable_reg_ranges,
	.n_yes_ranges = ARRAY_SIZE(fomcu_writeable_reg_ranges),
};

static const struct regmap_range fomcu_nonvolatile_reg_ranges[] = {
	regmap_reg_range(FOMCU_REG_VERSION, FOMCU_REG_VERSION + 1),
	regmap_reg_range(FOMCU_REG_INTMSK_INPUT, FOMCU_REG_INPUT_BTNS - 1),
};

static const struct regmap_access_table fomcu_volatile_regs = {
	.no_ranges = fomcu_nonvolatile_reg_ranges,
	.n_no_ranges = ARRAY_SIZE(fomcu_nonvolatile_reg_ranges),
};

static const struct regmap_range fomcu_precious_reg_ranges[] = {
	regmap_reg_range(FOMCU_REG_INTSTS_INPUT,
			 FOMCU_REG_INTMSK_INPUT - 1),
};

static const struct regmap_access_table fomcu_precious_regs = {
	.yes_ranges = fomcu_precious_reg_ranges,
	.n_yes_ranges = ARRAY_SIZE(fomcu_precious_reg_ranges),
};

static const struct regmap_config fomcu_regmap_config = {
	.name = "flipper-one-mcu",
	.reg_bits = 16,
	.reg_stride = 2,
	.val_bits = 16,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
	.max_register = FOMCU_REG_MAX,
	.wr_table = &fomcu_writeable_regs,
	.volatile_table = &fomcu_volatile_regs,
	.precious_table = &fomcu_precious_regs,
};

#define CAT(a, b) CAT_I(a, b)
#define CAT_I(a, b) a##b
#define FOMCU_IRQ_REG(subsys, bit) \
	REGMAP_IRQ_REG(CAT(FOMCU_INT_, CAT(subsys, CAT(_, bit))), \
		       CAT(FOMCU_INTOFF_, subsys), \
		       CAT(FOMCU_INTSTS_, CAT(subsys, CAT(_, bit))))

static const struct regmap_irq fomcu_irqs[] = {
	FOMCU_IRQ_REG(INPUT, BTN),
	FOMCU_IRQ_REG(INPUT, TOUCH),
};

static unsigned int irq_input_offsets[] = { FOMCU_INTOFF_INPUT };

static const struct regmap_irq_sub_irq_map fomcu_sub_irqs[] = {
	REGMAP_IRQ_MAIN_REG_OFFSET(irq_input_offsets),
};

static const struct regmap_irq_chip fomcu_irq_chip = {
	.name = "fomcu-irq",
	.irqs = fomcu_irqs,
	.num_irqs = ARRAY_SIZE(fomcu_irqs),
	.main_status = FOMCU_REG_INTSTS,
	.status_base = FOMCU_REG_INTSTS_INPUT,
	.mask_base = FOMCU_REG_INTMSK_INPUT,
	.sub_reg_offsets = &fomcu_sub_irqs[0],
	.num_main_regs = 1,
	.num_regs = ARRAY_SIZE(fomcu_sub_irqs),
};

static const struct resource fo_input_irqs[] = {
	DEFINE_RES_IRQ_NAMED(FOMCU_INT_INPUT_BTN, "flipper-one-input-btn"),
	DEFINE_RES_IRQ_NAMED(FOMCU_INT_INPUT_TOUCH, "flipper-one-input-touch"),
};

static const struct mfd_cell cells[] = {
	MFD_CELL_RES("flipper-one-input", fo_input_irqs),
	MFD_CELL_NAME("flipper-one-leds"),
	MFD_CELL_NAME("flipper-one-power"),
	MFD_CELL_NAME("flipper-one-regulators"),
	MFD_CELL_NAME("flipper-one-thermal"),
	MFD_CELL_NAME("flipper-one-typec"),
};

static int fomcu_probe(struct i2c_client *client)
{
	struct regmap_irq_chip_data *irq_data;
	struct fomcu_device *ddata;
	int ret;

	ddata = devm_kzalloc(&client->dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	ddata->client = client;

	ddata->regmap = devm_regmap_init_i2c(client, &fomcu_regmap_config);
	if (IS_ERR(ddata->regmap)) {
		return dev_err_probe(&client->dev, PTR_ERR(ddata->regmap),
				     "Failed to allocate register map\n");
	}

	i2c_set_clientdata(client, ddata);

	ret = devm_regmap_add_irq_chip(&client->dev, ddata->regmap,
				       client->irq, IRQF_ONESHOT, 0,
				       &fomcu_irq_chip, &irq_data);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Failed to add IRQ chip\n");

	ret = devm_mfd_add_devices(&client->dev, PLATFORM_DEVID_AUTO,
				   cells, ARRAY_SIZE(cells), NULL, 0,
				   regmap_irq_get_domain(irq_data));
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Failed to register child devices\n");

	return ret;
}

static const struct i2c_device_id fomcu_i2c_ids[] = {
	{ "flipper-one-mcu" },
	{}
};
MODULE_DEVICE_TABLE(i2c, fomcu_i2c_ids);

static const struct of_device_id fomcu_of_match[] = {
	{ .compatible = "flipper,one-mcu" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, fomcu_of_match);

static struct i2c_driver fomcu_driver = {
	.driver = {
		.name = "flipper-one-mcu",
		.of_match_table = fomcu_of_match,
	},
	.probe = fomcu_probe,
	.id_table = fomcu_i2c_ids,
};
module_i2c_driver(fomcu_driver);

MODULE_DESCRIPTION("Flipper One MCU driver");
MODULE_AUTHOR("Alexey Charkov <alchark@flipper.net>");
MODULE_LICENSE("GPL");
