# SPDX-License-Identifier: GPL-2.0-only

obj-$(CONFIG_AIC8800D80_USB) += aic8800_fdrv.o

aic8800_fdrv-y := \
	rwnx_wakelock.o \
	rwnx_msg_tx.o \
	rwnx_msg_rx.o \
	rwnx_utils.o \
	rwnx_cmds.o \
	rwnx_cfgfile.o \
	rwnx_strs.o \
	rwnx_rx.o \
	rwnx_tx.o \
	rwnx_txq.o \
	rwnx_main.o \
	rwnx_mod_params.o \
	rwnx_mesh.o \
	rwnx_platform.o \
	ipc_host.o \
	rwnx_tdls.o \
	regdb.o \
	md5.o \
	aicwf_compat_8800d80.o \
	rwnx_radar.o \
	aicwf_tcp_ack.o \
	usb_host.o \
	aicwf_txrxif.o \
	aicwf_usb.o

ccflags-y += \
	-Wno-implicit-fallthrough \
	-Wno-unused-variable \
	-I$(src) \
	-DCONFIG_RWNX_P2P_DEBUGFS \
	-DNX_VIRT_DEV_MAX=4 \
	-DNX_REMOTE_STA_MAX_FOR_OLD_IC=8 \
	-DNX_REMOTE_STA_MAX=32 \
	-DNX_MU_GROUP_MAX=62 \
	-DNX_TXDESC_CNT=64 \
	-DNX_TX_MAX_RATES=4 \
	-DNX_CHAN_CTXT_CNT=3 \
	-DCONFIG_START_FROM_BOOTROM \
	-DCONFIG_VRF_DCDC_MODE \
	-DCONFIG_ROM_PATCH_EN \
	-DCONFIG_COEX \
	-DCONFIG_RWNX_FULLMAC \
	-DCONFIG_RWNX_RADAR \
	-DCONFIG_RWNX_DBG \
	-DCONFIG_RFTEST \
	-DCONFIG_MCC \
	-DAICWF_USB_SUPPORT \
	-DCONFIG_USER_MAX=1 \
	-DNX_TXQ_CNT=5 \
	-DAICWF_RX_REORDER \
	-DAICWF_ARP_OFFLOAD \
	-DUSE_5G \
	-DCONFIG_USB_BT \
	-DCONFIG_ALIGN_8BYTES \
	-DCONFIG_TXRX_THREAD_PRIO \
	-DCONFIG_USB_ALIGN_DATA \
	-DCONFIG_MAC_RANDOM_IF_NO_MAC_IN_EFUSE \
	-DDEFAULT_COUNTRY_CODE=\"00\" \
	-DCONFIG_RX_NETIF_RECV_SKB \
	-DCONFIG_USB_MSG_OUT_EP \
	-DCONFIG_USB_MSG_IN_EP \
	-DCONFIG_USE_USB_ZERO_PACKET \
	-DCONFIG_STA_SCAN_WHEN_P2P_WORKING \
	-DCONFIG_SUPPORT_REALTIME_CHANGE_MAC \
	-DCONFIG_PREALLOC_RX_SKB \
	-DCONFIG_PREALLOC_TXQ \
	-DCONFIG_DPD \
	-DCONFIG_FORCE_DPD_CALIB \
	-DCONFIG_FILTER_TCP_ACK
