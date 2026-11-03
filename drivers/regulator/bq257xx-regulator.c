// SPDX-License-Identifier: GPL-2.0
/*
 * BQ257XX Battery Charger Driver
 * Copyright (C) 2025 Chris Morgan <macromorgan@hotmail.com>
 */

#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/mfd/bq257xx.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/workqueue.h>

/*
 * How long the BQ25792 VBUS pull-down stays on after a step-down or turn-off:
 * tSafe0V, by when a USB PD source has VBUS at vSafe0V after turning it off,
 * and which also covers the shorter time it has to settle after a step-down.
 */
#define BQ25792_VBUS_DISCHARGE_MS	650

/* Highest OTG voltage the system may sleep with */
#define BQ257XX_OTG_SLEEP_MAX_UV	5000000

struct bq257xx_reg_data {
	struct gpio_desc *otg_en_gpio;
	struct regulator_desc desc;
	/* OTG is on above BQ257XX_OTG_SLEEP_MAX_UV */
	bool vbus_high;
	struct device *dev;
	struct regmap *regmap;
	/* Serialises switching the VBUS pull-down on and off */
	struct mutex discharge_lock;
	struct delayed_work discharge_work;
};

static int bq25703_vbus_get_cur_limit(struct regulator_dev *rdev)
{
	struct regmap *regmap = rdev_get_regmap(rdev);
	int ret;
	unsigned int reg;

	ret = regmap_read(regmap, BQ25703_OTG_CURRENT, &reg);
	if (ret)
		return ret;
	return FIELD_GET(BQ25703_OTG_CUR_MASK, reg) * BQ25703_OTG_CUR_STEP_UA;
}

static int bq25792_vbus_get_cur_limit(struct regulator_dev *rdev)
{
	struct regmap *regmap = rdev_get_regmap(rdev);
	int ret;
	unsigned int reg;

	ret = regmap_read(regmap, BQ25792_REG0D_IOTG_REGULATION, &reg);
	if (ret)
		return ret;
	return FIELD_GET(BQ25792_REG0D_IOTG_MASK, reg) * BQ25792_OTG_CUR_STEP_UA;
}

static int bq25792_vbus_get_voltage_sel(struct regulator_dev *rdev)
{
	struct regmap *regmap = rdev_get_regmap(rdev);
	__be16 reg;
	int ret;

	ret = regmap_raw_read(regmap, BQ25792_REG0B_VOTG_REGULATION,
			      &reg, sizeof(reg));
	if (ret)
		return ret;

	return FIELD_GET(BQ25792_REG0B_VOTG_MASK, be16_to_cpu(reg));
}

/*
 * Check if the minimum current and maximum current requested are
 * sane values, then set the register accordingly.
 */
static int bq25703_vbus_set_cur_limit(struct regulator_dev *rdev,
				      int min_uA, int max_uA)
{
	struct regmap *regmap = rdev_get_regmap(rdev);
	unsigned int reg;

	if ((min_uA > BQ25703_OTG_CUR_MAX_UA) || (max_uA < 0))
		return -EINVAL;

	reg = min(max_uA, BQ25703_OTG_CUR_MAX_UA) / BQ25703_OTG_CUR_STEP_UA;

	/* Catch rounding errors since our step is 50000uA. */
	if ((reg * BQ25703_OTG_CUR_STEP_UA) < min_uA)
		return -EINVAL;

	return regmap_write(regmap, BQ25703_OTG_CURRENT,
			    FIELD_PREP(BQ25703_OTG_CUR_MASK, reg));
}

static int bq25792_vbus_set_cur_limit(struct regulator_dev *rdev,
				      int min_uA, int max_uA)
{
	struct regmap *regmap = rdev_get_regmap(rdev);
	unsigned int reg;

	if ((min_uA > BQ25792_OTG_CUR_MAX_UA) ||
	    (max_uA < BQ25792_OTG_CUR_MIN_UA))
		return -EINVAL;

	reg = min(max_uA, BQ25792_OTG_CUR_MAX_UA) / BQ25792_OTG_CUR_STEP_UA;

	/* Catch rounding errors since our step is 40000uA. */
	if ((reg * BQ25792_OTG_CUR_STEP_UA) < min_uA)
		return -EINVAL;

	return regmap_update_bits(regmap, BQ25792_REG0D_IOTG_REGULATION,
				  BQ25792_REG0D_IOTG_MASK,
				  FIELD_PREP(BQ25792_REG0D_IOTG_MASK, reg));
}

