// SPDX-License-Identifier: GPL-2.0-only
/*
 * INA4230 Quad Current/Voltage Monitor
 *
 * Based on INA3221 driver by Texas Instruments Incorporated - https://www.ti.com/
 * Adapted for INA4230 by Alexey Charkov <alchark@flipper.net>
 */

#include <linux/bitfield.h>
#include <linux/byteorder/generic.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/i2c.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/util_macros.h>

#define INA4230_DRIVER_NAME			"ina4230"

#define INA4230_SHUNT_VOLTAGE_CH1		0x00
#define INA4230_BUS_VOLTAGE_CH1			0x01
#define INA4230_CURRENT_CH1			0x02
#define INA4230_POWER_CH1			0x03
#define INA4230_ENERGY_CH1			0x04
#define INA4230_CALIBRATION_CH1			0x05
#define INA4230_ALERT_LIMIT1			0x06
#define INA4230_ALERT_CONFIG1			0x07
#define INA4230_SHUNT_VOLTAGE_CH2		0x08
#define INA4230_BUS_VOLTAGE_CH2			0x09
#define INA4230_CURRENT_CH2			0x0A
#define INA4230_POWER_CH2			0x0B
#define INA4230_ENERGY_CH2			0x0C
#define INA4230_CALIBRATION_CH2			0x0D
#define INA4230_ALERT_LIMIT2			0x0E
#define INA4230_ALERT_CONFIG2			0x0F
#define INA4230_SHUNT_VOLTAGE_CH3		0x10
#define INA4230_BUS_VOLTAGE_CH3			0x11
#define INA4230_CURRENT_CH3			0x12
#define INA4230_POWER_CH3			0x13
#define INA4230_ENERGY_CH3			0x14
#define INA4230_CALIBRATION_CH3			0x15
#define INA4230_ALERT_LIMIT3			0x16
#define INA4230_ALERT_CONFIG3			0x17
#define INA4230_SHUNT_VOLTAGE_CH4		0x18
#define INA4230_BUS_VOLTAGE_CH4			0x19
#define INA4230_CURRENT_CH4			0x1A
#define INA4230_POWER_CH4			0x1B
#define INA4230_ENERGY_CH4			0x1C
#define INA4230_CALIBRATION_CH4			0x1D
#define INA4230_ALERT_LIMIT4			0x1E
#define INA4230_ALERT_CONFIG4			0x1F
#define INA4230_CONFIG1				0x20
#define INA4230_CONFIG2				0x21
#define INA4230_FLAGS				0x22
#define INA4230_MANUFACTURER_ID			0x7E

#define INA4230_CALIBRATION_MASK		GENMASK(14, 0)

#define INA4230_ALERT_CHANNEL_MASK		GENMASK(4, 3)
#define INA4230_ALERT_MASK			GENMASK(2, 0)
/* Shunt voltage over limit */
#define INA4230_ALERT_MASK_SOL			0x1
/* Shunt voltage under limit */
#define INA4230_ALERT_MASK_SUL			0x2
/* Bus voltage over limit */
#define INA4230_ALERT_MASK_BOL			0x3
/* Bus voltage under limit */
#define INA4230_ALERT_MASK_BUL			0x4
/* Power over limit */
#define INA4230_ALERT_MASK_POL			0x5

#define INA4230_CONFIG1_ACTIVE_CHANNEL_MASK	GENMASK(15, 12)
#define INA4230_CONFIG1_AVG_MASK		GENMASK(11, 9)
#define INA4230_CONFIG1_VBUSCT_MASK		GENMASK(8, 6)
#define INA4230_CONFIG1_VSHCT_MASK		GENMASK(5, 3)
#define INA4230_CONFIG1_MODE_MASK		GENMASK(2, 0)
#define INA4230_MODE_POWERDOWN			0
#define INA4230_MODE_SHUNT_SINGLE		1
#define INA4230_MODE_BUS_SINGLE			2
#define INA4230_MODE_BUS_SHUNT_SINGLE		3
#define INA4230_MODE_POWERDOWN1			4
#define INA4230_MODE_SHUNT_CONTINUOUS		5
#define INA4230_MODE_BUS_CONTINUOUS		6
#define INA4230_MODE_BUS_SHUNT_CONTINUOUS	7

#define INA4230_CONFIG2_RST			BIT(15)
#define INA4230_CONFIG2_ACC_RST_MASK		GENMASK(11, 8)
#define INA4230_CONFIG2_CNVR_MASK		BIT(7)
#define INA4230_CONFIG2_ENOF_MASK		BIT(6)
#define INA4230_CONFIG2_ALERT_LATCH		BIT(5)
#define INA4230_CONFIG2_ALERT_POL		BIT(4)
#define INA4230_CONFIG2_RANGE_MASK		GENMASK(3, 0)
#define INA4230_CONFIG2_RANGE_CH(x) \
		FIELD_PREP(INA4230_CONFIG2_RANGE_MASK, BIT((x)))

