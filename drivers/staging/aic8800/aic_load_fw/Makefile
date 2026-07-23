# SPDX-License-Identifier: GPL-2.0

ccflags-y += -DAICWF_USB_SUPPORT
ccflags-y += -DCONFIG_RFTEST
ccflags-y += -DCONFIG_USB_BT
ccflags-y += -DCONFIG_USB_MSG_EP
ccflags-y += -DCONFIG_LINK_DET_5G
ccflags-y += -DCONFIG_PREALLOC_RX_SKB
ccflags-y += -DCONFIG_PREALLOC_TXQ
ccflags-y += -DCONFIG_PLATFORM_UBUNTU

obj-$(CONFIG_AIC8800D80_USB) += aic_load_fw.o

aic_load_fw-y := \
	aic_bluetooth_main.o \
	aicbluetooth.o \
	aicwf_usb.o \
	aic_txrxif.o \
	aicbluetooth_cmds.o \
	aic_compat_8800d80.o \
	md5.o \
	aicwf_rx_prealloc.o \
	aicwf_txq_prealloc.o