static void bq257xx_vbus_note_level(struct regulator_dev *rdev, bool enabled,
				    int sel)
{
	struct bq257xx_reg_data *pdata = rdev_get_drvdata(rdev);

	pdata->vbus_high = enabled && regulator_list_voltage_linear(rdev, sel) >
				      BQ257XX_OTG_SLEEP_MAX_UV;
}

/*
 * A sleeping system handles a detach only once it has resumed, and on the
 * BQ25792, nothing kicks the watchdog that turns OTG off when the system stops
 * managing the device. Refuse to sleep while OTG puts out more than 5V.
 */
static int bq257xx_regulator_prepare(struct device *dev)
{
	struct bq257xx_reg_data *pdata = dev_get_drvdata(dev);

	if (pdata->vbus_high) {
		dev_info(dev, "Not suspending while VBUS is above 5V\n");
		return -EBUSY;
	}

	return 0;
}

static const struct dev_pm_ops bq257xx_regulator_pm_ops = {
	.prepare = pm_sleep_ptr(bq257xx_regulator_prepare),
};

static bool bq25792_vbus_can_discharge(struct regulator_dev *rdev)
{
	return rdev->constraints->active_discharge ==
	       REGULATOR_ACTIVE_DISCHARGE_ENABLE;
}

/*
 * The converter stops switching while VBUS is above the setpoint, after a
 * step-down or when OTG turns off, and leaves VBUS to the load. Pull it down
 * through the 6kOhm resistor until it surely got there, then release, as
 * the resistor draws up to 3.3mA.
 */
static void bq25792_vbus_discharge(struct regulator_dev *rdev)
{
	struct bq257xx_reg_data *pdata = rdev_get_drvdata(rdev);

	guard(mutex)(&pdata->discharge_lock);
	if (!regmap_set_bits(pdata->regmap, BQ25792_REG16_TEMPERATURE_CONTROL,
			     BQ25792_REG16_VBUS_PD_EN))
		mod_delayed_work(system_dfl_wq, &pdata->discharge_work,
				 msecs_to_jiffies(BQ25792_VBUS_DISCHARGE_MS));
}

/* Called with discharge_lock held */
static int bq25792_vbus_discharge_off(struct bq257xx_reg_data *pdata)
{
	return regmap_clear_bits(pdata->regmap,
				 BQ25792_REG16_TEMPERATURE_CONTROL,
				 BQ25792_REG16_VBUS_PD_EN);
}

static void bq25792_vbus_discharge_work(struct work_struct *work)
{
	struct bq257xx_reg_data *pdata = container_of(work,
						      struct bq257xx_reg_data,
						      discharge_work.work);
	int ret;

	guard(mutex)(&pdata->discharge_lock);
	/* Leave it to a discharge started while this one waited */
	if (delayed_work_pending(&pdata->discharge_work))
		return;

	ret = bq25792_vbus_discharge_off(pdata);
	if (ret) {
		dev_warn_ratelimited(pdata->dev,
				     "cannot release VBUS pull-down: %d\n", ret);
		mod_delayed_work(system_dfl_wq, &pdata->discharge_work,
				 msecs_to_jiffies(BQ25792_VBUS_DISCHARGE_MS));
	}
}

/* Don't leave the pull-down on once the regulator is gone */
static void bq25792_vbus_discharge_stop(void *data)
{
	struct bq257xx_reg_data *pdata = data;

	cancel_delayed_work_sync(&pdata->discharge_work);
	guard(mutex)(&pdata->discharge_lock);
	bq25792_vbus_discharge_off(pdata);
}

static int bq25792_vbus_set_voltage_sel(struct regulator_dev *rdev,
					unsigned int sel)
{
	struct regmap *regmap = rdev_get_regmap(rdev);
	bool step_down;
	__be16 reg;
	int ret;

	step_down = bq25792_vbus_can_discharge(rdev) &&
		    regulator_is_enabled_regmap(rdev) > 0 &&
		    (int)sel < bq25792_vbus_get_voltage_sel(rdev);

	reg = cpu_to_be16(FIELD_PREP(BQ25792_REG0B_VOTG_MASK, sel));
	ret = regmap_raw_write(regmap, BQ25792_REG0B_VOTG_REGULATION,
			       &reg, sizeof(reg));
	if (!ret)
		bq257xx_vbus_note_level(rdev,
					regulator_is_enabled_regmap(rdev) > 0,
					sel);
	if (!ret && step_down)
		bq25792_vbus_discharge(rdev);

	return ret;
}