#define INA4230_FLAGS_LIMIT4_ALERT		BIT(15)
#define INA4230_FLAGS_LIMIT3_ALERT		BIT(14)
#define INA4230_FLAGS_LIMIT2_ALERT		BIT(13)
#define INA4230_FLAGS_LIMIT1_ALERT		BIT(12)
#define INA4230_FLAGS_ENERGY_OVERFLOW_CH4	BIT(11)
#define INA4230_FLAGS_ENERGY_OVERFLOW_CH3	BIT(10)
#define INA4230_FLAGS_ENERGY_OVERFLOW_CH2	BIT(9)
#define INA4230_FLAGS_ENERGY_OVERFLOW_CH1	BIT(8)
#define INA4230_FLAGS_CVRF			BIT(7)
#define INA4230_FLAGS_MATH_OVERFLOW		BIT(6)

#define INA4230_RSHUNT_DEFAULT			10000
#define INA4230_CONFIG_DEFAULT \
		(FIELD_PREP(INA4230_CONFIG1_ACTIVE_CHANNEL_MASK, 0xF) | \
		FIELD_PREP(INA4230_CONFIG1_AVG_MASK, 0x1) | \
		FIELD_PREP(INA4230_CONFIG1_VBUSCT_MASK, 0x4) | \
		FIELD_PREP(INA4230_CONFIG1_VSHCT_MASK, 0x4) | \
		FIELD_PREP(INA4230_CONFIG1_MODE_MASK, 0x7))
#define INA4230_CONFIG_CHx_EN(x) \
		FIELD_PREP(INA4230_CONFIG1_ACTIVE_CHANNEL_MASK, BIT((x)))

enum ina4230_fields {
	/* Alert configuration settings: channel masks */
	F_ALERT1_CH, F_ALERT2_CH, F_ALERT3_CH, F_ALERT4_CH,
	/* Alert configuration settings: alert masks */
	F_ALERT1_TYPE, F_ALERT2_TYPE, F_ALERT3_TYPE, F_ALERT4_TYPE,
	/* Configuration registers */
	F_CH_EN, F_AVG, F_VBUSCT, F_VSHCT, F_MODE,
	F_RST, F_ACC_RST, F_CNV_ALERT, F_ENOF, F_ALERT_LATCH, F_ALERT_POL, F_RANGE,
	/* Status flags */
	F_LIMIT1_ALERT, F_LIMIT2_ALERT, F_LIMIT3_ALERT, F_LIMIT4_ALERT,
	F_ENERGY_OVERFLOW_CH1, F_ENERGY_OVERFLOW_CH2, F_ENERGY_OVERFLOW_CH3, F_ENERGY_OVERFLOW_CH4,
	F_CVRF, F_MATH_OVERFLOW,
	/* sentinel */
	F_MAX_FIELDS
};

static const struct reg_field ina4230_reg_fields[] = {
	[F_ALERT1_CH] = REG_FIELD(INA4230_ALERT_CONFIG1, 3, 4),
	[F_ALERT2_CH] = REG_FIELD(INA4230_ALERT_CONFIG2, 3, 4),
	[F_ALERT3_CH] = REG_FIELD(INA4230_ALERT_CONFIG3, 3, 4),
	[F_ALERT4_CH] = REG_FIELD(INA4230_ALERT_CONFIG4, 3, 4),

	[F_ALERT1_TYPE] = REG_FIELD(INA4230_ALERT_CONFIG1, 0, 2),
	[F_ALERT2_TYPE] = REG_FIELD(INA4230_ALERT_CONFIG2, 0, 2),
	[F_ALERT3_TYPE] = REG_FIELD(INA4230_ALERT_CONFIG3, 0, 2),
	[F_ALERT4_TYPE] = REG_FIELD(INA4230_ALERT_CONFIG4, 0, 2),

	[F_CH_EN] = REG_FIELD(INA4230_CONFIG1, 12, 15),
	[F_AVG] = REG_FIELD(INA4230_CONFIG1, 9, 11),
	[F_VBUSCT] = REG_FIELD(INA4230_CONFIG1, 6, 8),
	[F_VSHCT] = REG_FIELD(INA4230_CONFIG1, 3, 5),
	[F_MODE] = REG_FIELD(INA4230_CONFIG1, 0, 2),
	[F_RST] = REG_FIELD(INA4230_CONFIG2, 15, 15),
	[F_ACC_RST] = REG_FIELD(INA4230_CONFIG2, 8, 11),
	[F_CNV_ALERT] = REG_FIELD(INA4230_CONFIG2, 7, 7),
	[F_ENOF] = REG_FIELD(INA4230_CONFIG2, 6, 6),
	[F_ALERT_LATCH] = REG_FIELD(INA4230_CONFIG2, 5, 5),
	[F_ALERT_POL] = REG_FIELD(INA4230_CONFIG2, 4, 4),
	[F_RANGE] = REG_FIELD(INA4230_CONFIG2, 0, 3),

	[F_LIMIT1_ALERT] = REG_FIELD(INA4230_FLAGS, 12, 12),
	[F_LIMIT2_ALERT] = REG_FIELD(INA4230_FLAGS, 13, 13),
	[F_LIMIT3_ALERT] = REG_FIELD(INA4230_FLAGS, 14, 14),
	[F_LIMIT4_ALERT] = REG_FIELD(INA4230_FLAGS, 15, 15),
	[F_ENERGY_OVERFLOW_CH1] = REG_FIELD(INA4230_FLAGS, 8, 8),
	[F_ENERGY_OVERFLOW_CH2] = REG_FIELD(INA4230_FLAGS, 9, 9),
	[F_ENERGY_OVERFLOW_CH3] = REG_FIELD(INA4230_FLAGS, 10, 10),
	[F_ENERGY_OVERFLOW_CH4] = REG_FIELD(INA4230_FLAGS, 11, 11),
	[F_CVRF] = REG_FIELD(INA4230_FLAGS, 7, 7),
	[F_MATH_OVERFLOW] = REG_FIELD(INA4230_FLAGS, 6, 6),
};

