// SPDX-License-Identifier: GPL-2.0-only
/*
 * Flipper One builtin buttons and touchpad driver
 * Copyright (C) 2026 Flipper FZCO
 */

#include <linux/array_size.h>
#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/regmap.h>
#include <linux/types.h>

#include <linux/mfd/flipper-one-mcu.h>

#define FO_BTN_VIEW		BIT(0)
#define FO_BTN_ESCAPE		BIT(1)
#define FO_BTN_POWER		BIT(2)
#define FO_BTN_EDIT		BIT(3)
#define FO_BTN_RUN		BIT(4)
#define FO_BTN_APPSELECT	BIT(5)
#define FO_BTN_BACK		BIT(6)
#define FO_BTN_DOWN		BIT(7)
#define FO_BTN_RIGHT		BIT(8)
#define FO_BTN_CENTER		BIT(9)
#define FO_BTN_LEFT		BIT(10)
#define FO_BTN_UP		BIT(11)
#define FO_BTN_PTT		BIT(12)

#define FO_HS_HPPRESENT		BIT(0)
#define FO_HS_MICPRESENT	BIT(1)
#define FO_HS_BTN_A		BIT(2)
#define FO_HS_BTN_B		BIT(3)
#define FO_HS_BTN_C		BIT(4)
#define FO_HS_BTN_D		BIT(5)

#define FO_SWBTN_POWER		BIT(0)

static irqreturn_t fo_input_btn_handler(int irq, void *data)
{
	struct input_dev *idev = data;
	struct regmap *regmap = input_get_drvdata(idev);
	struct device *parent = idev->dev.parent;
	unsigned int reg;
	int err;

	err = regmap_read(regmap, FOMCU_REG_INPUT_BTNS, &reg);
	if (err) {
		dev_err(parent, "Failed to read button states: %d\n", err);
		return IRQ_NONE;
	}

	input_report_key(idev, KEY_ENTER, FO_BTN_CENTER & reg);
	input_report_key(idev, KEY_UP, FO_BTN_UP & reg);
	input_report_key(idev, KEY_DOWN, FO_BTN_DOWN & reg);
	input_report_key(idev, KEY_LEFT, FO_BTN_LEFT & reg);
	input_report_key(idev, KEY_RIGHT, FO_BTN_RIGHT & reg);
	input_report_key(idev, KEY_TAB, FO_BTN_APPSELECT & reg);
	input_report_key(idev, KEY_BACKSPACE, FO_BTN_BACK & reg);
	input_report_key(idev, KEY_A, FO_BTN_PTT & reg);
	input_report_key(idev, KEY_Z, FO_BTN_ESCAPE & reg);
	input_report_key(idev, KEY_X, FO_BTN_VIEW & reg);
	input_report_key(idev, KEY_C, FO_BTN_POWER & reg);
	input_report_key(idev, KEY_V, FO_BTN_EDIT & reg);
	input_report_key(idev, KEY_B, FO_BTN_RUN & reg);
	input_sync(idev);

	return IRQ_HANDLED;
}

static void fo_input_setup_btn(struct input_dev *idev)
{
	input_set_capability(idev, EV_KEY, KEY_ENTER);		/* D-pad center */
	input_set_capability(idev, EV_KEY, KEY_UP);		/* D-pad up */
	input_set_capability(idev, EV_KEY, KEY_DOWN);		/* D-pad down */
	input_set_capability(idev, EV_KEY, KEY_LEFT);		/* D-pad left */
	input_set_capability(idev, EV_KEY, KEY_RIGHT);		/* D-pad right */
	input_set_capability(idev, EV_KEY, KEY_TAB);		/* App switcher */
	input_set_capability(idev, EV_KEY, KEY_BACKSPACE);	/* Back */
	input_set_capability(idev, EV_KEY, KEY_A);		/* PTT */
	input_set_capability(idev, EV_KEY, KEY_Z);		/* Escape */
	input_set_capability(idev, EV_KEY, KEY_X);		/* View */
	input_set_capability(idev, EV_KEY, KEY_C);		/* Power */
	input_set_capability(idev, EV_KEY, KEY_V);		/* Edit */
	input_set_capability(idev, EV_KEY, KEY_B);		/* Run */
}