static int bq25792_vbus_enable(struct regulator_dev *rdev)
{
	struct bq257xx_reg_data *pdata = rdev_get_drvdata(rdev);
	int ret;

	/* Don't load the fresh output with what is left of a discharge */
	if (bq25792_vbus_can_discharge(rdev)) {
		scoped_guard(mutex, &pdata->discharge_lock) {
			cancel_delayed_work(&pdata->discharge_work);
			ret = bq25792_vbus_discharge_off(pdata);
		}
		if (ret)
			return ret;
	}

	ret = regulator_enable_regmap(rdev);
	if (!ret)
		bq257xx_vbus_note_level(rdev, true,
					bq25792_vbus_get_voltage_sel(rdev));

	return ret;
}

static int bq25792_vbus_disable(struct regulator_dev *rdev)
{
	int ret;

	ret = regulator_disable_regmap(rdev);
	if (!ret)
		bq257xx_vbus_note_level(rdev, false, 0);
	if (!ret && bq25792_vbus_can_discharge(rdev))
		bq25792_vbus_discharge(rdev);

	return ret;
}

static int bq25703_vbus_enable(struct regulator_dev *rdev)
{
	struct bq257xx_reg_data *pdata = rdev_get_drvdata(rdev);
	int ret;

	if (pdata->otg_en_gpio)
		gpiod_set_value_cansleep(pdata->otg_en_gpio, 1);
	ret = regulator_enable_regmap(rdev);
	if (!ret)
		bq257xx_vbus_note_level(rdev, true,
					regulator_get_voltage_sel_regmap(rdev));

	return ret;
}

static int bq25703_vbus_disable(struct regulator_dev *rdev)
{
	struct bq257xx_reg_data *pdata = rdev_get_drvdata(rdev);
	int ret;

	if (pdata->otg_en_gpio)
		gpiod_set_value_cansleep(pdata->otg_en_gpio, 0);
	ret = regulator_disable_regmap(rdev);
	if (!ret)
		bq257xx_vbus_note_level(rdev, false, 0);

	return ret;
}

static int bq25703_vbus_set_voltage_sel(struct regulator_dev *rdev,
					unsigned int sel)
{
	int ret;

	ret = regulator_set_voltage_sel_regmap(rdev, sel);
	if (!ret)
		bq257xx_vbus_note_level(rdev,
					regulator_is_enabled_regmap(rdev) > 0,
					sel);

	return ret;
}

static const struct regulator_ops bq25703_vbus_ops = {
	.enable = bq25703_vbus_enable,
	.disable = bq25703_vbus_disable,
	.is_enabled = regulator_is_enabled_regmap,
	.list_voltage = regulator_list_voltage_linear,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = bq25703_vbus_set_voltage_sel,
	.get_current_limit = bq25703_vbus_get_cur_limit,
	.set_current_limit = bq25703_vbus_set_cur_limit,
};

static const struct regulator_desc bq25703_vbus_desc = {
	.name = "vbus",
	.of_match = of_match_ptr("vbus"),
	.regulators_node = of_match_ptr("regulators"),
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.ops = &bq25703_vbus_ops,
	.min_uV = BQ25703_OTG_VOLT_MIN_UV,
	.uV_step = BQ25703_OTG_VOLT_STEP_UV,
	.n_voltages = BQ25703_OTG_VOLT_NUM_VOLT,
	.enable_mask = BQ25703_EN_OTG_MASK,
	.enable_reg = BQ25703_CHARGE_OPTION_3,
	.enable_val = BQ25703_EN_OTG_MASK,
	.disable_val = 0,
	.vsel_reg = BQ25703_OTG_VOLT,
	.vsel_mask = BQ25703_OTG_VOLT_MASK,
};

static const struct regulator_ops bq25792_vbus_ops = {
	/* No GPIO for enabling the OTG regulator */
	.enable = bq25792_vbus_enable,
	.disable = bq25792_vbus_disable,
	.is_enabled = regulator_is_enabled_regmap,
	.list_voltage = regulator_list_voltage_linear,
	.get_voltage_sel = bq25792_vbus_get_voltage_sel,
	.set_voltage_sel = bq25792_vbus_set_voltage_sel,
	.get_current_limit = bq25792_vbus_get_cur_limit,
	.set_current_limit = bq25792_vbus_set_cur_limit,
};

static const struct regulator_desc bq25792_vbus_desc = {
	.name = "vbus",
	.of_match = of_match_ptr("vbus"),
	.regulators_node = of_match_ptr("regulators"),
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.ops = &bq25792_vbus_ops,
	.min_uV = BQ25792_OTG_VOLT_MIN_UV,
	.uV_step = BQ25792_OTG_VOLT_STEP_UV,
	.n_voltages = BQ25792_OTG_VOLT_NUM_VOLT,
	.enable_mask = BQ25792_REG12_EN_OTG,
	.enable_reg = BQ25792_REG12_CHARGER_CONTROL_3,
	.enable_val = BQ25792_REG12_EN_OTG,
	.disable_val = 0,
};