enum ina4230_channels {
	INA4230_CHANNEL1,
	INA4230_CHANNEL2,
	INA4230_CHANNEL3,
	INA4230_CHANNEL4,
	INA4230_NUM_CHANNELS
};

/**
 * struct ina4230_input - channel input source specific information
 * @label: label of channel input source
 * @shunt_resistor: shunt resistor value of channel input source
 * @shunt_gain: gain of shunt voltage for current calculation
 * @max_expected_current: maximum expected current in micro-Ampere for ADC
 *			  calibration
 * @current_lsb_uA: current LSB in micro-Amperes
 * @disconnected: connection status of channel input source
 */
struct ina4230_input {
	const char *label;
	int shunt_resistor;
	int shunt_gain;
	int max_expected_current;
	int current_lsb_uA;
	bool disconnected;
};

/**
 * struct ina4230_data - device specific information
 * @pm_dev: Device pointer for pm runtime
 * @regmap: Register map of the device
 * @fields: Register fields of the device
 * @inputs: Array of channel input source specific structures
 * @reg_config1: cached value of CONFIG1 register
 * @reg_config2: cached value of CONFIG2 register
 * @alert_active_high: flag indicating alert polarity is active high
 */
struct ina4230_data {
	struct device *pm_dev;
	struct regmap *regmap;
	struct regmap_field *fields[F_MAX_FIELDS];
	struct ina4230_input inputs[INA4230_NUM_CHANNELS];
	unsigned int reg_config1;
	unsigned int reg_config2;
	bool alert_active_high;
};

static inline bool ina4230_is_enabled(struct ina4230_data *ina, int channel)
{
	return pm_runtime_active(ina->pm_dev) &&
		!ina->inputs[channel].disconnected &&
		ina->reg_config1 & INA4230_CONFIG_CHx_EN(channel);
}

/* Lookup table for Bus and Shunt conversion times in usec */
static const u16 ina4230_conv_time[] = {
	140, 204, 332, 588, 1100, 2116, 4156, 8244,
};

/* Lookup table for number of samples used in averaging mode */
static const int ina4230_avg_samples[] = {
	1, 4, 16, 64, 128, 256, 512, 1024,
};

/* Converting update_interval in msec to conversion time in usec */
static inline u32 ina4230_interval_ms_to_conv_time(u16 config, int interval)
{
	u32 channels = hweight16(config & INA4230_CONFIG1_ACTIVE_CHANNEL_MASK);
	u32 samples_idx = FIELD_GET(INA4230_CONFIG1_AVG_MASK, config);
	u32 samples = ina4230_avg_samples[samples_idx];

	/* Bisect the result to Bus and Shunt conversion times */
	return DIV_ROUND_CLOSEST(interval * 1000 / 2, channels * samples);
}

/* Converting CONFIG register value to update_interval in usec */
static inline u32 ina4230_reg_to_interval_us(u16 config)
{
	u32 channels = hweight16(config & INA4230_CONFIG1_ACTIVE_CHANNEL_MASK);
	u32 vbus_ct_idx = FIELD_GET(INA4230_CONFIG1_VBUSCT_MASK, config);
	u32 vsh_ct_idx = FIELD_GET(INA4230_CONFIG1_VSHCT_MASK, config);
	u32 vbus_ct = ina4230_conv_time[vbus_ct_idx];
	u32 vsh_ct = ina4230_conv_time[vsh_ct_idx];

	/* Calculate total conversion time */
	return channels * (vbus_ct + vsh_ct);
}

