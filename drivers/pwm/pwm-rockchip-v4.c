// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2025 Collabora Ltd.
 *
 * A Pulse-Width-Modulation (PWM) generator driver for the generators found in
 * Rockchip SoCs such as the RK3576, internally referred to as "PWM v4". Uses
 * the MFPWM infrastructure to guarantee exclusive use over the device without
 * other functions of the device from different drivers interfering with its
 * operation while it's active.
 *
 * Technical Reference Manual: Chapter 31 of the RK3506 TRM Part 1, a SoC which
 * uses the same PWM hardware and has a publicly available TRM.
 * https://opensource.rock-chips.com/images/3/36/Rockchip_RK3506_TRM_Part_1_V1.2-20250811.pdf
 *
 * Authors:
 *     Nicolas Frattaroli <nicolas.frattaroli@collabora.com>
 *
 * Limitations:
 * - The hardware supports both completing the currently running period
 *   on disable (by switching to oneshot mode with a single repetition and
 *   only disable when the complete irq fires), and abrupt disable (freeze).
 *   Only the latter is implemented in the driver.
 * - When the output is disabled, the pin will remain driven to whatever state
 *   it last had.
 * - Adjustments to the duty cycle will only take effect during the next period.
 * - Adjustments to the period length will only take effect during the next
 *   period.
 * - The hardware only supports offsets in [0, period - duty_cycle]
 */

#include <linux/math64.h>
#include <linux/mfd/rockchip-mfpwm.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>

struct rockchip_pwm_v4 {
	struct rockchip_mfpwm_func *pwmf;
	struct pwm_chip chip;
};

struct __packed rockchip_pwm_v4_wf {
	u32 period;
	u32 duty;
	u32 offset;
	unsigned long rate;
};

