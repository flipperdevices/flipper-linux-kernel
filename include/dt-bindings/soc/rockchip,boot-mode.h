/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ROCKCHIP_BOOT_MODE_H
#define __ROCKCHIP_BOOT_MODE_H

/*high 24 bits is tag, low 8 bits is type*/
#define REBOOT_FLAG		0x5242C300
/* normal boot */
#define BOOT_NORMAL		(REBOOT_FLAG + 0)
/* enter bootloader rockusb mode */
#define BOOT_BL_DOWNLOAD	(REBOOT_FLAG + 1)
/* enter recovery */
#define BOOT_RECOVERY		(REBOOT_FLAG + 3)
 /* enter fastboot mode */
#define BOOT_FASTBOOT		(REBOOT_FLAG + 9)
/* enter boot ROM usb download (maskrom) mode */
#define BOOT_BROM_DOWNLOAD	0xEF08A53C

#endif
