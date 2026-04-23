// SPDX-License-Identifier: GPL-2.0
/*
 * Flipper One LED driver
 * Copyright (C) 2026 Flipper FZCO
 */

#include <linux/led-class-multicolor.h>
#include <linux/leds.h>
#include <linux/mfd/flipper-one-mcu.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define FOLED_NUM_LEDS		4
#define FOLED_NUM_COLORS	3

/* RGB565 component shifts and max values */
#define FOLED_R_SHIFT		11
#define FOLED_G_SHIFT		5
#define FOLED_B_SHIFT		0
#define FOLED_R_MAX		31
#define FOLED_G_MAX		63
#define FOLED_B_MAX		31

struct foled_device;

struct foled_led {
	struct foled_device *foled;
	struct led_classdev_mc mc_cdev;
	struct mc_subled subleds[FOLED_NUM_COLORS];
	unsigned int reg;
};

struct foled_device {
	struct regmap *regmap;
	struct foled_led leds[FOLED_NUM_LEDS];
};

static const unsigned int foled_regs[FOLED_NUM_LEDS] = {
	FOMCU_REG_LEDS_COLOR_LINK1,
	FOMCU_REG_LEDS_COLOR_LINK2,
	FOMCU_REG_LEDS_COLOR_LINK3,
	FOMCU_REG_LEDS_COLOR_LINK4,
};

static const char * const foled_led_names[FOLED_NUM_LEDS] = {
	"flipper-one:rgb:link",
	"flipper-one:rgb:wifi",
	"flipper-one:rgb:eth1",
	"flipper-one:rgb:eth0",
};

static int foled_brightness_set(struct led_classdev *cdev,
				enum led_brightness brightness)
{
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(cdev);
	struct foled_led *led = container_of(mc_cdev, struct foled_led, mc_cdev);
	u16 r, g, b, rgb565;

	led_mc_calc_color_components(mc_cdev, brightness);

	r = (u16)mc_cdev->subled_info[0].brightness * FOLED_R_MAX / LED_FULL;
	g = (u16)mc_cdev->subled_info[1].brightness * FOLED_G_MAX / LED_FULL;
	b = (u16)mc_cdev->subled_info[2].brightness * FOLED_B_MAX / LED_FULL;

	rgb565 = (r << FOLED_R_SHIFT) | (g << FOLED_G_SHIFT) | (b << FOLED_B_SHIFT);

	return regmap_write(led->foled->regmap, led->reg, rgb565);
}

static int foled_probe(struct platform_device *pdev)
{
	struct foled_device *foled;
	struct foled_led *led;
	struct led_classdev *cdev;
	int i, ret;

	foled = devm_kzalloc(&pdev->dev, sizeof(*foled), GFP_KERNEL);
	if (!foled)
		return -ENOMEM;

	foled->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!foled->regmap)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "Failed to get parent regmap\n");

	for (i = 0; i < FOLED_NUM_LEDS; i++) {
		led = &foled->leds[i];
		led->foled = foled;
		led->reg = foled_regs[i];

		led->subleds[0].color_index = LED_COLOR_ID_RED;
		led->subleds[1].color_index = LED_COLOR_ID_GREEN;
		led->subleds[2].color_index = LED_COLOR_ID_BLUE;

		led->mc_cdev.subled_info = led->subleds;
		led->mc_cdev.num_colors = FOLED_NUM_COLORS;

		cdev = &led->mc_cdev.led_cdev;
		cdev->name = foled_led_names[i];
		cdev->max_brightness = LED_FULL;
		cdev->brightness_set_blocking = foled_brightness_set;
		cdev->flags = LED_CORE_SUSPENDRESUME;

		ret = devm_led_classdev_multicolor_register(&pdev->dev,
							    &led->mc_cdev);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
					     "Failed to register LED %d\n", i + 1);
	}

	platform_set_drvdata(pdev, foled);
	return 0;
}

static const struct platform_device_id foled_id_table[] = {
	{ "flipper-one-leds" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(platform, foled_id_table);

static struct platform_driver foled_driver = {
	.probe		= foled_probe,
	.driver		= {
		.name	= "flipper-one-leds",
	},
	.id_table	= foled_id_table,
};
module_platform_driver(foled_driver);

MODULE_DESCRIPTION("Flipper One LED driver");
MODULE_AUTHOR("Alexey Charkov <alchark@flipper.net>");
MODULE_LICENSE("GPL");
