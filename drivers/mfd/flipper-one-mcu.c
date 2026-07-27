// SPDX-License-Identifier: GPL-2.0
/*
 * Flipper One MCU interconnect driver
 * Copyright (C) 2026 Flipper FZCO
 */

#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/mfd/core.h>
#include <linux/mfd/flipper-one-mcu.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/reboot.h>
#include <linux/regmap.h>

static const struct regmap_range fomcu_writeable_reg_ranges[] = {
	regmap_reg_range(FOMCU_REG_CPUSTATE, FOMCU_REG_CPUSTATE),
	regmap_reg_range(FOMCU_REG_INTMSK_INPUT,
			 FOMCU_REG_INPUT_BTNS - 1),
	regmap_reg_range(FOMCU_REG_LEDS_BR_LINK,
			 FOMCU_REG_LEDS_COLOR_LINK4),
	regmap_reg_range(FOMCU_REG_HAPTIC, FOMCU_REG_HAPTIC),
	regmap_reg_range(FOMCU_REG_UCSI_CONTROL,
			 FOMCU_REG_UCSI_CONTROL + 6),
	regmap_reg_range(FOMCU_REG_UCSI_MESSAGE_OUT, FOMCU_REG_MAX),
};

static const struct regmap_access_table fomcu_writeable_regs = {
	.yes_ranges = fomcu_writeable_reg_ranges,
	.n_yes_ranges = ARRAY_SIZE(fomcu_writeable_reg_ranges),
};

static const struct regmap_range fomcu_nonvolatile_reg_ranges[] = {
	regmap_reg_range(FOMCU_REG_VERSION, FOMCU_REG_VERSION + 1),
	regmap_reg_range(FOMCU_REG_INTMSK_INPUT, FOMCU_REG_INPUT_BTNS - 1),
};

static const struct regmap_access_table fomcu_volatile_regs = {
	.no_ranges = fomcu_nonvolatile_reg_ranges,
	.n_no_ranges = ARRAY_SIZE(fomcu_nonvolatile_reg_ranges),
};

static const struct regmap_range fomcu_precious_reg_ranges[] = {
	regmap_reg_range(FOMCU_REG_INTSTS_INPUT,
			 FOMCU_REG_INTMSK_INPUT - 1),
};

static const struct regmap_access_table fomcu_precious_regs = {
	.yes_ranges = fomcu_precious_reg_ranges,
	.n_yes_ranges = ARRAY_SIZE(fomcu_precious_reg_ranges),
};

static const struct regmap_config fomcu_regmap_config = {
	.name = "flipper-one-mcu",
	.reg_bits = 16,
	.reg_stride = 2,
	.val_bits = 16,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
	.max_register = FOMCU_REG_MAX,
	.wr_table = &fomcu_writeable_regs,
	.volatile_table = &fomcu_volatile_regs,
	.precious_table = &fomcu_precious_regs,
};

#define CAT(a, b) CAT_I(a, b)
#define CAT_I(a, b) a##b
#define FOMCU_IRQ_REG(subsys, bit) \
	REGMAP_IRQ_REG(CAT(FOMCU_INT_, CAT(subsys, CAT(_, bit))), \
		       CAT(FOMCU_INTOFF_, subsys), \
		       CAT(FOMCU_INTSTS_, CAT(subsys, CAT(_, bit))))

static const struct regmap_irq fomcu_irqs[] = {
	FOMCU_IRQ_REG(INPUT, BTN),
	FOMCU_IRQ_REG(INPUT, TOUCH),
	FOMCU_IRQ_REG(INPUT, HEADSET),
	FOMCU_IRQ_REG(INPUT, SWBTN),
	FOMCU_IRQ_REG(UCSI, EVENT),
};

static unsigned int irq_input_offsets[] = { FOMCU_INTOFF_INPUT };
static unsigned int irq_ucsi_offsets[] = { FOMCU_INTOFF_UCSI };

static const struct regmap_irq_sub_irq_map fomcu_sub_irqs[] = {
	REGMAP_IRQ_MAIN_REG_OFFSET(irq_input_offsets),
	REGMAP_IRQ_MAIN_REG_OFFSET(irq_ucsi_offsets),
};

static const struct regmap_irq_chip fomcu_irq_chip = {
	.name = "fomcu-irq",
	.irqs = fomcu_irqs,
	.num_irqs = ARRAY_SIZE(fomcu_irqs),
	.main_status = FOMCU_REG_INTSTS,
	.status_base = FOMCU_REG_INTSTS_INPUT,
	.mask_base = FOMCU_REG_INTMSK_INPUT,
	.sub_reg_offsets = &fomcu_sub_irqs[0],
	.num_main_regs = 1,
	.num_regs = ARRAY_SIZE(fomcu_sub_irqs),
};

