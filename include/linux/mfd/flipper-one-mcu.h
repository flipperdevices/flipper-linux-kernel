/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Register definitions for the Flipper One MCU interconnect
 * Copyright (C) 2026 Flipper FZCO
 */

#ifndef __LINUX_MFD_FLIPPER_ONE_MCU_H
#define __LINUX_MFD_FLIPPER_ONE_MCU_H

#include <linux/i2c.h>
#include <linux/notifier.h>
#include <linux/regmap.h>

enum fomcu_interrupts {
	FOMCU_INT_INPUT_BTN,
	FOMCU_INT_INPUT_TOUCH,
	FOMCU_INT_INPUT_HEADSET,
	FOMCU_INT_INPUT_SWBTN,
	FOMCU_INT_UCSI_EVENT,
};

#define FOMCU_REG_INTSTS		0x0000

#define FOMCU_REG_CPUSTATE		0x0040

/*
 * These values are part of the wire protocol shared with the MCU firmware;
 * keep them stable and in sync with the MCU side when adding new states.
 */
enum fomcu_cpu_states {
	FOMCU_CPUSTATE_UNKNOWN = 0,
	FOMCU_CPUSTATE_BOOTLOADER,	/* set by the MCU/bootloader itself */
	FOMCU_CPUSTATE_KERNEL_INIT,	/* this driver has probed */
	FOMCU_CPUSTATE_ONLINE,		/* userspace up / resumed from suspend */
	FOMCU_CPUSTATE_SUSPEND_REQ,	/* userspace preparing to suspend */
	FOMCU_CPUSTATE_SUSPEND,		/* kernel about to suspend */
	FOMCU_CPUSTATE_REBOOT_REQ,	/* userspace preparing to reboot */
	FOMCU_CPUSTATE_POWEROFF_REQ,	/* userspace preparing to power off */
	FOMCU_CPUSTATE_SHUTTING_DOWN,	/* kernel reboot/power-off in progress */
	FOMCU_CPUSTATE_POWERED_OFF,	/* safe to cut power to the PMIC */
	FOMCU_CPUSTATE_NUM_STATES
};

#define FOMCU_REG_VERSION		0x0080

#define FOMCU_REG_INTSTS_INPUT		0x0100
#define FOMCU_INTOFF_INPUT		0x0
#define FOMCU_INTSTS_INPUT_BTN		BIT(0)
#define FOMCU_INTSTS_INPUT_TOUCH	BIT(1)
#define FOMCU_INTSTS_INPUT_HEADSET	BIT(2)
#define FOMCU_INTSTS_INPUT_SWBTN	BIT(3)

#define FOMCU_REG_INTSTS_UCSI		0x0102
#define FOMCU_INTOFF_UCSI		0x2
#define FOMCU_INTSTS_UCSI_EVENT		BIT(0)

#define FOMCU_REG_INTMSK_INPUT		0x0180
#define FOMCU_REG_INTMSK_UCSI		0x0182

#define FOMCU_REG_INPUT_BTNS		0x0200
#define FOMCU_REG_INPUT_TOUCH_X		0x0202
#define FOMCU_REG_INPUT_TOUCH_Y		0x0204
#define FOMCU_REG_INPUT_TOUCH_Z		0x0206
#define FOMCU_REG_INPUT_HEADSET		0x0208
#define FOMCU_REG_INPUT_SWBTNS		0x020a

#define FOMCU_REG_LEDS_BR_LINK		0x0300
#define FOMCU_REG_LEDS_BR_POWER		0x0302
#define FOMCU_REG_LEDS_BR_WATT		0x0304

/* RGB565 values per each LED */
#define FOMCU_REG_LEDS_COLOR_LINK1	0x0310
#define FOMCU_REG_LEDS_COLOR_LINK2	0x0312
#define FOMCU_REG_LEDS_COLOR_LINK3	0x0314
#define FOMCU_REG_LEDS_COLOR_LINK4	0x0316

#define FOMCU_REG_HAPTIC		0x0400
#define FOMCU_HAPTIC_PLAY		BIT(15)
#define FOMCU_HAPTIC_EFFECT		GENMASK(14, 8)
#define FOMCU_HAPTIC_DURATION		GENMASK(7, 0)

#define FOMCU_REG_UCSI			0x0500
#define FOMCU_REG_UCSI_VERSION		(FOMCU_REG_UCSI + 0x00)
#define FOMCU_REG_UCSI_CCI		(FOMCU_REG_UCSI + 0x04)
#define FOMCU_REG_UCSI_CONTROL		(FOMCU_REG_UCSI + 0x08)
#define FOMCU_REG_UCSI_MESSAGE_IN	0x0510
#define FOMCU_REG_UCSI_MESSAGE_OUT	0x0610
#define FOMCU_UCSI_MESSAGE_LEN		256

#define FOMCU_REG_MAX			(FOMCU_REG_UCSI_MESSAGE_OUT + \
					 FOMCU_UCSI_MESSAGE_LEN - 1)

struct fomcu_device {
	struct i2c_client *client;
	struct regmap *regmap;
	struct notifier_block reboot_nb;
	/* Last non-transient CPU state, restored on resume. */
	enum fomcu_cpu_states cpustate;
};

#endif /* __LINUX_MFD_FLIPPER_ONE_MCU_H */
