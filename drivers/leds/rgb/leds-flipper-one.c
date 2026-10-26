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

/*
 * On top of the per-LED colours, the MCU scales each LED group by a group
 * brightness (0 is off, 255 is full). The MCU keeps it in flash, and it can
 * also be changed from the MCU menu, so it is always read back from the MCU.
 * It is exposed on the controller device rather than on an individual LED, as
 * one setting covers several LEDs, some of which Linux does not drive at all.
 */
static ssize_t foled_group_brightness_show(struct device *dev, char *buf,
					   unsigned int reg)
{
	struct foled_device *foled = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = regmap_read(foled->regmap, reg, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t foled_group_brightness_store(struct device *dev,
					    const char *buf, size_t count,
					    unsigned int reg)
{
	struct foled_device *foled = dev_get_drvdata(dev);
	u8 val;
	int ret;

	ret = kstrtou8(buf, 10, &val);
	if (ret)
		return ret;

	ret = regmap_write(foled->regmap, reg, val);

	return ret ?: count;
}

#define FOLED_GROUP_BRIGHTNESS_ATTR(_group, _reg)			\
static ssize_t _group##_brightness_show(struct device *dev,		\
					struct device_attribute *attr,	\
					char *buf)			\
{									\
	return foled_group_brightness_show(dev, buf, _reg);		\
}									\
									\
static ssize_t _group##_brightness_store(struct device *dev,		\
					 struct device_attribute *attr,	\
					 const char *buf, size_t count)	\
{									\
	return foled_group_brightness_store(dev, buf, count, _reg);	\
}									\
static DEVICE_ATTR_RW(_group##_brightness)

FOLED_GROUP_BRIGHTNESS_ATTR(link, FOMCU_REG_LEDS_BR_LINK);
FOLED_GROUP_BRIGHTNESS_ATTR(power, FOMCU_REG_LEDS_BR_POWER);
FOLED_GROUP_BRIGHTNESS_ATTR(wattmeter, FOMCU_REG_LEDS_BR_WATT);

static struct attribute *foled_attrs[] = {
	&dev_attr_link_brightness.attr,
	&dev_attr_power_brightness.attr,
	&dev_attr_wattmeter_brightness.attr,
	NULL
};
ATTRIBUTE_GROUPS(foled);

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
		.name		= "flipper-one-leds",
		.dev_groups	= foled_groups,
	},
	.id_table	= foled_id_table,
};
module_platform_driver(foled_driver);

MODULE_DESCRIPTION("Flipper One LED driver");
MODULE_AUTHOR("Alexey Charkov <alchark@flipper.net>");
MODULE_LICENSE("GPL");