static const struct resource fo_input_irqs[] = {
	DEFINE_RES_IRQ_NAMED(FOMCU_INT_INPUT_BTN, "flipper-one-input-btn"),
	DEFINE_RES_IRQ_NAMED(FOMCU_INT_INPUT_TOUCH, "flipper-one-input-touch"),
	DEFINE_RES_IRQ_NAMED(FOMCU_INT_INPUT_HEADSET, "flipper-one-input-headset"),
	DEFINE_RES_IRQ_NAMED(FOMCU_INT_INPUT_SWBTN, "flipper-one-input-swbtn"),
};

static const struct resource fo_ucsi_irqs[] = {
	DEFINE_RES_IRQ_NAMED(FOMCU_INT_UCSI_EVENT, "flipper-one-ucsi"),
};

static const struct mfd_cell cells[] = {
	MFD_CELL_NAME("flipper-one-haptic"),
	MFD_CELL_RES("flipper-one-input", fo_input_irqs),
	MFD_CELL_NAME("flipper-one-leds"),
	MFD_CELL_NAME("flipper-one-power"),
	MFD_CELL_NAME("flipper-one-regulators"),
	MFD_CELL_NAME("flipper-one-thermal"),
	MFD_CELL_OF("flipper-one-typec", fo_ucsi_irqs, NULL, 0, 0,
		    "flipper,one-typec"),
};

static int fomcu_set_cpustate(struct fomcu_device *ddata,
			      enum fomcu_cpu_states state)
{
	int ret;

	ret = regmap_write(ddata->regmap, FOMCU_REG_CPUSTATE, state);
	if (ret)
		return ret;

	ddata->cpustate = state;
	return 0;
}

static const char * const fomcu_state_names[FOMCU_CPUSTATE_NUM_STATES] = {
	[FOMCU_CPUSTATE_UNKNOWN]	= "unknown",
	[FOMCU_CPUSTATE_BOOTLOADER]	= "bootloader",
	[FOMCU_CPUSTATE_KERNEL_INIT]	= "kernel-init",
	[FOMCU_CPUSTATE_ONLINE]		= "online",
	[FOMCU_CPUSTATE_SUSPEND_REQ]	= "suspend-request",
	[FOMCU_CPUSTATE_SUSPEND]	= "suspend",
	[FOMCU_CPUSTATE_REBOOT_REQ]	= "reboot-request",
	[FOMCU_CPUSTATE_POWEROFF_REQ]	= "poweroff-request",
	[FOMCU_CPUSTATE_SHUTTING_DOWN]	= "shutting-down",
	[FOMCU_CPUSTATE_POWERED_OFF]	= "powered-off",
};

/* States userspace is permitted to report through the cpustate sysfs file. */
static const enum fomcu_cpu_states fomcu_user_writable_states[] = {
	FOMCU_CPUSTATE_ONLINE,
	FOMCU_CPUSTATE_SUSPEND_REQ,
	FOMCU_CPUSTATE_REBOOT_REQ,
	FOMCU_CPUSTATE_POWEROFF_REQ,
};

static ssize_t cpustate_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct fomcu_device *ddata = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", fomcu_state_names[ddata->cpustate]);
}

static ssize_t cpustate_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct fomcu_device *ddata = dev_get_drvdata(dev);
	enum fomcu_cpu_states state;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(fomcu_user_writable_states); i++) {
		state = fomcu_user_writable_states[i];
		if (sysfs_streq(buf, fomcu_state_names[state]))
			break;
	}

	if (i == ARRAY_SIZE(fomcu_user_writable_states))
		return -EINVAL;

	/*
	 * Only ONLINE is a sticky state that resume should restore. The
	 * *-request states are transient announcements of an imminent
	 * transition, so they must not become the resume restore-point
	 * (otherwise a suspend-request/suspend/resume cycle would come back
	 * as "suspend-request" instead of "online").
	 */
	if (state == FOMCU_CPUSTATE_ONLINE)
		ret = fomcu_set_cpustate(ddata, state);
	else
		ret = regmap_write(ddata->regmap, FOMCU_REG_CPUSTATE, state);

	return ret ? : count;
}
static DEVICE_ATTR_RW(cpustate);

static struct attribute *fomcu_attrs[] = {
	&dev_attr_cpustate.attr,
	NULL,
};
ATTRIBUTE_GROUPS(fomcu);

