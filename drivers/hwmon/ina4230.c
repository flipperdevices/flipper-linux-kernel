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
#include <linux/i2c.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
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
#define INA4230_DEVICE_ID			0x7F

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
/* Longest wait for fresh data after reset recovery, under the hwmon lock */
#define INA4230_RECOVERY_TIMEOUT_US		(100 * USEC_PER_MSEC)
#define INA4230_CONFIG_DEFAULT \
		(FIELD_PREP(INA4230_CONFIG1_ACTIVE_CHANNEL_MASK, 0xF) | \
		FIELD_PREP(INA4230_CONFIG1_AVG_MASK, 0x1) | \
		FIELD_PREP(INA4230_CONFIG1_VBUSCT_MASK, 0x4) | \
		FIELD_PREP(INA4230_CONFIG1_VSHCT_MASK, 0x4) | \
		FIELD_PREP(INA4230_CONFIG1_MODE_MASK, 0x7))
#define INA4230_CONFIG_CHx_EN(x) \
		FIELD_PREP(INA4230_CONFIG1_ACTIVE_CHANNEL_MASK, BIT((x)))

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
	u32 shunt_resistor;
	int shunt_gain;
	u32 max_expected_current;
	u32 current_lsb_uA;
	bool disconnected;
};

/**
 * struct ina4230_data - device specific information
 * @pm_dev: Device pointer for pm runtime
 * @regmap: Register map of the device
 * @inputs: Array of channel input source specific structures
 * @alert_active_high: flag indicating alert polarity is active high
 */
struct ina4230_data {
	struct device *pm_dev;
	struct regmap *regmap;
	struct ina4230_input inputs[INA4230_NUM_CHANNELS];
	bool alert_active_high;
};

