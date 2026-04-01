/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Register definitions for the Flipper One MCU interconnect
 * Copyright (C) 2026 Flipper FZCO
 */

#ifndef __LINUX_MFD_FLIPPER_ONE_MCU_H
#define __LINUX_MFD_FLIPPER_ONE_MCU_H

#include <linux/i2c.h>
#include <linux/regmap.h>

enum fomcu_interrupts {
	FOMCU_INT_INPUT_BTN,
	FOMCU_INT_INPUT_TOUCH,
};

#define FOMCU_REG_INTSTS		0x0000

#define FOMCU_REG_VERSION		0x0080

#define FOMCU_REG_INTSTS_INPUT		0x0100
#define FOMCU_INTOFF_INPUT		0x0
#define FOMCU_INTSTS_INPUT_BTN		BIT(0)
#define FOMCU_INTSTS_INPUT_TOUCH	BIT(1)

#define FOMCU_REG_INTMSK_INPUT		0x0180

#define FOMCU_REG_INPUT_BTNS		0x0200
#define FOMCU_REG_INPUT_TOUCH_X		0x0202
#define FOMCU_REG_INPUT_TOUCH_Y		0x0204
#define FOMCU_REG_INPUT_TOUCH_Z		0x0206

#define FOMCU_REG_MAX			(FOMCU_REG_INPUT_TOUCH_Z + 1)

struct fomcu_device {
	struct i2c_client *client;
	struct regmap *regmap;
};

#endif /* __LINUX_MFD_FLIPPER_ONE_MCU_H */
