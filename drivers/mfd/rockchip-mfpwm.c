// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2025 Collabora Ltd.
 *
 * A driver to manage all the different functionalities exposed by Rockchip's
 * PWMv4 hardware.
 *
 * This driver is chiefly focused on guaranteeing non-concurrent operation
 * between the different device functions, as well as setting the clocks.
 * It registers the device function platform devices, e.g. PWM output or
 * PWM capture.
 *
 * Authors:
 *     Nicolas Frattaroli <nicolas.frattaroli@collabora.com>
 */

#include <linux/array_size.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/mfd/core.h>
#include <linux/mfd/rockchip-mfpwm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

static const char *const pwm_clk_names[] = {
	"pwm", "osc", "rc"
};

/**
 * struct rockchip_mfpwm - private mfpwm driver instance state struct
 * @dev: pointer to this instance's &struct device
 * @base: pointer to the memory mapped registers of this device
 * @pwm_clks: array of pointers to the PLL, fixed crystal and RC oscillator
 *            &struct clk
 * @chosen_clk: a clk-mux of pwm_clk, osc_clk and rc_clk
 * @pclk: pointer to the APB bus clock needed for mmio register access
 * @active_func: pointer to the currently active device function, or %NULL if no
 *               device function is currently actively using any of the shared
 *               resources. May only be checked/modified with @state_lock held.
 * @acquire_cnt: number of times @active_func has currently mfpwm_acquire()'d
 *               it. Must only be checked or modified while holding @state_lock.
 * @state_lock: this lock is held while either the active device function, the
 *              enable register, or the chosen clock is being changed.
 * @irq: the IRQ number of this device
 */
struct rockchip_mfpwm {
	struct device *dev;
	void __iomem *base;
	struct clk *pwm_clks[ARRAY_SIZE(pwm_clk_names)];
	struct clk *chosen_clk;
	struct clk *pclk;
	struct rockchip_mfpwm_func *active_func;
	unsigned int acquire_cnt;
	spinlock_t state_lock;
	int irq;
};

static atomic_t subdev_id = ATOMIC_INIT(0);

__attribute__((nonnull))
static int mfpwm_do_acquire(struct rockchip_mfpwm_func *pwmf)
{
	struct rockchip_mfpwm *mfpwm = pwmf->parent;

	/*
	 * If some other function driver already has exclusive access to the
	 * hardware, let the driver trying to do an acquire know.
	 */
	if (mfpwm->active_func && pwmf->id != mfpwm->active_func->id)
		return -EBUSY;

	if (!mfpwm->active_func)
		mfpwm->active_func = pwmf;

	/* Prevent resource leaks in function drivers from turning into more bugs */
	if (WARN_ON(check_add_overflow(mfpwm->acquire_cnt, 1, &mfpwm->acquire_cnt))) {
		mfpwm->acquire_cnt = UINT_MAX;
		return -EOVERFLOW;
	}

	/* Enable the bus clock so hardware registers can be accessed. */
	return clk_enable(mfpwm->pclk);
}

int mfpwm_acquire(struct rockchip_mfpwm_func *pwmf)
{

	if (WARN_ON(IS_ERR_OR_NULL(pwmf)) || WARN_ON(IS_ERR_OR_NULL(pwmf->parent)))
		return -EINVAL;

	/*
	 * Take the state lock and increase the active counter if pwmf is either
	 * already the currently active function driver that has ownership of
	 * the hardware, or if no other function driver is making a claim. If a
	 * different function driver has exclusive ownership currently, return
	 * -EBUSY to the calling driver.
	 */
	guard(spinlock_irqsave)(&pwmf->parent->state_lock);

	return mfpwm_do_acquire(pwmf);
}
EXPORT_SYMBOL_NS_GPL(mfpwm_acquire, "ROCKCHIP_MFPWM");

__attribute__((nonnull))
static void mfpwm_do_release(const struct rockchip_mfpwm_func *pwmf)
{
	struct rockchip_mfpwm *mfpwm = pwmf->parent;

	if (!mfpwm->active_func)
		return;

	if (mfpwm->active_func->id != pwmf->id)
		return;

	/*
	 * No need to check_sub_overflow here, !mfpwm->active_func in
	 * mfpwm_do_acquire() catches what would otherwise be an underflow
	 * of the active_cnt already.
	 */
	mfpwm->acquire_cnt--;

	if (!mfpwm->acquire_cnt)
		mfpwm->active_func = NULL;

	clk_disable(mfpwm->pclk);
}

void mfpwm_release(const struct rockchip_mfpwm_func *pwmf)
{
	if (WARN_ON(IS_ERR_OR_NULL(pwmf)) || WARN_ON(IS_ERR_OR_NULL(pwmf->parent)))
		return;

	/*
	 * Take the state lock and decrease the active counter if pwmf is the
	 * active function driver that has ownership of the hardware right now.
	 */
	guard(spinlock_irqsave)(&pwmf->parent->state_lock);

	mfpwm_do_release(pwmf);
}
EXPORT_SYMBOL_NS_GPL(mfpwm_release, "ROCKCHIP_MFPWM");

int mfpwm_get_mode(const struct rockchip_mfpwm_func *pwmf)
{
	struct rockchip_mfpwm *mfpwm;
	int ret;

	if (WARN_ON(IS_ERR_OR_NULL(pwmf)) || WARN_ON(IS_ERR_OR_NULL(pwmf->parent)))
		return -EINVAL;

	mfpwm = pwmf->parent;

	clk_enable(mfpwm->pclk);

	guard(spinlock_irqsave)(&mfpwm->state_lock);

	if (!rockchip_pwm_v4_is_enabled(mfpwm_reg_read(mfpwm->base, PWMV4_REG_ENABLE))) {
		ret = -1;
		goto out_disable_clk;
	}

	ret = mfpwm_reg_read(mfpwm->base, PWMV4_REG_CTRL) & PWMV4_MODE_MASK;

out_disable_clk:
	clk_disable(mfpwm->pclk);

	return ret;
}
EXPORT_SYMBOL_NS_GPL(mfpwm_get_mode, "ROCKCHIP_MFPWM");

