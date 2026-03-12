/*
 * Copyright (c) 2015 NVIDIA Corporation. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef DRM_SCDC_H
#define DRM_SCDC_H

#include <linux/bits.h>

#define SCDC_SINK_VERSION			0x01

#define SCDC_SOURCE_VERSION			0x02

#define SCDC_UPDATE_0				0x10
#define  SCDC_FLT_UPDATE			BIT(5)
#define  SCDC_FRL_START				BIT(4)
#define  SCDC_SOURCE_TEST_UPDATE		BIT(3)
#define  SCDC_READ_REQUEST_TEST			BIT(2)
#define  SCDC_CED_UPDATE			BIT(1)
#define  SCDC_STATUS_UPDATE			BIT(0)

#define SCDC_UPDATE_1				0x11

#define SCDC_TMDS_CONFIG			0x20
#define  SCDC_TMDS_BIT_CLOCK_RATIO_BY_40	BIT(1)
#define  SCDC_SCRAMBLING_ENABLE			BIT(0)

#define SCDC_SCRAMBLER_STATUS			0x21
#define  SCDC_SCRAMBLING_STATUS			BIT(0)

#define SCDC_CONFIG_0				0x30
#define  SCDC_FLT_NO_RETRAIN			BIT(1)
#define  SCDC_READ_REQUEST_ENABLE		BIT(0)

#define SCDC_CONFIG_1				0x31
#define  SCDC_FFE_LEVELS_MASK			GENMASK(7, 4)
#define  SCDC_FRL_RATE_MASK			GENMASK(3, 0)
#define  SCDC_FRL_RATE_DISABLE			0
#define  SCDC_FRL_RATE_3GBPS_3LANE		1
#define  SCDC_FRL_RATE_6GBPS_3LANE		2
#define  SCDC_FRL_RATE_6GBPS_4LANE		3
#define  SCDC_FRL_RATE_8GBPS_4LANE		4
#define  SCDC_FRL_RATE_10GBPS_4LANE		5
#define  SCDC_FRL_RATE_12GBPS_4LANE		6

#define SCDC_SOURCE_TEST_CONFIG			0x35
#define  SCDC_FLT_NO_TIMEOUT			BIT(5)

#define SCDC_STATUS_FLAGS_0			0x40
#define  SCDC_FLT_READY				BIT(6)
#define  SCDC_CH2_LOCK				BIT(3)
#define  SCDC_CH1_LOCK				BIT(2)
#define  SCDC_CH0_LOCK				BIT(1)
#define  SCDC_CH_LOCK_MASK			(SCDC_CH2_LOCK | SCDC_CH1_LOCK | SCDC_CH0_LOCK)
#define  SCDC_CLOCK_DETECT			BIT(0)

#define SCDC_STATUS_FLAGS_1			0x41
#define  SCDC_FRL_LN1_LTP_REQ_MASK		GENMASK(7, 4)
#define  SCDC_FRL_LN0_LTP_REQ_MASK		GENMASK(3, 0)

#define SCDC_STATUS_FLAGS_2			0x42
#define  SCDC_FRL_LN3_LTP_REQ_MASK		GENMASK(7, 4)
#define  SCDC_FRL_LN2_LTP_REQ_MASK		GENMASK(3, 0)

#define SCDC_ERR_DET_0_L			0x50
#define SCDC_ERR_DET_0_H			0x51
#define SCDC_ERR_DET_1_L			0x52
#define SCDC_ERR_DET_1_H			0x53
#define SCDC_ERR_DET_2_L			0x54
#define SCDC_ERR_DET_2_H			0x55
#define  SCDC_CHANNEL_VALID			BIT(7)

#define SCDC_ERR_DET_CHECKSUM			0x56

#define SCDC_TEST_CONFIG_0			0xc0
#define  SCDC_TEST_READ_REQUEST			BIT(7)
#define  SCDC_TEST_READ_REQUEST_DELAY(x)	((x) & 0x7f)

#define SCDC_MANUFACTURER_IEEE_OUI		0xd0
#define SCDC_MANUFACTURER_IEEE_OUI_SIZE		3

#define SCDC_DEVICE_ID				0xd3
#define SCDC_DEVICE_ID_SIZE			8

#define SCDC_DEVICE_HARDWARE_REVISION		0xdb
#define  SCDC_GET_DEVICE_HARDWARE_REVISION_MAJOR(x) (((x) >> 4) & 0xf)
#define  SCDC_GET_DEVICE_HARDWARE_REVISION_MINOR(x) (((x) >> 0) & 0xf)

#define SCDC_DEVICE_SOFTWARE_MAJOR_REVISION	0xdc
#define SCDC_DEVICE_SOFTWARE_MINOR_REVISION	0xdd

#define SCDC_MANUFACTURER_SPECIFIC		0xde
#define SCDC_MANUFACTURER_SPECIFIC_SIZE		34

#endif
