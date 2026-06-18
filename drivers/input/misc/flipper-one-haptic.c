// SPDX-License-Identifier: GPL-2.0-only
/*
 * Flipper One haptic feedback driver
 * Copyright (C) 2026 Flipper FZCO
 */

#include <linux/bitfield.h>
#include <linux/input.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include <linux/mfd/flipper-one-mcu.h>

/* Highest effect index in the controller's built-in waveform library */
#define FO_HAPTIC_EFFECT_MAX	123

/* Number of effects userspace may keep uploaded at once */
#define FO_HAPTIC_MAX_EFFECTS	16

struct fo_haptic {
	struct fomcu_device *fomcu;
	struct device *dev;
	struct work_struct play_work;
	u16 effect_val[FO_HAPTIC_MAX_EFFECTS];
	u16 play_val;
};

static void fo_haptic_play_work(struct work_struct *work)
{
	struct fo_haptic *haptic = container_of(work, struct fo_haptic,
						play_work);
	int err;

	err = regmap_write(haptic->fomcu->regmap, FOMCU_REG_HAPTIC,
			   READ_ONCE(haptic->play_val));
	if (err)
		dev_err(haptic->dev, "Failed to write haptic register: %d\n",
			err);
}

static int fo_haptic_upload(struct input_dev *idev, struct ff_effect *effect,
			    struct ff_effect *old)
{
	struct fo_haptic *haptic = input_get_drvdata(idev);
	unsigned int duration;
	s16 id;

	if (effect->type != FF_PERIODIC ||
	    effect->u.periodic.waveform != FF_CUSTOM)
		return -EINVAL;

	/* custom_data holds a single s16: the library effect index */
	if (effect->u.periodic.custom_len != 1)
		return -EINVAL;

	if (copy_from_user(&id, effect->u.periodic.custom_data, sizeof(id)))
		return -EFAULT;

	if (id < 0 || id > FO_HAPTIC_EFFECT_MAX)
		return -EINVAL;

	/*
	 * Durations 0 and 1 are reserved to mean "play the full
	 * library waveform", other values are in milliseconds
	 */
	duration = min_t(unsigned int, effect->replay.length,
			 FIELD_MAX(FOMCU_HAPTIC_DURATION));

	haptic->effect_val[effect->id] = FOMCU_HAPTIC_PLAY |
		FIELD_PREP(FOMCU_HAPTIC_EFFECT, id) |
		FIELD_PREP(FOMCU_HAPTIC_DURATION, duration);

	return 0;
}

static int fo_haptic_playback(struct input_dev *idev, int effect_id, int value)
{
	struct fo_haptic *haptic = input_get_drvdata(idev);

	WRITE_ONCE(haptic->play_val,
		   value ? haptic->effect_val[effect_id] : 0);
	schedule_work(&haptic->play_work);

	return 0;
}

static void fo_haptic_close(struct input_dev *idev)
{
	struct fo_haptic *haptic = input_get_drvdata(idev);

	cancel_work_sync(&haptic->play_work);
	regmap_write(haptic->fomcu->regmap, FOMCU_REG_HAPTIC, 0);
}

static int fo_haptic_probe(struct platform_device *pdev)
{
	struct fomcu_device *fomcu = dev_get_drvdata(pdev->dev.parent);
	struct device *dev = &pdev->dev;
	struct fo_haptic *haptic;
	struct input_dev *idev;
	int err;

	haptic = devm_kzalloc(dev, sizeof(*haptic), GFP_KERNEL);
	if (!haptic)
		return -ENOMEM;

	haptic->fomcu = fomcu;
	haptic->dev = dev;
	INIT_WORK(&haptic->play_work, fo_haptic_play_work);

	idev = devm_input_allocate_device(dev);
	if (!idev)
		return -ENOMEM;

	idev->name = "Flipper One Haptic";
	idev->phys = "flipper-one-haptic/input0";
	idev->id.bustype = BUS_I2C;
	idev->close = fo_haptic_close;
	input_set_drvdata(idev, haptic);

	input_set_capability(idev, EV_FF, FF_PERIODIC);
	input_set_capability(idev, EV_FF, FF_CUSTOM);

	err = input_ff_create(idev, FO_HAPTIC_MAX_EFFECTS);
	if (err)
		return dev_err_probe(dev, err, "Failed to create FF device\n");

	idev->ff->upload = fo_haptic_upload;
	idev->ff->playback = fo_haptic_playback;

	err = input_register_device(idev);
	if (err)
		return dev_err_probe(dev, err,
				     "Failed to register input device\n");

	return 0;
}

static const struct platform_device_id fo_haptic_id_table[] = {
	{ "flipper-one-haptic", },
	{ }
};
MODULE_DEVICE_TABLE(platform, fo_haptic_id_table);

static struct platform_driver fo_haptic_driver = {
	.driver = {
		.name = "flipper-one-haptic",
	},
	.probe = fo_haptic_probe,
	.id_table = fo_haptic_id_table,
};
module_platform_driver(fo_haptic_driver);

MODULE_DESCRIPTION("Flipper One haptic feedback driver");
MODULE_AUTHOR("Alexey Charkov <alchark@flipper.net>");
MODULE_LICENSE("GPL");