static inline struct rockchip_pwm_v4 *to_rockchip_pwm_v4(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

/**
 * rockchip_pwm_v4_round_single - convert a PWM parameter to hardware
 * @rate: clock rate of the PWM clock, as per clk_get_rate
 *        Assumed to be <= 1GHz for overflow considerations
 * @in_val: parameter in nanoseconds to convert
 *
 * Returns the rounded value, saturating at U32_MAX if too large
 */
static u32 rockchip_pwm_v4_round_single(unsigned long rate, u64 in_val)
{
	u64 tmp;

	tmp = mul_u64_u64_div_u64(rate, in_val, NSEC_PER_SEC);
	if (tmp > U32_MAX)
		tmp = U32_MAX;

	return tmp;
}

/**
 * rockchip_pwm_v4_round_params - convert PWM parameters to hardware
 * @rate: PWM clock rate to do the calculations at
 * @wf: pointer to the generic &struct pwm_waveform input parameters
 * @wfhw: pointer to the hardware-specific &struct rockchip_pwm_v4_wf output
 *        parameters that the results will be stored in
 *
 * Convert nanosecond-based duty/period/offset parameters to the PWM hardware's
 * native rounded representation in number of cycles at clock rate @rate. Should
 * any of the input parameters be out of range for the hardware, the
 * corresponding output parameter is the maximum permissible value for said
 * parameter with considerations to the others.
 */
static void rockchip_pwm_v4_round_params(unsigned long rate,
					 const struct pwm_waveform *wf,
					 struct rockchip_pwm_v4_wf *wfhw)
{
	wfhw->period = rockchip_pwm_v4_round_single(rate, wf->period_length_ns);

	wfhw->duty = rockchip_pwm_v4_round_single(rate, wf->duty_length_ns);

	/* As per TRM, PWM_OFFSET: "The value ranges from 0 to (period-duty)" */
	wfhw->offset = rockchip_pwm_v4_round_single(rate, wf->duty_offset_ns);
	if (!wfhw->period) /* Don't underflow when pwm disabled */
		wfhw->offset = 0;
	else if (wfhw->offset > wfhw->period - wfhw->duty)
		wfhw->offset = wfhw->period - wfhw->duty;
}

static int rockchip_pwm_v4_round_wf_tohw(struct pwm_chip *chip,
					 struct pwm_device *pwm,
					 const struct pwm_waveform *wf,
					 void *_wfhw)
{
	struct rockchip_pwm_v4 *pc = to_rockchip_pwm_v4(chip);
	struct rockchip_pwm_v4_wf *wfhw = _wfhw;
	unsigned long rate;

	rate = clk_get_rate(pc->pwmf->core);

	/*
	 * It's unlikely this code path is ever taken, as current hardware does
	 * not expose a clock that comes anywhere close to 1GHz. However, in
	 * order to avoid even a theoretical overflow in parameter rounding,
	 * error out if this ever happens to be the case.
	 */
	if (rate > NSEC_PER_SEC)
		return -ERANGE;

	rockchip_pwm_v4_round_params(rate, wf, wfhw);

	if (wf->period_length_ns > 0)
		wfhw->rate = rate;
	else
		wfhw->rate = 0;

	dev_dbg(&chip->dev,
		"tohw: pwm#%u: %lld/%lld [+%lld] @%lu -> DUTY: %08x, PERIOD: %08x, OFFSET: %08x\n",
		pwm->hwpwm, wf->duty_length_ns, wf->period_length_ns, wf->duty_offset_ns,
		rate, wfhw->duty, wfhw->period, wfhw->offset);

	return 0;
}

static int rockchip_pwm_v4_round_wf_fromhw(struct pwm_chip *chip,
					   struct pwm_device *pwm,
					   const void *_wfhw,
					   struct pwm_waveform *wf)
{
	const struct rockchip_pwm_v4_wf *wfhw = _wfhw;
	unsigned long rate = wfhw->rate;

	if (rate) {
		wf->period_length_ns = DIV_ROUND_UP((u64)wfhw->period * NSEC_PER_SEC, rate);
		wf->duty_length_ns = DIV_ROUND_UP((u64)wfhw->duty * NSEC_PER_SEC, rate);
		wf->duty_offset_ns = DIV_ROUND_UP((u64)wfhw->offset * NSEC_PER_SEC, rate);
	} else {
		wf->period_length_ns = 0;
		wf->duty_length_ns = 0;
		wf->duty_offset_ns = 0;
	}

	dev_dbg(&chip->dev,
		"fromhw: pwm#%u: DUTY: %08x, PERIOD: %08x, OFFSET: %08x @%lu -> %lld/%lld [+%lld]\n",
		pwm->hwpwm, wfhw->duty, wfhw->period, wfhw->offset, rate,
		wf->duty_length_ns, wf->period_length_ns, wf->duty_offset_ns);

	return 0;
}

static int rockchip_pwm_v4_read_wf(struct pwm_chip *chip, struct pwm_device *pwm,
				   void *_wfhw)
{
	struct rockchip_pwm_v4 *pc = to_rockchip_pwm_v4(chip);
	struct rockchip_pwm_v4_wf *wfhw = _wfhw;
	unsigned long rate;
	int ret;

	ret = mfpwm_acquire(pc->pwmf);
	if (ret)
		return ret;

	rate = clk_get_rate(pc->pwmf->core);

	wfhw->period = mfpwm_reg_read(pc->pwmf->base, PWMV4_REG_PERIOD);
	wfhw->duty = mfpwm_reg_read(pc->pwmf->base, PWMV4_REG_DUTY);
	wfhw->offset = mfpwm_reg_read(pc->pwmf->base, PWMV4_REG_OFFSET);
	if (rockchip_pwm_v4_is_enabled(mfpwm_reg_read(pc->pwmf->base, PWMV4_REG_ENABLE)))
		wfhw->rate = rate;
	else
		wfhw->rate = 0;

	mfpwm_release(pc->pwmf);

	return 0;
}

static int rockchip_pwm_v4_write_wf(struct pwm_chip *chip, struct pwm_device *pwm,
				    const void *_wfhw)
{
	struct rockchip_pwm_v4 *pc = to_rockchip_pwm_v4(chip);
	const struct rockchip_pwm_v4_wf *wfhw = _wfhw;
	bool was_enabled;
	int ret;

	ret = mfpwm_acquire(pc->pwmf);
	if (ret)
		return ret;

	was_enabled = rockchip_pwm_v4_is_enabled(mfpwm_reg_read(pc->pwmf->base,
								PWMV4_REG_ENABLE));

	/*
	 * "But Nicolas", you ask with valid concerns, "why would you enable the
	 * PWM before setting all the parameter registers?"
	 *
	 * Excellent question, Mr. Reader M. Strawman! The RK3576 TRM Part 1
	 * Section 34.6.3 specifies that this is the intended order of writes.
	 * Doing the PWM_EN and PWM_CLK_EN writes after the params but before
	 * the CTRL_UPDATE_EN, or even after the CTRL_UPDATE_EN, results in
	 * erratic behaviour where repeated turning on and off of the PWM may
	 * not turn it off under all circumstances. This is also why we don't
	 * use relaxed writes; it's not worth the footgun.
	 */
	if (wfhw->rate)
		mfpwm_reg_write(pc->pwmf->base, PWMV4_REG_ENABLE,
				FIELD_PREP_WM16(PWMV4_EN_BOTH_MASK,
						PWMV4_EN_BOTH_MASK));
	else
		mfpwm_reg_write(pc->pwmf->base, PWMV4_REG_ENABLE,
				FIELD_PREP_WM16(PWMV4_EN_BOTH_MASK, 0));

	mfpwm_reg_write(pc->pwmf->base, PWMV4_REG_PERIOD, wfhw->period);
	mfpwm_reg_write(pc->pwmf->base, PWMV4_REG_DUTY, wfhw->duty);
	mfpwm_reg_write(pc->pwmf->base, PWMV4_REG_OFFSET, wfhw->offset);

	mfpwm_reg_write(pc->pwmf->base, PWMV4_REG_CTRL, PWMV4_CTRL_CONT_FLAGS);

	/* Commit new configuration to hardware output. */
	mfpwm_reg_write(pc->pwmf->base, PWMV4_REG_ENABLE,
			PWMV4_CTRL_UPDATE_EN);

	if (wfhw->rate) {
		if (!was_enabled) {
			dev_dbg(&chip->dev, "Enabling PWM output\n");
			ret = clk_enable(pc->pwmf->core);
			if (ret)
				goto err_mfpwm_release;
			ret = clk_set_rate_exclusive(pc->pwmf->core, wfhw->rate);
			if (ret) {
				clk_disable(pc->pwmf->core);
				goto err_mfpwm_release;
			}

			/*
			 * Output should be on now, acquire device to guarantee
			 * exclusion with other device functions while it's on.
			 *
			 * It's highly unlikely that this fails, as mfpwm has
			 * already been acquired before, and this is just a
			 * usage counter increase. Not worth the added
			 * complexity of clearing the PWMV4_REG_ENABLE again,
			 * especially considering the CTRL_UPDATE_EN behaviour.
			 */
			ret = mfpwm_acquire(pc->pwmf);
			if (ret) {
				clk_rate_exclusive_put(pc->pwmf->core);
				clk_disable(pc->pwmf->core);
				goto err_mfpwm_release;
			}
		}
	} else if (was_enabled) {
		dev_dbg(&chip->dev, "Disabling PWM output\n");
		clk_rate_exclusive_put(pc->pwmf->core);
		clk_disable(pc->pwmf->core);
		/* Output is off now, extra release to balance extra acquire */
		mfpwm_release(pc->pwmf);
	}

err_mfpwm_release:
	mfpwm_release(pc->pwmf);

	return ret;
}

static const struct pwm_ops rockchip_pwm_v4_ops = {
	.sizeof_wfhw = sizeof(struct rockchip_pwm_v4_wf),
	.round_waveform_tohw = rockchip_pwm_v4_round_wf_tohw,
	.round_waveform_fromhw = rockchip_pwm_v4_round_wf_fromhw,
	.read_waveform = rockchip_pwm_v4_read_wf,
	.write_waveform = rockchip_pwm_v4_write_wf,
};

static bool rockchip_pwm_v4_on_and_continuous(struct rockchip_pwm_v4 *pc)
{
	bool en;
	u32 val;

	en = rockchip_pwm_v4_is_enabled(mfpwm_reg_read(pc->pwmf->base,
						       PWMV4_REG_ENABLE));
	val = mfpwm_reg_read(pc->pwmf->base, PWMV4_REG_CTRL);

	return en && ((val & PWMV4_MODE_MASK) == PWMV4_MODE_CONT);
}

static int rockchip_pwm_v4_probe(struct platform_device *pdev)
{
	struct rockchip_mfpwm_func *pwmf = dev_get_platdata(&pdev->dev);
	struct rockchip_pwm_v4 *pc;
	struct pwm_chip *chip;
	struct device *dev = &pdev->dev;
	int ret;

	/*
	 * For referencing the PWM in the DT to work, we need the parent MFD
	 * device's OF node.
	 */
	dev->of_node_reused = true;
	device_set_node(dev, of_fwnode_handle(dev->parent->of_node));

	chip = devm_pwmchip_alloc(dev, 1, sizeof(*pc));
	if (IS_ERR(chip))
		return PTR_ERR(chip);

	pc = to_rockchip_pwm_v4(chip);
	pc->pwmf = pwmf;

	ret = mfpwm_acquire(pwmf);
	if (ret)
		return dev_err_probe(dev, ret, "Couldn't acquire mfpwm in probe\n");

	if (!rockchip_pwm_v4_on_and_continuous(pc))
		mfpwm_release(pwmf);
	else {
		dev_dbg(dev, "PWM was already on at probe time\n");
		ret = clk_enable(pwmf->core);
		if (ret) {
			dev_err_probe(dev, ret, "Enabling pwm clock failed\n");
			goto err_mfpwm_release;
		}
		ret = clk_rate_exclusive_get(pc->pwmf->core);
		if (ret) {
			dev_err_probe(dev, ret, "Protecting pwm clock failed\n");
			goto err_clk_disable;
		}
	}

	platform_set_drvdata(pdev, chip);

	chip->ops = &rockchip_pwm_v4_ops;

	ret = devm_pwmchip_add(dev, chip);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to add PWM chip\n");
		if (rockchip_pwm_v4_on_and_continuous(pc))
			goto err_rate_put;

		return ret;
	}

	return 0;

err_rate_put:
	clk_rate_exclusive_put(pwmf->core);
err_clk_disable:
	clk_disable(pwmf->core);
err_mfpwm_release:
	mfpwm_release(pwmf);

	return ret;
}

static const struct platform_device_id rockchip_pwm_v4_ids[] = {
	{ .name = "rockchip-pwm-v4", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(platform, rockchip_pwm_v4_ids);

static struct platform_driver rockchip_pwm_v4_driver = {
	.probe = rockchip_pwm_v4_probe,
	.driver = {
		.name = "rockchip-pwm-v4",
	},
	.id_table = rockchip_pwm_v4_ids,
};
module_platform_driver(rockchip_pwm_v4_driver);

MODULE_AUTHOR("Nicolas Frattaroli <nicolas.frattaroli@collabora.com>");
MODULE_DESCRIPTION("Rockchip PWMv4 Driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("ROCKCHIP_MFPWM");
MODULE_ALIAS("platform:pwm-rockchip-v4");