static irqreturn_t fo_input_touch_handler(int irq, void *data)
{
	struct input_dev *idev = data;
	struct regmap *regmap = input_get_drvdata(idev);
	struct device *parent = idev->dev.parent;
	u16 buf[3];
	int err;

	err = regmap_bulk_read(regmap, FOMCU_REG_INPUT_TOUCH_X, &buf, ARRAY_SIZE(buf));
	if (err) {
		dev_err(parent, "Failed to read touch inputs: %d\n", err);
		return IRQ_NONE;
	}

	input_report_key(idev, BTN_TOUCH, !!buf[2]);
	input_report_key(idev, BTN_TOOL_FINGER, !!buf[2]);
	input_report_abs(idev, ABS_X, buf[0]);
	input_report_abs(idev, ABS_Y, buf[1]);
	input_report_abs(idev, ABS_PRESSURE, buf[2]);
	input_sync(idev);

	return IRQ_HANDLED;
}

static void fo_input_setup_touch(struct input_dev *idev)
{
	input_set_capability(idev, EV_KEY, BTN_TOUCH);
	input_set_capability(idev, EV_KEY, BTN_TOOL_FINGER);
	input_set_abs_params(idev, ABS_X, 0, 1024, 0, 0);
	input_set_abs_params(idev, ABS_Y, 0, 800, 0, 0);
	input_set_abs_params(idev, ABS_PRESSURE, 0, 12288, 0, 0);
	__set_bit(INPUT_PROP_POINTER, idev->propbit);
}

static irqreturn_t fo_input_headset_handler(int irq, void *data)
{
	struct input_dev *idev = data;
	struct regmap *regmap = input_get_drvdata(idev);
	struct device *parent = idev->dev.parent;
	unsigned int reg;
	int err;

	err = regmap_read(regmap, FOMCU_REG_INPUT_HEADSET, &reg);
	if (err) {
		dev_err(parent, "Failed to read headset states: %d\n", err);
		return IRQ_NONE;
	}

	input_report_switch(idev, SW_HEADPHONE_INSERT, reg & FO_HS_HPPRESENT);
	input_report_switch(idev, SW_MICROPHONE_INSERT, reg & FO_HS_MICPRESENT);
	input_report_key(idev, KEY_PLAYPAUSE, reg & FO_HS_BTN_A);
	input_report_key(idev, KEY_VOLUMEUP, reg & FO_HS_BTN_B);
	input_report_key(idev, KEY_VOLUMEDOWN, reg & FO_HS_BTN_C);
	input_report_key(idev, KEY_VOICECOMMAND, reg & FO_HS_BTN_D);
	input_sync(idev);

	return IRQ_HANDLED;
}

static void fo_input_setup_headset(struct input_dev *idev)
{
	input_set_capability(idev, EV_SW, SW_HEADPHONE_INSERT);
	input_set_capability(idev, EV_SW, SW_MICROPHONE_INSERT);
	input_set_capability(idev, EV_KEY, KEY_PLAYPAUSE);
	input_set_capability(idev, EV_KEY, KEY_VOLUMEUP);
	input_set_capability(idev, EV_KEY, KEY_VOLUMEDOWN);
	input_set_capability(idev, EV_KEY, KEY_VOICECOMMAND);
}

static irqreturn_t fo_input_swbtn_handler(int irq, void *data)
{
	struct input_dev *idev = data;
	struct regmap *regmap = input_get_drvdata(idev);
	struct device *parent = idev->dev.parent;
	unsigned int reg;
	int err;

	err = regmap_read(regmap, FOMCU_REG_INPUT_SWBTNS, &reg);
	if (err) {
		dev_err(parent, "Failed to read SW button states: %d\n", err);
		return IRQ_NONE;
	}

	input_report_key(idev, KEY_POWER, FO_SWBTN_POWER & reg);
	input_sync(idev);

	return IRQ_HANDLED;
}

