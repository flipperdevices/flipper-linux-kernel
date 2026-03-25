/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/* SPDX-FileCopyrightText: 2026 Flipper FZCO */
/*
 * Interrupts of the Flipper One MCU
 */

#ifndef _DT_BINDINGS_MFD_FLIPPER_ONE_MCU_H
#define _DT_BINDINGS_MFD_FLIPPER_ONE_MCU_H

/* Device tree ABI: append new interrupts, never renumber */
#define FOMCU_INT_INPUT_BTN		0
#define FOMCU_INT_INPUT_TOUCH		1
#define FOMCU_INT_INPUT_HEADSET		2
#define FOMCU_INT_INPUT_SWBTN		3
#define FOMCU_INT_UCSI_EVENT		4
#define FOMCU_INT_SYSTEM_RESET		5
#define FOMCU_INT_SYSTEM_FRAME		6

#endif /* _DT_BINDINGS_MFD_FLIPPER_ONE_MCU_H */