/**
 * mfpwm_register_subdev - register a single mfpwm_func
 * @mfpwm: pointer to the parent &struct rockchip_mfpwm
 * @name: sub-device name string
 *
 * Allocate a single &struct mfpwm_func, fill its members with appropriate data,
 * and register a new mfd cell.
 *
 * Returns: 0 on success, negative errno on error
 */
static int mfpwm_register_subdev(struct rockchip_mfpwm *mfpwm,
				 const char *name)
{
	struct rockchip_mfpwm_func *func;
	struct mfd_cell cell = {};

	func = devm_kzalloc(mfpwm->dev, sizeof(*func), GFP_KERNEL);
	if (IS_ERR(func))
		return PTR_ERR(func);
	func->irq = mfpwm->irq;
	func->parent = mfpwm;
	func->id = atomic_inc_return(&subdev_id);
	func->base = mfpwm->base;
	func->core = mfpwm->chosen_clk;
	cell.name = name;
	cell.platform_data = func;
	cell.pdata_size = sizeof(*func);

	return devm_mfd_add_devices(mfpwm->dev, func->id, &cell, 1, NULL,
				    0, NULL);
}

static int mfpwm_register_subdevs(struct rockchip_mfpwm *mfpwm)
{
	int ret;

	ret = mfpwm_register_subdev(mfpwm, "rockchip-pwm-v4");
	if (ret)
		return ret;

	ret = mfpwm_register_subdev(mfpwm, "rockchip-pwm-capture");
	if (ret)
		return ret;

	return 0;
}

static int rockchip_mfpwm_probe(struct platform_device *pdev)
{
	const char *mux_p_names[ARRAY_SIZE(pwm_clk_names)];
	struct device *dev = &pdev->dev;
	struct rockchip_mfpwm *ddata;
	char *clk_mux_name;
	unsigned int i;
	int ret;

	ddata = devm_kzalloc(&pdev->dev, sizeof(*ddata), GFP_KERNEL);
	if (IS_ERR(ddata))
		return PTR_ERR(ddata);

	ddata->dev = &pdev->dev;

	spin_lock_init(&ddata->state_lock);

	ddata->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ddata->base))
		return PTR_ERR(ddata->base);

	ddata->pclk = devm_clk_get_prepared(dev, "pclk");
	if (IS_ERR(ddata->pclk))
		return dev_err_probe(dev, PTR_ERR(ddata->pclk),
				     "couldn't get and prepare 'pclk' clock\n");

	ddata->irq = platform_get_irq(pdev, 0);
	if (ddata->irq < 0)
		return dev_err_probe(dev, ddata->irq, "couldn't get irq 0\n");

	for (i = 0; i < ARRAY_SIZE(pwm_clk_names); i++) {
		ddata->pwm_clks[i] = devm_clk_get_prepared(dev, pwm_clk_names[i]);
		if (IS_ERR(ddata->pwm_clks[i]))
			return dev_err_probe(dev, PTR_ERR(ddata->pwm_clks[i]),
					     "couldn't get and prepare '%s' clock\n",
					     pwm_clk_names[i]);
		mux_p_names[i] = __clk_get_name(ddata->pwm_clks[i]);
	}

	clk_mux_name = devm_kasprintf(dev, GFP_KERNEL, "%s_chosen", dev_name(dev));
	if (!clk_mux_name)
		return -ENOMEM;

	ddata->chosen_clk = clk_register_mux(dev, clk_mux_name, mux_p_names,
					     ARRAY_SIZE(mux_p_names),
					     CLK_SET_RATE_PARENT,
					     ddata->base + PWMV4_REG_CLK_CTRL,
					     PWMV4_CLK_SRC_SHIFT, PWMV4_CLK_SRC_WIDTH,
					     CLK_MUX_HIWORD_MASK, NULL);
	ret = clk_prepare(ddata->chosen_clk);
	if (ret)
		return dev_err_probe(dev, ret, "failed to prepare PWM clock mux\n");

	platform_set_drvdata(pdev, ddata);

	ret = mfpwm_register_subdevs(ddata);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register sub-devices\n");

	return 0;
}

static void rockchip_mfpwm_remove(struct platform_device *pdev)
{
	struct rockchip_mfpwm *mfpwm = platform_get_drvdata(pdev);

	guard(spinlock_irqsave)(&mfpwm->state_lock);

	if (mfpwm->chosen_clk) {
		clk_unprepare(mfpwm->chosen_clk);
		clk_unregister_mux(mfpwm->chosen_clk);
	}
}

static const struct of_device_id rockchip_mfpwm_of_match[] = {
	{ .compatible = "rockchip,rk3576-pwm" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rockchip_mfpwm_of_match);

static struct platform_driver rockchip_mfpwm_driver = {
	.driver = {
		.name = "rockchip-mfpwm",
		.of_match_table = rockchip_mfpwm_of_match,
	},
	.probe = rockchip_mfpwm_probe,
	.remove = rockchip_mfpwm_remove,
};
module_platform_driver(rockchip_mfpwm_driver);

MODULE_AUTHOR("Nicolas Frattaroli <nicolas.frattaroli@collabora.com>");
MODULE_DESCRIPTION("Rockchip MFPWM Driver");
MODULE_LICENSE("GPL");
