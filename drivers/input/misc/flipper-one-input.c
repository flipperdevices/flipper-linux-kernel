// SPDX-License-Identifier: GPL-2.0-only
/*
 * Flipper One builtin buttons and touchpad driver
 * Copyright (C) 2026 Flipper FZCO
 */

#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/limits.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/unaligned.h>

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

struct fo_input {
	struct input_dev *idev_btn;
	struct input_dev *idev_touch;
	struct input_dev *idev_headset;
	struct fomcu_device *fomcu;
};

struct fo_irq {
	const char *name;
	irqreturn_t (*handler)(int, void *);
};

static irqreturn_t fo_input_btn_handler(int irq, void *data)
{
	struct fo_input *input = data;
	struct regmap *regmap = input->fomcu->regmap;
	struct input_dev *idev = input->idev_btn;
	struct device *parent = idev->dev.parent;
	unsigned int reg;
	int err;

	err = regmap_read(regmap, FOMCU_REG_INPUT_BTNS, &reg);
	if (err) {
		dev_err(parent, "Failed to read button states: %d\n", err);
		return IRQ_NONE;
	}

	input_report_key(idev, KEY_K, FO_BTN_CENTER & reg);
	input_report_key(idev, KEY_I, FO_BTN_UP & reg);
	input_report_key(idev, KEY_M, FO_BTN_DOWN & reg);
	input_report_key(idev, KEY_J, FO_BTN_LEFT & reg);
	input_report_key(idev, KEY_L, FO_BTN_RIGHT & reg);
	input_report_key(idev, KEY_H, FO_BTN_APPSELECT & reg);
	input_report_key(idev, KEY_N, FO_BTN_BACK & reg);
	input_report_key(idev, KEY_A, FO_BTN_PTT & reg);
	input_report_key(idev, KEY_Z, FO_BTN_ESCAPE & reg);
	input_report_key(idev, KEY_X, FO_BTN_VIEW & reg);
	input_report_key(idev, KEY_C, FO_BTN_POWER & reg);
	input_report_key(idev, KEY_V, FO_BTN_EDIT & reg);
	input_report_key(idev, KEY_B, FO_BTN_RUN & reg);
	input_sync(idev);

	return IRQ_HANDLED;
}