/* Returns 1 if the channel is enabled, 0 if not, or a negative error code */
static int ina4230_is_enabled(struct ina4230_data *ina, int channel)
{
	unsigned int config;
	int ret;

	if (!pm_runtime_active(ina->pm_dev) || ina->inputs[channel].disconnected)
		return 0;

	ret = regmap_read(ina->regmap, INA4230_CONFIG1, &config);
	if (ret)
		return ret;

	return !!(config & INA4230_CONFIG_CHx_EN(channel));
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
static inline u32 ina4230_interval_ms_to_conv_time(u16 config, long interval)
{
	/* With no channel enabled, size the conversion time for one */
	u32 channels = hweight16(config & INA4230_CONFIG1_ACTIVE_CHANNEL_MASK) ?: 1;
	u32 samples_idx = FIELD_GET(INA4230_CONFIG1_AVG_MASK, config);
	u32 samples = ina4230_avg_samples[samples_idx];

	/* Maximum supported interval is 8244 * 4 * 2 * 1024 = 67534848 usec */
	interval = clamp_val(interval, 0, 67535);

	/* Bisect the result to Bus and Shunt conversion times */
	return DIV_ROUND_CLOSEST(interval * 1000 / 2, channels * samples);
}

/* Converting CONFIG register value to update_interval in usec */
static inline u32 ina4230_reg_to_interval_us(u16 config)
{
	u32 channels = hweight16(config & INA4230_CONFIG1_ACTIVE_CHANNEL_MASK);
	u32 vbus_ct_idx = FIELD_GET(INA4230_CONFIG1_VBUSCT_MASK, config);
	u32 vsh_ct_idx = FIELD_GET(INA4230_CONFIG1_VSHCT_MASK, config);
	u32 samples_idx = FIELD_GET(INA4230_CONFIG1_AVG_MASK, config);
	u32 samples = ina4230_avg_samples[samples_idx];
	u32 vbus_ct = ina4230_conv_time[vbus_ct_idx];
	u32 vsh_ct = ina4230_conv_time[vsh_ct_idx];

	/*
	 * Calculate total conversion time including the time to obtain
	 * the first averaged sample after starting conversion.
	 * Subsequent conversions will actually be available after
	 * channels * (vbus_ct + vsh_ct) usec
	 */
	return channels * (vbus_ct + vsh_ct) * samples;
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
	u64 n, d;
	u32 calibration;
	int ret;

	/* Use the 4x gain only if the full-scale drop is within 20.48 mV (in pV) */
	input->shunt_gain = (u64)input->max_expected_current * input->shunt_resistor >
			    20480ULL * 1000000 ? 1 : 4;

	ret = regmap_update_bits(ina->regmap, INA4230_CONFIG2,
				 INA4230_CONFIG2_RANGE_CH(channel),
				 input->shunt_gain == 4 ?
				 INA4230_CONFIG2_RANGE_CH(channel) : 0);
	if (ret)
		return ret;

	/* Spread a u32 current over the s16 CURRENT register: at most 2^17 */
	input->current_lsb_uA = DIV_ROUND_UP_ULL(input->max_expected_current, 32768);
	n = 5120000000ULL;
	d = (u64)input->current_lsb_uA * input->shunt_resistor * input->shunt_gain;
	/* Ensure rounding to the closest integer */
	n += d / 2;
	n = div64_u64(n, d);
	if (n > INA4230_CALIBRATION_MASK) {
		dev_err(ina->pm_dev,
			"Shunt %uuOhm too low for expected current %uuA, cannot calibrate channel %d\n",
			input->shunt_resistor, input->max_expected_current, channel + 1);
		return -ERANGE;
	}
	if (!n) {
		dev_err(ina->pm_dev,
			"Shunt %uuOhm too high for expected current %uuA, cannot calibrate channel %d\n",
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

static const u8 ina4230_curr_reg[] = {
	INA4230_CURRENT_CH1, INA4230_CURRENT_CH2,
	INA4230_CURRENT_CH3, INA4230_CURRENT_CH4
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
	unsigned int config;
	int ret;

	ret = regmap_read(ina->regmap, INA4230_CONFIG1, &config);
	if (ret)
		return ret;

	switch (attr) {
	case hwmon_chip_samples:
		*val = ina4230_avg_samples[FIELD_GET(INA4230_CONFIG1_AVG_MASK, config)];
		return 0;
	case hwmon_chip_update_interval:
		/* Return in msec */
		*val = ina4230_reg_to_interval_us(config);
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
	unsigned int regval;
	u8 reg;
	int ret;

	switch (attr) {
	case hwmon_in_input:
		reg = ina4230_in_reg[channel];

		/*
		 * Translate shunt channel index to sensor channel index
		 */
		channel %= INA4230_NUM_CHANNELS;

		ret = ina4230_is_enabled(ina, channel);
		if (ret <= 0)
			return ret ?: -ENODATA;

		ret = regmap_read(ina->regmap, reg, &regval);
		if (ret)
			return ret;

		/*
		 * Scale of shunt voltage (uV): LSB is 2.5uV or 625nV
		 *				depending on gain setting
		 * Scale of bus voltage (mV): LSB is 1.6mV
		 */
		if (is_shunt)
			*val = mult_frac((long)(s16)regval,
					 2500 / ina->inputs[channel].shunt_gain,
					 1000000);
		else
			*val = mult_frac((long)(s16)regval,
					 1600,
					 1000);
		return 0;
	case hwmon_in_enable:
		ret = ina4230_is_enabled(ina, channel);
		if (ret < 0)
			return ret;

		*val = ret;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

/*
 * Restore the cached configuration to the chip. Writing CONFIG1 restarts the
 * conversions, so write it last, once the ranges and calibration are in place.
 */
static int ina4230_restore(struct ina4230_data *ina)
{
	int ret;

	ret = regcache_sync_region(ina->regmap, 0, INA4230_CONFIG1 - 1);
	if (ret)
		return ret;

	ret = regcache_sync_region(ina->regmap, INA4230_CONFIG2,
				   INA4230_DEVICE_ID);
	if (ret)
		return ret;

	return regcache_sync_region(ina->regmap, INA4230_CONFIG1, INA4230_CONFIG1);
}

/*
 * The chip returns to its power-up defaults after an externally triggered
 * reset, such as an I2C general call, or a voltage glitch. Its calibration is
 * then zero and so are all current, power and energy readings. When a reading
 * comes back zero, check the calibration and restore the configuration if the
 * chip has lost it.
 *
 * Returns 1 if the chip was reinitialized, 0 if it was not reset, or a
 * negative error code.
 */
static int ina4230_recover_reset(struct ina4230_data *ina, int channel)
{
	unsigned int cal, config, flags;
	u32 timeout;
	int ret;

	ret = regmap_read_bypassed(ina->regmap,
				   ina4230_calibration_reg[channel], &cal);
	if (ret || cal)
		return ret;

	dev_warn_ratelimited(ina->pm_dev, "chip was reset, reinitializing\n");

	ret = ina4230_restore(ina);
	if (ret)
		return ret;

	ret = regmap_read(ina->regmap, INA4230_CONFIG1, &config);
	if (ret)
		return ret;

	/*
	 * Restoring CONFIG1 restarts the conversions. Wait for the first cycle,
	 * but within a bound: with long averaging a cycle takes over a minute.
	 */
	timeout = min(2 * ina4230_reg_to_interval_us(config),
		      INA4230_RECOVERY_TIMEOUT_US);
	ret = regmap_read_poll_timeout(ina->regmap, INA4230_FLAGS, flags,
				       flags & INA4230_FLAGS_CVRF,
				       USEC_PER_MSEC, timeout);
	if (ret == -ETIMEDOUT)
		return -EAGAIN;
	if (ret)
		return ret;

	return 1;
}

static int ina4230_read_checked(struct ina4230_data *ina, int channel,
				unsigned int reg, unsigned int *regval)
{
	int ret;

	ret = regmap_read(ina->regmap, reg, regval);
	if (ret || *regval)
		return ret;

	ret = ina4230_recover_reset(ina, channel);
	if (ret <= 0)
		return ret;

	return regmap_read(ina->regmap, reg, regval);
}

static int ina4230_read_power(struct device *dev, u32 attr, int channel, long *val)
{
	struct ina4230_data *ina = dev_get_drvdata(dev);
	unsigned int regval;
	int ret;

	switch (attr) {
	case hwmon_power_input:
		ret = ina4230_is_enabled(ina, channel);
		if (ret <= 0)
			return ret ?: -ENODATA;

		ret = ina4230_read_checked(ina, channel,
					   ina4230_power_reg[channel], &regval);
		if (ret)
			return ret;

		/*
		 * Power = 32 x CURRENT_LSB x POWER (datasheet equation 4).
		 * u16 * 2^17 * 32 needs 38 bits, more than a 32-bit long holds.
		 */
		*val = min_t(u64, (u64)regval *
			     ina->inputs[channel].current_lsb_uA * 32, LONG_MAX);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int ina4230_read_energy(struct device *dev, u32 attr, int channel, s64 *val)
{
	struct ina4230_data *ina = dev_get_drvdata(dev);
	u8 reg = ina4230_energy_reg[channel];
	int ret;
	__be32 regval;

	switch (attr) {
	case hwmon_energy_input:
		ret = ina4230_is_enabled(ina, channel);
		if (ret <= 0)
			return ret ?: -ENODATA;

		/*
		 * Read four bytes without incrementing the register address,
		 * which the device returns in its endianness format (BE32).
		 * Note that this internally produces a single 4-byte I2C read
		 * transaction and not two consecutive 16-bit register reads,
		 * so the result is in fact a four-byte big-endian number
		 */
		ret = regmap_noinc_read(ina->regmap, reg, &regval, sizeof(regval));
		if (ret)
			return ret;

		/* A reset restarts the accumulation from zero */
		if (!regval) {
			ret = ina4230_recover_reset(ina, channel);
			if (ret < 0)
				return ret;
		}

		/*
		 * Energy = 32 x CURRENT_LSB x ENERGY (datasheet equation 5).
		 * u32 * 2^17 * 32 needs 54 bits, hence the 64-bit energy attribute.
		 */
		*val = (u64)be32_to_cpu(regval) *
			ina->inputs[channel].current_lsb_uA * 32;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int ina4230_read_curr(struct device *dev, u32 attr,
			     int channel, long *val)
{
	struct ina4230_data *ina = dev_get_drvdata(dev);
	unsigned int regval;
	int ret;

	switch (attr) {
	case hwmon_curr_input:
		ret = ina4230_is_enabled(ina, channel);
		if (ret <= 0)
			return ret ?: -ENODATA;

		ret = ina4230_read_checked(ina, channel,
					   ina4230_curr_reg[channel], &regval);
		if (ret)
			return ret;

		/* s16 * 2^17 needs 33 bits; divided by 1000 it fits a long */
		*val = div_s64((s64)(s16)regval *
			       ina->inputs[channel].current_lsb_uA, 1000);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int ina4230_write_chip(struct device *dev, u32 attr, long val)
{
	struct ina4230_data *ina = dev_get_drvdata(dev);
	unsigned int config;
	int idx, ret;
	u32 tmp;

	switch (attr) {
	case hwmon_chip_samples:
		idx = find_closest(val, ina4230_avg_samples,
				   ARRAY_SIZE(ina4230_avg_samples));

		return regmap_update_bits(ina->regmap, INA4230_CONFIG1,
					  INA4230_CONFIG1_AVG_MASK,
					  FIELD_PREP(INA4230_CONFIG1_AVG_MASK, idx));
	case hwmon_chip_update_interval:
		ret = regmap_read(ina->regmap, INA4230_CONFIG1, &config);
		if (ret)
			return ret;

		tmp = ina4230_interval_ms_to_conv_time(config, val);
		idx = find_closest(tmp, ina4230_conv_time,
				   ARRAY_SIZE(ina4230_conv_time));

		return regmap_update_bits(ina->regmap, INA4230_CONFIG1,
					  INA4230_CONFIG1_VBUSCT_MASK |
					  INA4230_CONFIG1_VSHCT_MASK,
					  FIELD_PREP(INA4230_CONFIG1_VBUSCT_MASK, idx) |
					  FIELD_PREP(INA4230_CONFIG1_VSHCT_MASK, idx));
	default:
		return -EOPNOTSUPP;
	}
}

static int ina4230_write_enable(struct device *dev, int channel, bool enable)
{
	struct ina4230_data *ina = dev_get_drvdata(dev);
	unsigned int config, mask = INA4230_CONFIG_CHx_EN(channel);
	int ret;

	ret = regmap_read(ina->regmap, INA4230_CONFIG1, &config);
	if (ret)
		return ret;

	/* Bypass if enable status is not being changed */
	if (!!(config & mask) == enable)
		return 0;

	if (enable && ina->inputs[channel].disconnected)
		return -ENODEV;

	/* For enabling routine, increase refcount and resume() at first */
	if (enable) {
		ret = pm_runtime_resume_and_get(ina->pm_dev);
		if (ret < 0) {
			dev_err(dev, "Failed to get PM runtime\n");
			return ret;
		}
	}

	/*
	 * Even if the bus write fails, the cache takes the new value for the next
	 * sync to apply, so the reference follows the cached enable bit.
	 */
	ret = regmap_update_bits(ina->regmap, INA4230_CONFIG1, mask,
				 enable ? mask : 0);

	/* For disabling routine, decrease refcount or suspend() at last */
	if (!enable)
		pm_runtime_put_sync(ina->pm_dev);

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
		ret = ina4230_read_in(dev, attr, channel, val);
		break;
	case hwmon_curr:
		ret = ina4230_read_curr(dev, attr, channel, val);
		break;
	case hwmon_power:
		ret = ina4230_read_power(dev, attr, channel, val);
		break;
	case hwmon_energy64:
		ret = ina4230_read_energy(dev, attr, channel, (s64 *)val);
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
		if (val != 0 && val != 1)
			return -EINVAL;

		ret = ina4230_write_enable(dev, channel, val);
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

	*str = ina->inputs[channel].label;

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
		switch (attr) {
		case hwmon_in_label:
			if (channel <= INA4230_CHANNEL4)
				input = &ina->inputs[channel];
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
	case hwmon_energy64:
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
			   /* 0-3: input voltage Channels */
			   HWMON_I_INPUT | HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_ENABLE | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_ENABLE | HWMON_I_LABEL,
			   /* 4-7: shunt voltage Channels */
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
	HWMON_CHANNEL_INFO(energy64,
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

static const struct regmap_range ina4230_vol_ranges[] = {
	regmap_reg_range(INA4230_SHUNT_VOLTAGE_CH1, INA4230_ENERGY_CH1),
	regmap_reg_range(INA4230_SHUNT_VOLTAGE_CH2, INA4230_ENERGY_CH2),
	regmap_reg_range(INA4230_SHUNT_VOLTAGE_CH3, INA4230_ENERGY_CH3),
	regmap_reg_range(INA4230_SHUNT_VOLTAGE_CH4, INA4230_ENERGY_CH4),
	/*
	 * CONFIG2 stays cached despite its self-clearing RST and ACC_RST bits:
	 * the driver sets RST only below regmap and never sets ACC_RST, so the
	 * cache holds the configuration to restore after a reset.
	 */
	regmap_reg_range(INA4230_FLAGS, INA4230_FLAGS),
};

static const struct regmap_access_table ina4230_volatile_table = {
	.yes_ranges = ina4230_vol_ranges,
	.n_yes_ranges = ARRAY_SIZE(ina4230_vol_ranges),
};

static const struct regmap_range ina4230_rd_ranges[] = {
	regmap_reg_range(INA4230_SHUNT_VOLTAGE_CH1, INA4230_FLAGS),
	regmap_reg_range(INA4230_MANUFACTURER_ID, INA4230_DEVICE_ID),
};

static const struct regmap_access_table ina4230_rd_table = {
	.yes_ranges = ina4230_rd_ranges,
	.n_yes_ranges = ARRAY_SIZE(ina4230_rd_ranges),
};

static const struct regmap_range ina4230_wr_ranges[] = {
	regmap_reg_range(INA4230_CALIBRATION_CH1, INA4230_ALERT_CONFIG1),
	regmap_reg_range(INA4230_CALIBRATION_CH2, INA4230_ALERT_CONFIG2),
	regmap_reg_range(INA4230_CALIBRATION_CH3, INA4230_ALERT_CONFIG3),
	regmap_reg_range(INA4230_CALIBRATION_CH4, INA4230_ALERT_CONFIG4),
	regmap_reg_range(INA4230_CONFIG1, INA4230_CONFIG2),
};

static const struct regmap_access_table ina4230_wr_table = {
	.yes_ranges = ina4230_wr_ranges,
	.n_yes_ranges = ARRAY_SIZE(ina4230_wr_ranges),
};

static const struct regmap_config ina4230_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = INA4230_DEVICE_ID,
	.use_single_write = true,

	.cache_type = REGCACHE_MAPLE,
	.rd_table = &ina4230_rd_table,
	.wr_table = &ina4230_wr_table,
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
		if (!val)
			return dev_err_probe(dev, -EINVAL,
				"invalid shunt resistor value %u of %pOFn\n",
				val, child);

		input->shunt_resistor = val;
	}

	/* Save the expected maxcurrent */
	if (!of_property_read_u32(child, "ti,maximum-expected-current-microamp", &val)) {
		if (val < 32768)
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

/* Reset the chip and restore whatever configuration the cache holds */
static int ina4230_init_chip(struct ina4230_data *ina)
{
	int ret;

	/* Reset below regmap, keeping the self-clearing RST bit out of the cache */
	ret = i2c_smbus_write_word_swapped(to_i2c_client(ina->pm_dev),
					   INA4230_CONFIG2, INA4230_CONFIG2_RST);
	if (ret) {
		dev_err(ina->pm_dev, "Unable to reset device\n");
		return ret;
	}

	return ina4230_restore(ina);
}

static void ina4230_pm_disable(void *data)
{
	struct ina4230_data *ina = data;
	unsigned int config;
	int i;

	pm_runtime_disable(ina->pm_dev);
	pm_runtime_set_suspended(ina->pm_dev);

	/* A cached register, so this cannot reach the bus and fail */
	regmap_read(ina->regmap, INA4230_CONFIG1, &config);

	/* Drop the references held for the enabled channels */
	for (i = 0; i < INA4230_NUM_CHANNELS; i++) {
		if (config & INA4230_CONFIG_CHx_EN(i))
			pm_runtime_put_noidle(ina->pm_dev);
	}
}

static int ina4230_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	unsigned int config1, config2 = 0;
	struct ina4230_data *ina;
	struct device *hwmon_dev;
	int i, ret;

	ina = devm_kzalloc(dev, sizeof(*ina), GFP_KERNEL);
	if (!ina)
		return -ENOMEM;

	ina->regmap = devm_regmap_init_i2c(client, &ina4230_regmap_config);
	if (IS_ERR(ina->regmap))
		return PTR_ERR(ina->regmap);

	for (i = 0; i < INA4230_NUM_CHANNELS; i++) {
		ina->inputs[i].shunt_resistor = INA4230_RSHUNT_DEFAULT;
		/* Default for 1mA LSB current measurements */
		ina->inputs[i].max_expected_current = 32768000;
	}

	ret = ina4230_probe_from_dt(dev, ina);
	if (ret)
		return dev_err_probe(dev, ret,
			"Unable to probe from device tree\n");

	/* Start from the driver default configuration */
	config1 = INA4230_CONFIG_DEFAULT;

	if (ina->alert_active_high)
		config2 |= INA4230_CONFIG2_ALERT_POL;

	/* Disable channels if their inputs are disconnected */
	for (i = 0; i < INA4230_NUM_CHANNELS; i++) {
		if (ina->inputs[i].disconnected)
			config1 &= ~INA4230_CONFIG_CHx_EN(i);
	}

	ina->pm_dev = dev;
	dev_set_drvdata(dev, ina);

	ret = ina4230_init_chip(ina);
	if (ret)
		return ret;

	/*
	 * Program the configuration, which also puts it in the cache that later
	 * reads, updates and restores rely on. CONFIG1 goes last, as writing it
	 * restarts the conversions.
	 */
	ret = regmap_write(ina->regmap, INA4230_CONFIG2, config2);
	if (ret)
		return ret;

	for (i = 0; i < INA4230_NUM_CHANNELS; i++) {
		if (!ina->inputs[i].disconnected) {
			ret = ina4230_set_calibration(ina, i);
			if (ret)
				return ret;
		}
	}

	ret = regmap_write(ina->regmap, INA4230_CONFIG1, config1);
	if (ret)
		return ret;

	/* Hold one runtime PM reference per enabled channel */
	pm_runtime_set_active(dev);
	for (i = 0; i < INA4230_NUM_CHANNELS; i++) {
		if (config1 & INA4230_CONFIG_CHx_EN(i))
			pm_runtime_get_noresume(dev);
	}
	pm_runtime_enable(dev);

	/* Registered first, so it runs only after the hwmon device is gone */
	ret = devm_add_action_or_reset(dev, ina4230_pm_disable, ina);
	if (ret)
		return ret;

	hwmon_dev = devm_hwmon_device_register_with_info(dev, client->name, ina,
							 &ina4230_chip_info,
							 NULL);
	if (IS_ERR(hwmon_dev))
		return dev_err_probe(dev, PTR_ERR(hwmon_dev),
			"Unable to register hwmon device\n");

	return 0;
}

static int ina4230_suspend(struct device *dev)
{
	struct ina4230_data *ina = dev_get_drvdata(dev);

	regcache_cache_only(ina->regmap, true);

	return 0;
}

static int ina4230_resume(struct device *dev)
{
	struct ina4230_data *ina = dev_get_drvdata(dev);

	regcache_cache_only(ina->regmap, false);

	return ina4230_init_chip(ina);
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