static const u8 ina4230_calibration_reg[] = {
	INA4230_CALIBRATION_CH1,
	INA4230_CALIBRATION_CH2,
	INA4230_CALIBRATION_CH3,
	INA4230_CALIBRATION_CH4,
};

static int ina4230_set_calibration(struct ina4230_data *ina, int channel)
{
	struct ina4230_input *input = &ina->inputs[channel];
	u8 reg = ina4230_calibration_reg[channel];
	int shunt_range_uV, ret;
	u32 calibration;
	u64 n, d;

	shunt_range_uV = mult_frac(input->max_expected_current,
				   input->shunt_resistor,
				   1000000);
	input->shunt_gain = shunt_range_uV > 20480 ? 1 : 4;
	ina->reg_config2 &= ~INA4230_CONFIG2_RANGE_CH(channel);
	if (input->shunt_gain == 4)
		ina->reg_config2 |= INA4230_CONFIG2_RANGE_CH(channel);

	ret = regmap_write(ina->regmap, INA4230_CONFIG2, ina->reg_config2);
	if (ret)
		return ret;

	input->current_lsb_uA = DIV_ROUND_UP(input->max_expected_current, 32768);
	n = 5120000000ULL;
	d = (u64)input->current_lsb_uA * input->shunt_resistor * input->shunt_gain;
	/* Ensure rounding to the closest integer */
	n += d / 2;
	n = div64_u64(n, d);
	if (n > INA4230_CALIBRATION_MASK) {
		dev_err(ina->pm_dev,
			"Shunt %duOhm too low for expected current %duA, cannot calibrate channel %d\n",
			input->shunt_resistor, input->max_expected_current, channel + 1);
		return -ERANGE;
	}

	calibration = n & INA4230_CALIBRATION_MASK;

	return regmap_write(ina->regmap, reg, calibration);
}

static const u8 ina4230_in_reg[] = {
	INA4230_BUS_VOLTAGE_CH1,
	INA4230_BUS_VOLTAGE_CH2,
	INA4230_BUS_VOLTAGE_CH3,
	INA4230_BUS_VOLTAGE_CH4,
	INA4230_SHUNT_VOLTAGE_CH1,
	INA4230_SHUNT_VOLTAGE_CH2,
	INA4230_SHUNT_VOLTAGE_CH3,
	INA4230_SHUNT_VOLTAGE_CH4,
};

static const u8 ina4230_curr_reg[][INA4230_NUM_CHANNELS] = {
	[hwmon_curr_input] = { INA4230_CURRENT_CH1, INA4230_CURRENT_CH2,
			       INA4230_CURRENT_CH3, INA4230_CURRENT_CH4 },
};

static const u8 ina4230_power_reg[] = {
	INA4230_POWER_CH1, INA4230_POWER_CH2, INA4230_POWER_CH3, INA4230_POWER_CH4
};

static const u8 ina4230_energy_reg[] = {
	INA4230_ENERGY_CH1, INA4230_ENERGY_CH2,
	INA4230_ENERGY_CH3, INA4230_ENERGY_CH4
};