static void fo_input_setup_swbtn(struct input_dev *idev)
{
	input_set_capability(idev, EV_KEY, KEY_POWER);
}

struct fo_input_config {
	const char *name;
	const char *phys;
	const char *irq_name;
	irqreturn_t (*handler)(int irq, void *data);
	void (*setup)(struct input_dev *idev);
};

static const struct fo_input_config fo_input_configs[] = {
	{
		.name = "Flipper One Buttons",
		.phys = "flipper-one-input/input0",
		.irq_name = "flipper-one-input-btn",
		.handler = fo_input_btn_handler,
		.setup = fo_input_setup_btn,
	},
	{
		.name = "Flipper One Touchpad",
		.phys = "flipper-one-input/input1",
		.irq_name = "flipper-one-input-touch",
		.handler = fo_input_touch_handler,
		.setup = fo_input_setup_touch,
	},
	{
		.name = "Flipper One Headset",
		.phys = "flipper-one-input/input2",
		.irq_name = "flipper-one-input-headset",
		.handler = fo_input_headset_handler,
		.setup = fo_input_setup_headset,
	},
	{
		.name = "Flipper One Software Buttons",
		.phys = "flipper-one-input/input3",
		.irq_name = "flipper-one-input-swbtn",
		.handler = fo_input_swbtn_handler,
		.setup = fo_input_setup_swbtn,
	},
};

static int fo_input_probe(struct platform_device *pdev)
{
	struct fomcu_device *fomcu = dev_get_drvdata(pdev->dev.parent);
	struct device *dev = &pdev->dev;
	int irq, err, i;

	err = devm_device_init_wakeup(dev);
	if (err)
		return dev_err_probe(dev, err, "Failed to init wakeup\n");

	for (i = 0; i < ARRAY_SIZE(fo_input_configs); i++) {
		const struct fo_input_config *cfg = &fo_input_configs[i];
		struct input_dev *idev;

		idev = devm_input_allocate_device(dev);
		if (!idev)
			return dev_err_probe(dev, -ENOMEM,
					     "Failed to allocate %s input device\n",
					     cfg->name);

		idev->name = cfg->name;
		idev->phys = cfg->phys;
		idev->id.bustype = BUS_I2C;
		input_set_drvdata(idev, fomcu->regmap);
		cfg->setup(idev);

		irq = platform_get_irq_byname(pdev, cfg->irq_name);
		if (irq < 0)
			return dev_err_probe(dev, irq, "Failed to get IRQ %s\n",
					     cfg->irq_name);

		err = devm_request_threaded_irq(dev, irq, NULL, cfg->handler,
						IRQF_ONESHOT | IRQF_NO_SUSPEND,
						cfg->irq_name, idev);
		if (err)
			return dev_err_probe(dev, err, "Failed to request IRQ %s\n",
					     cfg->irq_name);

		err = input_register_device(idev);
		if (err)
			return dev_err_probe(dev, err,
					     "Failed to register %s input device\n",
					     cfg->name);
	}

	return 0;
}

static const struct platform_device_id fo_input_id_table[] = {
	{ "flipper-one-input", },
	{ }
};
MODULE_DEVICE_TABLE(platform, fo_input_id_table);

static struct platform_driver fo_input_driver = {
	.driver = {
		.name = "flipper-one-input",
	},
	.probe = fo_input_probe,
	.id_table = fo_input_id_table,
};
module_platform_driver(fo_input_driver);

MODULE_DESCRIPTION("Flipper One buttons and touchpad driver");
MODULE_AUTHOR("Alexey Charkov <alchark@flipper.net>");
MODULE_LICENSE("GPL");