static int fomcu_reboot_notify(struct notifier_block *nb,
			       unsigned long action, void *data)
{
	struct fomcu_device *ddata =
		container_of(nb, struct fomcu_device, reboot_nb);

	/* Runs before device_shutdown() for both reboot and power-off */
	regmap_write(ddata->regmap, FOMCU_REG_CPUSTATE,
		     FOMCU_CPUSTATE_SHUTTING_DOWN);

	return NOTIFY_DONE;
}

static int fomcu_power_off(struct sys_off_data *data)
{
	struct fomcu_device *ddata = data->cb_data;

	/*
	 * Runs after device_shutdown(), just before the machine is actually
	 * powered off. Only reached on power-off, not on reboot, so this is
	 * where we tell the MCU it may cut power to the PMIC.
	 */
	regmap_write(ddata->regmap, FOMCU_REG_CPUSTATE,
		     FOMCU_CPUSTATE_POWERED_OFF);

	return NOTIFY_DONE;
}

static int fomcu_probe(struct i2c_client *client)
{
	struct regmap_irq_chip_data *irq_data;
	struct fomcu_device *ddata;
	int ret;

	ddata = devm_kzalloc(&client->dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	ddata->client = client;

	ddata->regmap = devm_regmap_init_i2c(client, &fomcu_regmap_config);
	if (IS_ERR(ddata->regmap)) {
		return dev_err_probe(&client->dev, PTR_ERR(ddata->regmap),
				     "Failed to allocate register map\n");
	}

	i2c_set_clientdata(client, ddata);

	ret = devm_regmap_add_irq_chip(&client->dev, ddata->regmap,
				       client->irq, IRQF_ONESHOT, 0,
				       &fomcu_irq_chip, &irq_data);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Failed to add IRQ chip\n");

	ret = devm_mfd_add_devices(&client->dev, PLATFORM_DEVID_AUTO,
				   cells, ARRAY_SIZE(cells), NULL, 0,
				   regmap_irq_get_domain(irq_data));
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Failed to register child devices\n");

	ddata->reboot_nb.notifier_call = fomcu_reboot_notify;
	ret = devm_register_reboot_notifier(&client->dev, &ddata->reboot_nb);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Failed to register reboot notifier\n");

	ret = devm_register_sys_off_handler(&client->dev,
					    SYS_OFF_MODE_POWER_OFF_PREPARE,
					    SYS_OFF_PRIO_DEFAULT,
					    fomcu_power_off, ddata);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Failed to register power-off handler\n");

	/*
	 * Signal that early kernel init has reached this driver. Userspace is
	 * expected to move the MCU to FOMCU_CPUSTATE_ONLINE once boot completes.
	 */
	return fomcu_set_cpustate(ddata, FOMCU_CPUSTATE_KERNEL_INIT);
}

static int fomcu_suspend(struct device *dev)
{
	struct fomcu_device *ddata = dev_get_drvdata(dev);

	/*
	 * Leave ddata->cpustate untouched so that resume can restore whatever
	 * state we were in before suspending (KERNEL_INIT if userspace had not
	 * come up yet, ONLINE otherwise).
	 */
	return regmap_write(ddata->regmap, FOMCU_REG_CPUSTATE, FOMCU_CPUSTATE_SUSPEND);
}

static int fomcu_resume(struct device *dev)
{
	struct fomcu_device *ddata = dev_get_drvdata(dev);

	return regmap_write(ddata->regmap, FOMCU_REG_CPUSTATE, ddata->cpustate);
}

static DEFINE_SIMPLE_DEV_PM_OPS(fomcu_pm_ops, fomcu_suspend, fomcu_resume);

static const struct i2c_device_id fomcu_i2c_ids[] = {
	{ "flipper-one-mcu" },
	{}
};
MODULE_DEVICE_TABLE(i2c, fomcu_i2c_ids);

static const struct of_device_id fomcu_of_match[] = {
	{ .compatible = "flipper,one-mcu" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, fomcu_of_match);

static struct i2c_driver fomcu_driver = {
	.driver = {
		.name = "flipper-one-mcu",
		.of_match_table = fomcu_of_match,
		.pm = pm_sleep_ptr(&fomcu_pm_ops),
		.dev_groups = fomcu_groups,
	},
	.probe = fomcu_probe,
	.id_table = fomcu_i2c_ids,
};
module_i2c_driver(fomcu_driver);

MODULE_DESCRIPTION("Flipper One MCU driver");
MODULE_AUTHOR("Alexey Charkov <alchark@flipper.net>");
MODULE_LICENSE("GPL");