static int ina4230_read_chip(struct device *dev, u32 attr, long *val)
{
	struct ina4230_data *ina = dev_get_drvdata(dev);
	int regval;

	switch (attr) {
	case hwmon_chip_samples:
		regval = FIELD_GET(INA4230_CONFIG1_AVG_MASK, ina->reg_config1);
		*val = ina4230_avg_samples[regval];
		return 0;
	case hwmon_chip_update_interval:
		/* Return in msec */
		*val = ina4230_reg_to_interval_us(ina->reg_config1);
		*val = DIV_ROUND_CLOSEST(*val, 1000);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int ina4230_read_in(struct device *dev, u32 attr, int channel, long *val)
{
	const bool is_shunt = channel > INA4230_CHANNEL4;
	struct ina4230_data *ina = dev_get_drvdata(dev);
	u8 reg = ina4230_in_reg[channel];
	int regval, ret;

	/*
	 * Translate shunt channel index to sensor channel index
	 */
	channel %= INA4230_NUM_CHANNELS;

	switch (attr) {
	case hwmon_in_input:
		if (!ina4230_is_enabled(ina, channel))
			return -ENODATA;

		ret = regmap_read(ina->regmap, reg, &regval);
		if (ret)
			return ret;

		/*
		 * Scale of shunt voltage (uV): LSB is 2.5uV or 625nV
		 *				depending on gain setting
		 * Scale of bus voltage (mV): LSB is 1.6mV
		 */
		if (is_shunt)
			*val = mult_frac((long)(int16_t)regval,
					 2500 / ina->inputs[channel].shunt_gain,
					 1000000);
		else
			*val = mult_frac((long)(int16_t)regval,
					 1600,
					 1000);
		return 0;
	case hwmon_in_enable:
		*val = ina4230_is_enabled(ina, channel);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int ina4230_read_power(struct device *dev, u32 attr, int channel, long *val)
{
	struct ina4230_data *ina = dev_get_drvdata(dev);
	u8 reg = ina4230_power_reg[channel];
	int regval, ret;

	switch (attr) {
	case hwmon_power_input:
		if (!ina4230_is_enabled(ina, channel))
			return -ENODATA;

		ret = regmap_read(ina->regmap, reg, &regval);
		if (ret)
			return ret;

		*val = (int16_t)regval *
			(long)ina->inputs[channel].current_lsb_uA * 32;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int ina4230_read_energy(struct device *dev, u32 attr, int channel, long *val)
{
	struct ina4230_data *ina = dev_get_drvdata(dev);
	u8 reg = ina4230_energy_reg[channel];
	int ret;
	__be32 regval;

	switch (attr) {
	case hwmon_energy_input:
		if (!ina4230_is_enabled(ina, channel))
			return -ENODATA;

		ret = regmap_noinc_read(ina->regmap, reg, &regval, sizeof(regval));
		if (ret)
			return ret;

		*val = be32_to_cpu(regval) *
			(long)ina->inputs[channel].current_lsb_uA * 32;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int ina4230_read_curr(struct device *dev, u32 attr,
			     int channel, long *val)
{
	struct ina4230_data *ina = dev_get_drvdata(dev);
	u8 reg = ina4230_curr_reg[attr][channel];
	int regval, ret;

	switch (attr) {
	case hwmon_curr_input:
		if (!ina4230_is_enabled(ina, channel))
			return -ENODATA;

		ret = regmap_read(ina->regmap, reg, &regval);
		if (ret)
			return ret;

		*val = (int16_t)regval *
			(long)ina->inputs[channel].current_lsb_uA / 1000;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int ina4230_write_chip(struct device *dev, u32 attr, long val)
{
	struct ina4230_data *ina = dev_get_drvdata(dev);
	int idx;
	u32 tmp;

	switch (attr) {
	case hwmon_chip_samples:
		idx = find_closest(val, ina4230_avg_samples,
				   ARRAY_SIZE(ina4230_avg_samples));

		FIELD_MODIFY(INA4230_CONFIG1_AVG_MASK, &ina->reg_config1, idx);
		return regmap_write(ina->regmap, INA4230_CONFIG1, ina->reg_config1);
	case hwmon_chip_update_interval:
		tmp = ina4230_interval_ms_to_conv_time(ina->reg_config1, val);
		idx = find_closest(tmp, ina4230_conv_time,
				   ARRAY_SIZE(ina4230_conv_time));

		FIELD_MODIFY(INA4230_CONFIG1_VBUSCT_MASK, &ina->reg_config1, idx);
		FIELD_MODIFY(INA4230_CONFIG1_VSHCT_MASK, &ina->reg_config1, idx);
		return regmap_write(ina->regmap, INA4230_CONFIG1, ina->reg_config1);
	default:
		return -EOPNOTSUPP;
	}
}

static int ina4230_write_enable(struct device *dev, int channel, bool enable)
{
	struct ina4230_data *ina = dev_get_drvdata(dev);
	u16 config, mask = INA4230_CONFIG_CHx_EN(channel);
	u16 config_old = ina->reg_config1 & mask;
	u32 tmp;
	int ret;

	config = enable ? mask : 0;

	/* Bypass if enable status is not being changed */
	if (config_old == config)
		return 0;

	/* For enabling routine, increase refcount and resume() at first */
	if (enable) {
		ret = pm_runtime_resume_and_get(ina->pm_dev);
		if (ret < 0) {
			dev_err(dev, "Failed to get PM runtime\n");
			return ret;
		}
	}

	/* Enable or disable the channel */
	tmp = (ina->reg_config1 & ~mask) | (config & mask);
	ret = regmap_write(ina->regmap, INA4230_CONFIG1, tmp);
	if (ret)
		goto fail;

	/* Cache the latest config register value */
	ina->reg_config1 = tmp;

	/* For disabling routine, decrease refcount or suspend() at last */
	if (!enable)
		pm_runtime_put_sync(ina->pm_dev);

	return 0;

fail:
	if (enable) {
		dev_err(dev, "Failed to enable channel %d: error %d\n",
			channel, ret);
		pm_runtime_put_sync(ina->pm_dev);
	}

	return ret;
}

static int ina4230_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	int ret;

	switch (type) {
	case hwmon_chip:
		ret = ina4230_read_chip(dev, attr, val);
		break;
	case hwmon_in:
		/* 0-align channel ID */
		ret = ina4230_read_in(dev, attr, channel - 1, val);
		break;
	case hwmon_curr:
		ret = ina4230_read_curr(dev, attr, channel, val);
		break;
	case hwmon_power:
		ret = ina4230_read_power(dev, attr, channel, val);
		break;
	case hwmon_energy:
		ret = ina4230_read_energy(dev, attr, channel, val);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}
	return ret;
}

static int ina4230_write(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long val)
{
	int ret;

	switch (type) {
	case hwmon_chip:
		ret = ina4230_write_chip(dev, attr, val);
		break;
	case hwmon_in:
		/* 0-align channel ID */
		ret = ina4230_write_enable(dev, channel - 1, val);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}
	return ret;
}

static int ina4230_read_string(struct device *dev, enum hwmon_sensor_types type,
			       u32 attr, int channel, const char **str)
{
	struct ina4230_data *ina = dev_get_drvdata(dev);
	int index = channel - 1;

	*str = ina->inputs[index].label;

	return 0;
}

static umode_t ina4230_is_visible(const void *drvdata,
				  enum hwmon_sensor_types type,
				  u32 attr, int channel)
{
	const struct ina4230_data *ina = drvdata;
	const struct ina4230_input *input = NULL;

	switch (type) {
	case hwmon_chip:
		switch (attr) {
		case hwmon_chip_samples:
		case hwmon_chip_update_interval:
			return 0644;
		default:
			return 0;
		}
	case hwmon_in:
		/* Ignore in0_ */
		if (channel == 0)
			return 0;

		switch (attr) {
		case hwmon_in_label:
			if (channel - 1 <= INA4230_CHANNEL4)
				input = &ina->inputs[channel - 1];
			/* Hide label node if label is not provided */
			return (input && input->label) ? 0444 : 0;
		case hwmon_in_input:
			return 0444;
		case hwmon_in_enable:
			return 0644;
		default:
			return 0;
		}
	case hwmon_curr:
		switch (attr) {
		case hwmon_curr_input:
			return 0444;
		default:
			return 0;
		}
	case hwmon_power:
		switch (attr) {
		case hwmon_power_input:
			return 0444;
		default:
			return 0;
		}
	case hwmon_energy:
		switch (attr) {
		case hwmon_energy_input:
			return 0444;
		default:
			return 0;
		}
	default:
		return 0;
	}
}

static const struct hwmon_channel_info * const ina4230_info[] = {
	HWMON_CHANNEL_INFO(chip,
			   HWMON_C_SAMPLES,
			   HWMON_C_UPDATE_INTERVAL),
	HWMON_CHANNEL_INFO(in,
			   /* 0: dummy, skipped in is_visible */
			   HWMON_I_INPUT,
			   /* 1-4: input voltage Channels */
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   /* 5-8: shunt voltage Channels */
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT),
	HWMON_CHANNEL_INFO(curr,
			   /* 1-4: current channels*/
			   HWMON_C_INPUT,
			   HWMON_C_INPUT,
			   HWMON_C_INPUT,
			   HWMON_C_INPUT),
	HWMON_CHANNEL_INFO(power,
			   /* 1-4: power channels*/
			   HWMON_P_INPUT,
			   HWMON_P_INPUT,
			   HWMON_P_INPUT,
			   HWMON_P_INPUT),
	HWMON_CHANNEL_INFO(energy,
			   /* 1-4: energy channels*/
			   HWMON_E_INPUT,
			   HWMON_E_INPUT,
			   HWMON_E_INPUT,
			   HWMON_E_INPUT),
	NULL
};

static const struct hwmon_ops ina4230_hwmon_ops = {
	.is_visible = ina4230_is_visible,
	.read_string = ina4230_read_string,
	.read = ina4230_read,
	.write = ina4230_write,
};

static const struct hwmon_chip_info ina4230_chip_info = {
	.ops = &ina4230_hwmon_ops,
	.info = ina4230_info,
};

/* Extra attribute groups */
static ssize_t ina4230_shunt_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct sensor_device_attribute *sd_attr = to_sensor_dev_attr(attr);
	struct ina4230_data *ina = dev_get_drvdata(dev);
	unsigned int channel = sd_attr->index;
	struct ina4230_input *input = &ina->inputs[channel];

	return sysfs_emit(buf, "%d\n", input->shunt_resistor);
}

static ssize_t ina4230_shunt_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct sensor_device_attribute *sd_attr = to_sensor_dev_attr(attr);
	struct ina4230_data *ina = dev_get_drvdata(dev);
	unsigned int channel = sd_attr->index;
	struct ina4230_input *input = &ina->inputs[channel];
	int val;
	int ret;

	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;

	val = clamp_val(val, 1, INT_MAX);

	input->shunt_resistor = val;
	ret = ina4230_set_calibration(ina, channel);
	if (ret)
		return ret;

	return count;
}

/* shunt resistance */
static SENSOR_DEVICE_ATTR_RW(shunt1_resistor, ina4230_shunt, INA4230_CHANNEL1);
static SENSOR_DEVICE_ATTR_RW(shunt2_resistor, ina4230_shunt, INA4230_CHANNEL2);
static SENSOR_DEVICE_ATTR_RW(shunt3_resistor, ina4230_shunt, INA4230_CHANNEL3);
static SENSOR_DEVICE_ATTR_RW(shunt4_resistor, ina4230_shunt, INA4230_CHANNEL4);

static struct attribute *ina4230_attrs[] = {
	&sensor_dev_attr_shunt1_resistor.dev_attr.attr,
	&sensor_dev_attr_shunt2_resistor.dev_attr.attr,
	&sensor_dev_attr_shunt3_resistor.dev_attr.attr,
	&sensor_dev_attr_shunt4_resistor.dev_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ina4230);

static const struct regmap_range ina4230_vol_ranges[] = {
	regmap_reg_range(INA4230_SHUNT_VOLTAGE_CH1, INA4230_ENERGY_CH1),
	regmap_reg_range(INA4230_SHUNT_VOLTAGE_CH2, INA4230_ENERGY_CH2),
	regmap_reg_range(INA4230_SHUNT_VOLTAGE_CH3, INA4230_ENERGY_CH3),
	regmap_reg_range(INA4230_SHUNT_VOLTAGE_CH4, INA4230_ENERGY_CH4),
	regmap_reg_range(INA4230_FLAGS, INA4230_FLAGS),
};

static const struct regmap_access_table ina4230_volatile_table = {
	.yes_ranges = ina4230_vol_ranges,
	.n_yes_ranges = ARRAY_SIZE(ina4230_vol_ranges),
};

static const struct regmap_config ina4230_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,

	.cache_type = REGCACHE_MAPLE,
	.volatile_table = &ina4230_volatile_table,
};

static int ina4230_probe_child_from_dt(struct device *dev,
				       struct device_node *child,
				       struct ina4230_data *ina)
{
	struct ina4230_input *input;
	u32 val;
	int ret;

	ret = of_property_read_u32(child, "reg", &val);
	if (ret)
		return dev_err_probe(dev, ret,
			"missing reg property of %pOFn\n", child);
	else if (val > INA4230_CHANNEL4)
		return dev_err_probe(dev, -EINVAL,
			"invalid reg %d of %pOFn\n", val, child);

	input = &ina->inputs[val];

	/* Log the disconnected channel input */
	if (!of_device_is_available(child)) {
		input->disconnected = true;
		return 0;
	}

	/* Save the connected input label if available */
	of_property_read_string(child, "label", &input->label);

	/* Overwrite default shunt resistor value optionally */
	if (!of_property_read_u32(child, "shunt-resistor-micro-ohms", &val)) {
		if (val < 1 || val > INT_MAX)
			return dev_err_probe(dev, -EINVAL,
				"invalid shunt resistor value %u of %pOFn\n",
				val, child);

		input->shunt_resistor = val;
	}

	/* Save the expected maxcurrent */
	if (!of_property_read_u32(child, "ti,maximum-expected-current-microamp", &val)) {
		if (val < 32768 || val > INT_MAX)
			return dev_err_probe(dev, -EINVAL,
				"invalid max current value %u of %pOFn\n",
				val, child);

		input->max_expected_current = val;
	}

	return 0;
}

static int ina4230_probe_from_dt(struct device *dev, struct ina4230_data *ina)
{
	const struct device_node *np = dev->of_node;
	int ret;

	/* Compatible with non-DT platforms */
	if (!np)
		return 0;

	ina->alert_active_high = of_property_read_bool(np, "ti,alert-polarity-active-high");

	for_each_child_of_node_scoped(np, child) {
		ret = ina4230_probe_child_from_dt(dev, child, ina);
		if (ret)
			return ret;
	}

	ret = devm_regulator_get_enable_optional(dev, "vs");
	if (ret && ret != -ENODEV)
		return dev_err_probe(dev, ret, "Failed to get regulator\n");

	return 0;
}

static int ina4230_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ina4230_data *ina;
	struct device *hwmon_dev;
	int i, ret;

	ina = devm_kzalloc(dev, sizeof(*ina), GFP_KERNEL);
	if (!ina)
		return -ENOMEM;

	ina->regmap = devm_regmap_init_i2c(client, &ina4230_regmap_config);
	if (IS_ERR(ina->regmap))
		return PTR_ERR(ina->regmap);

	ret = devm_regmap_field_bulk_alloc(dev, ina->regmap, ina->fields,
					   ina4230_reg_fields,
					   ARRAY_SIZE(ina4230_reg_fields));
	if (ret)
		return ret;

	for (i = 0; i < INA4230_NUM_CHANNELS; i++) {
		ina->inputs[i].shunt_resistor = INA4230_RSHUNT_DEFAULT;
		/* Default for 1mA LSB current measurements */
		ina->inputs[i].max_expected_current = 32768000;
	}

	ret = ina4230_probe_from_dt(dev, ina);
	if (ret)
		return dev_err_probe(dev, ret,
			"Unable to probe from device tree\n");

	/* The driver will be reset, so use reset value */
	ina->reg_config1 = INA4230_CONFIG_DEFAULT;
	ina->reg_config2 = 0;

	if (ina->alert_active_high)
		FIELD_MODIFY(INA4230_CONFIG2_ALERT_POL, &ina->reg_config2, 1);

	/* Disable channels if their inputs are disconnected */
	for (i = 0; i < INA4230_NUM_CHANNELS; i++) {
		if (ina->inputs[i].disconnected)
			ina->reg_config1 &= ~INA4230_CONFIG_CHx_EN(i);
	}

	ina->pm_dev = dev;
	dev_set_drvdata(dev, ina);

	/* Enable PM runtime -- status is suspended by default */
	pm_runtime_enable(ina->pm_dev);

	/* Initialize (resume) the device */
	for (i = 0; i < INA4230_NUM_CHANNELS; i++) {
		if (ina->inputs[i].disconnected)
			continue;

		/* Match the refcount with number of enabled channels */
		ret = pm_runtime_get_sync(ina->pm_dev);
		if (ret < 0)
			goto fail;
	}

	/* Set calibration values after device resume/reset */
	for (i = 0; i < INA4230_NUM_CHANNELS; i++) {
		if (!ina->inputs[i].disconnected) {
			ret = ina4230_set_calibration(ina, i);
			if (ret)
				goto fail;
		}
	}

	hwmon_dev = devm_hwmon_device_register_with_info(dev, client->name, ina,
							 &ina4230_chip_info,
							 ina4230_groups);
	if (IS_ERR(hwmon_dev)) {
		ret = dev_err_probe(dev, PTR_ERR(hwmon_dev),
			"Unable to register hwmon device\n");
		goto fail;
	}

	return 0;

fail:
	pm_runtime_disable(ina->pm_dev);
	pm_runtime_set_suspended(ina->pm_dev);
	/* pm_runtime_put_noidle() for connected channels to balance get_sync */
	for (i = 0; i < INA4230_NUM_CHANNELS; i++) {
		if (!ina->inputs[i].disconnected)
			pm_runtime_put_noidle(ina->pm_dev);
	}

	return ret;
}

static void ina4230_remove(struct i2c_client *client)
{
	struct ina4230_data *ina = dev_get_drvdata(&client->dev);
	int i;

	pm_runtime_disable(ina->pm_dev);
	pm_runtime_set_suspended(ina->pm_dev);

	/* pm_runtime_put_noidle() for connected channels to balance get_sync */
	for (i = 0; i < INA4230_NUM_CHANNELS; i++) {
		if (!ina->inputs[i].disconnected)
			pm_runtime_put_noidle(ina->pm_dev);
	}
}

static int ina4230_suspend(struct device *dev)
{
	struct ina4230_data *ina = dev_get_drvdata(dev);
	int ret;

	/* Save config register value and enable cache-only */
	ret = regmap_read(ina->regmap, INA4230_CONFIG1, &ina->reg_config1);
	if (ret)
		return ret;

	regcache_cache_only(ina->regmap, true);
	regcache_mark_dirty(ina->regmap);

	return 0;
}

static int ina4230_resume(struct device *dev)
{
	struct ina4230_data *ina = dev_get_drvdata(dev);
	int ret;

	regcache_cache_only(ina->regmap, false);

	/* Software reset the chip */
	ret = regmap_field_write(ina->fields[F_RST], true);
	if (ret) {
		dev_err(dev, "Unable to reset device\n");
		return ret;
	}

	/* Restore cached register values to hardware */
	ret = regcache_sync(ina->regmap);
	if (ret)
		return ret;

	return 0;
}

static DEFINE_RUNTIME_DEV_PM_OPS(ina4230_pm, ina4230_suspend, ina4230_resume,
				 NULL);

static const struct of_device_id ina4230_of_match_table[] = {
	{ .compatible = "ti,ina4230", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ina4230_of_match_table);

static const struct i2c_device_id ina4230_ids[] = {
	{ "ina4230" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, ina4230_ids);

static struct i2c_driver ina4230_i2c_driver = {
	.probe = ina4230_probe,
	.remove = ina4230_remove,
	.driver = {
		.name = INA4230_DRIVER_NAME,
		.of_match_table = ina4230_of_match_table,
		.pm = pm_ptr(&ina4230_pm),
	},
	.id_table = ina4230_ids,
};
module_i2c_driver(ina4230_i2c_driver);

MODULE_AUTHOR("Alexey Charkov <alchark@flipper.net>");
MODULE_DESCRIPTION("Texas Instruments INA4230 HWMon Driver");
MODULE_LICENSE("GPL");