static irqreturn_t fo_input_touch_handler(int irq, void *data)
{
	struct fo_input *input = data;
	struct regmap *regmap = input->fomcu->regmap;
	struct input_dev *idev = input->idev_touch;
	struct device *parent = idev->dev.parent;
	uint16_t buf[3];
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

static irqreturn_t fo_input_headset_handler(int irq, void *data)
{
	struct fo_input *input = data;
	struct regmap *regmap = input->fomcu->regmap;
	struct input_dev *idev = input->idev_headset;
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

static const struct fo_irq fo_irqs[] = {
	{ .name = "flipper-one-input-btn", .handler = fo_input_btn_handler },
	{ .name = "flipper-one-input-touch", .handler = fo_input_touch_handler },
	{ .name = "flipper-one-input-headset", .handler = fo_input_headset_handler },
};

static int fo_input_probe(struct platform_device *pdev)
{
	struct fomcu_device *fomcu = dev_get_drvdata(pdev->dev.parent);
	struct device *dev = &pdev->dev;
	struct fo_input *input;
	struct input_dev *idev_btn, *idev_touch, *idev_headset;
	int irq, err, i;

	input = devm_kzalloc(dev, sizeof(*input), GFP_KERNEL);
	if (!input)
		return -ENOMEM;

	input->fomcu = fomcu;

	idev_btn = devm_input_allocate_device(dev);
	if (!idev_btn) {
		dev_err(dev, "Failed to allocate buttons input device\n");
		return -ENOMEM;
	}
	input->idev_btn = idev_btn;

	idev_btn->name = "Flipper One Buttons";
	idev_btn->phys = "flipper-one-input/input0";
	idev_btn->id.bustype = BUS_I2C;

	idev_touch = devm_input_allocate_device(dev);
	if (!idev_touch) {
		dev_err(dev, "Failed to allocate touch input device\n");
		return -ENOMEM;
	}
	input->idev_touch = idev_touch;

	idev_touch->name = "Flipper One Touchpad";
	idev_touch->phys = "flipper-one-input/input1";
	idev_touch->id.bustype = BUS_I2C;

	idev_headset = devm_input_allocate_device(dev);
	if (!idev_headset) {
		dev_err(dev, "Failed to allocate headset input device\n");
		return -ENOMEM;
	}
	input->idev_headset = idev_headset;

	idev_headset->name = "Flipper One Headset";
	idev_headset->phys = "flipper-one-input/input2";
	idev_headset->id.bustype = BUS_I2C;

	/* Buttons */
	input_set_capability(idev_btn, EV_KEY, KEY_K);	/* D-pad center */
	input_set_capability(idev_btn, EV_KEY, KEY_I);	/* D-pad up */
	input_set_capability(idev_btn, EV_KEY, KEY_M);	/* D-pad down */
	input_set_capability(idev_btn, EV_KEY, KEY_J);	/* D-pad left */
	input_set_capability(idev_btn, EV_KEY, KEY_L);	/* D-pad right */
	input_set_capability(idev_btn, EV_KEY, KEY_H);	/* App switcher */
	input_set_capability(idev_btn, EV_KEY, KEY_N);	/* Back */
	input_set_capability(idev_btn, EV_KEY, KEY_A);	/* PTT */
	input_set_capability(idev_btn, EV_KEY, KEY_Z);	/* Escape */
	input_set_capability(idev_btn, EV_KEY, KEY_X);	/* View */
	input_set_capability(idev_btn, EV_KEY, KEY_C);	/* Power */
	input_set_capability(idev_btn, EV_KEY, KEY_V);	/* Edit */
	input_set_capability(idev_btn, EV_KEY, KEY_B);	/* Run */

	/* Touchpad */
	input_set_capability(idev_touch, EV_KEY, BTN_TOUCH);
	input_set_capability(idev_touch, EV_KEY, BTN_TOOL_FINGER);
	input_set_abs_params(idev_touch, ABS_X, 0, 1024, 0, 0);
	input_set_abs_params(idev_touch, ABS_Y, 0, 800, 0, 0);
	input_set_abs_params(idev_touch, ABS_PRESSURE, 0, 12288, 0, 0);
	__set_bit(INPUT_PROP_POINTER, idev_touch->propbit);

	/* Headset */
	input_set_capability(idev_headset, EV_SW, SW_HEADPHONE_INSERT);
	input_set_capability(idev_headset, EV_SW, SW_MICROPHONE_INSERT);
	input_set_capability(idev_headset, EV_KEY, KEY_PLAYPAUSE);
	input_set_capability(idev_headset, EV_KEY, KEY_VOLUMEUP);
	input_set_capability(idev_headset, EV_KEY, KEY_VOLUMEDOWN);
	input_set_capability(idev_headset, EV_KEY, KEY_VOICECOMMAND);

	device_set_wakeup_capable(dev, true);
	device_wakeup_enable(dev);

	for (i = 0; i < ARRAY_SIZE(fo_irqs); i++) {
		irq = platform_get_irq_byname(pdev, fo_irqs[i].name);
		if (irq < 0)
			return dev_err_probe(dev, irq, "Failed to get IRQ %s\n",
					     fo_irqs[i].name);

		err = devm_request_threaded_irq(dev, irq, NULL, fo_irqs[i].handler,
						IRQF_ONESHOT | IRQF_NO_SUSPEND,
						fo_irqs[i].name, input);
		if (err)
			return dev_err_probe(dev, err, "Failed to request IRQ %s\n",
					     fo_irqs[i].name);
	}

	err = input_register_device(idev_btn);
	if (err)
		return dev_err_probe(dev, err, "Failed to register buttons input device\n");

	err = input_register_device(idev_touch);
	if (err)
		return dev_err_probe(dev, err, "Failed to register touch input device\n");

	err = input_register_device(idev_headset);
	if (err)
		return dev_err_probe(dev, err, "Failed to register headset input device\n");

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