/* Get optional GPIO for OTG regulator enable. */
static void bq257xx_reg_dt_parse_gpio(struct platform_device *pdev)
{
	struct device_node *child, *subchild;
	struct bq257xx_reg_data *pdata = platform_get_drvdata(pdev);

	child = of_get_child_by_name(pdev->dev.of_node,
				     pdata->desc.regulators_node);
	if (!child)
		return;

	subchild = of_get_child_by_name(child, pdata->desc.of_match);
	of_node_put(child);
	if (!subchild)
		return;

	pdata->otg_en_gpio = devm_fwnode_gpiod_get_index(&pdev->dev,
							 of_fwnode_handle(subchild),
							 "enable", 0,
							 GPIOD_OUT_LOW,
							 pdata->desc.of_match);

	of_node_put(subchild);

	if (IS_ERR(pdata->otg_en_gpio)) {
		if (PTR_ERR(pdata->otg_en_gpio) == -ENOENT) {
			/* No GPIO, will only use register writes for OTG */
			pdata->otg_en_gpio = NULL;
			return;
		}

		dev_err(&pdev->dev, "Error getting enable gpio: %ld\n",
			PTR_ERR(pdata->otg_en_gpio));
		return;
	}
}

static int bq257xx_regulator_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct bq257xx_device *bq = dev_get_drvdata(pdev->dev.parent);
	struct bq257xx_reg_data *pdata;
	struct regulator_config cfg = {};
	struct regulator_dev *rdev;
	int ret;

	device_set_of_node_from_dev(&pdev->dev, pdev->dev.parent);

	pdata = devm_kzalloc(&pdev->dev, sizeof(struct bq257xx_reg_data), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	switch (bq->type) {
	case BQ25703A:
		pdata->desc = bq25703_vbus_desc;
		break;
	case BQ25792:
		pdata->desc = bq25792_vbus_desc;
		break;
	default:
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "Unsupported device type\n");
	}

	platform_set_drvdata(pdev, pdata);
	if (bq->type == BQ25703A)
		bq257xx_reg_dt_parse_gpio(pdev);

	cfg.dev = &pdev->dev;
	cfg.driver_data = pdata;
	cfg.regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!cfg.regmap)
		return -ENODEV;
	pdata->dev = dev;
	pdata->regmap = cfg.regmap;

	/* The regulator may be turned off as soon as it is registered */
	if (bq->type == BQ25792) {
		ret = devm_mutex_init(dev, &pdata->discharge_lock);
		if (ret)
			return ret;
		INIT_DELAYED_WORK(&pdata->discharge_work,
				  bq25792_vbus_discharge_work);

		/* Don't keep a pull-down left on from before */
		scoped_guard(mutex, &pdata->discharge_lock)
			ret = bq25792_vbus_discharge_off(pdata);
		if (ret)
			return ret;

		ret = devm_add_action_or_reset(dev, bq25792_vbus_discharge_stop,
					       pdata);
		if (ret)
			return ret;
	}

	rdev = devm_regulator_register(dev, &pdata->desc, &cfg);
	if (IS_ERR(rdev)) {
		return dev_err_probe(&pdev->dev, PTR_ERR(rdev),
				     "error registering bq257xx regulator");
	}

	return 0;
}

/*
 * The charger keeps running off the battery when the system goes down, so it
 * would go on powering a port partner while the system is off, or into the
 * next kernel after a kexec, which assumes VBUS is off. Turn the output off.
 */
static void bq257xx_regulator_shutdown(struct platform_device *pdev)
{
	struct bq257xx_reg_data *pdata = platform_get_drvdata(pdev);
	struct regmap *regmap = dev_get_regmap(pdev->dev.parent, NULL);

	if (pdata->otg_en_gpio)
		gpiod_set_value_cansleep(pdata->otg_en_gpio, 0);
	if (regmap)
		regmap_update_bits(regmap, pdata->desc.enable_reg,
				   pdata->desc.enable_mask,
				   pdata->desc.disable_val);
}

static struct platform_driver bq257xx_reg_driver = {
	.driver = {
		.name = "bq257xx-regulator",
		.pm = pm_sleep_ptr(&bq257xx_regulator_pm_ops),
	},
	.probe = bq257xx_regulator_probe,
	.shutdown = bq257xx_regulator_shutdown,
};

module_platform_driver(bq257xx_reg_driver);

MODULE_DESCRIPTION("bq257xx regulator driver");
MODULE_AUTHOR("Chris Morgan <macromorgan@hotmail.com>");
MODULE_LICENSE("GPL");
