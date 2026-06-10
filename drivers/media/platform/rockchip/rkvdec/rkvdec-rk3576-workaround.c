// SPDX-License-Identifier: GPL-2.0-only
/*
 * Rockchip RK3576 (VDPU383) power-up priming workaround.
 *
 * The VDPU383 decoder on the RK3576 comes out of power-up with its internal
 * deblocking-context state uninitialised. Without priming, H.264 decode
 * intermittently produces wrong pixels at the horizontal deblock edges (luma
 * rows 4, 12 and 13 within each 16-row macroblock row), propagating through
 * subsequent P-frames. The failure is non-deterministic and its rate varies
 * between boards.
 *
 * The Rockchip BSP avoids this by running a one-shot priming decode of a small
 * canned bitstream at every decoder power-up (rk3576_workaround_init /
 * rk3576_workaround_run). This ports that sequence: the priming buffer is
 * allocated once at probe and the priming decode is run on every pm_runtime
 * resume (which catches every power-up). It is best-effort - on failure a
 * warning is logged and decoding continues (degraded for H.264).
 *
 * The descriptor layout and the link-bank register kick sequence are
 * transcribed from the BSP rk3576_workaround_{init,run}; the 64-byte header is
 * an opaque IP-init blob taken verbatim from the BSP.
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/types.h>

#include "rkvdec.h"

/* Opaque 64-byte IP-init header blob, verbatim from the BSP .rodata. */
static const u8 rkvdec_rk3576_warmup_hdr[64] = {
	0x00, 0x00, 0x01, 0x65, 0x88, 0x82, 0x0b, 0x01,
	0x2f, 0x08, 0xc5, 0x00, 0x01, 0x51, 0x78, 0xe0,
	0x00, 0x24, 0xf7, 0x1c, 0x00, 0x04, 0xcc, 0xeb,
	0x89, 0xd7, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x40, 0x26, 0x00, 0x10, 0x04, 0x08, 0x00, 0x08,
	0x80, 0x01, 0x00, 0x00, 0x00, 0x40, 0x01, 0xd8,
	0x07, 0x7c, 0x7a, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* "rk" recognition markers the IP looks for in the priming descriptor. */
#define RK3576_WARMUP_SENTINEL_HI	0x76543210u
#define RK3576_WARMUP_SENTINEL_HI_PLUS2	0x76543212u
/* BSP allocates 8 KiB; the descriptor lives at +0x1000. */
#define RK3576_WARMUP_BUF_SIZE		0x2000u

/* Lay out the priming descriptor in the (zeroed) DMA buffer. Offsets and
 * constants are transcribed verbatim from the BSP rk3576_workaround_init.
 */
static void rkvdec_rk3576_warmup_populate(void *buf, dma_addr_t iova)
{
	u8 *b8 = buf;
	u32 *b32 = buf;
	u32 iova_lo = lower_32_bits(iova);

	memcpy(&b8[0],  &rkvdec_rk3576_warmup_hdr[0],  32);
	memcpy(&b8[64], &rkvdec_rk3576_warmup_hdr[32], 16);
	memcpy(&b8[80], &rkvdec_rk3576_warmup_hdr[48],  8);
	memcpy(&b8[88], &rkvdec_rk3576_warmup_hdr[56],  4);

	b32[4128 / 4] = 0x00000001u;
	b32[4148 / 4] = 0x0000ffffu;
	b32[4160 / 4] = 0x00000101u;
	b32[4176 / 4] = 0xffffffffu;
	b32[4180 / 4] = 0x3ff3ffffu;

	b32[4352 / 4] = RK3576_WARMUP_SENTINEL_HI;
	b32[4356 / 4] = 0x00000000u;
	b32[4360 / 4] = 0x00000020u;
	b32[4364 / 4] = 0x000000a8u;
	b32[4368 / 4] = 0x00000002u;
	b32[4372 / 4] = 0x00000002u;
	b32[4376 / 4] = 0x00000040u;

	b32[4608 / 4] = iova_lo;
	b32[4612 / 4] = iova_lo + 0x140u;
	b32[4620 / 4] = iova_lo + 0x040u;
	b32[4656 / 4] = iova_lo + 0x240u;
	b32[4660 / 4] = 0x000000c0u;
	b32[4688 / 4] = iova_lo + 0x340u;
	b32[4692 / 4] = 0x00000200u;
	b32[4768 / 4] = iova_lo + 0x540u;
	b32[4960 / 4] = iova_lo + 0xb40u;
	b32[4100 / 4] = RK3576_WARMUP_SENTINEL_HI_PLUS2;
	b32[4096 / 4] = iova_lo + 0x540u;
	b32[4104 / 4] = iova_lo + 0x1400u;
	b32[4108 / 4] = iova_lo + 0x1020u;
	b32[4112 / 4] = iova_lo + 0x1100u;
	b32[4116 / 4] = iova_lo + 0x1200u;

	wmb(); /* descriptor must reach RAM before the HW reads it */
}

/*
 * Allocate + populate the priming buffer (once, at probe). Device-managed, so
 * it is freed automatically on device teardown. Returns 0 or negative errno.
 */
int rkvdec_rk3576_warmup_alloc(struct device *dev, void **out_cpu,
			       dma_addr_t *out_dma)
{
	void *cpu;
	dma_addr_t dma;

	cpu = dmam_alloc_coherent(dev, RK3576_WARMUP_BUF_SIZE, &dma, GFP_KERNEL);
	if (!cpu)
		return -ENOMEM;

	rkvdec_rk3576_warmup_populate(cpu, dma);
	*out_cpu = cpu;
	*out_dma = dma;
	return 0;
}

/*
 * Run the priming decode through the link bank. Clocks and the power domain
 * must be on (true in pm_runtime resume). The priming decode raises no
 * completion IRQ - only the status register - so it is polled (~20 ms budget).
 * Returns 0 on clean completion, -EIO on HW error status, -ETIMEDOUT on no
 * completion.
 */
int rkvdec_rk3576_warmup_run(void __iomem *link_base, dma_addr_t buf_iova)
{
	u32 status = 0;
	int i;

	/* Kick sequence, verbatim from the BSP rk3576_workaround_run. */
	writel(0x8000u, link_base + 0x58);	/* ip_en: BIT(15) only */
	writel(0x7ffffu, link_base + 0x54);	/* ip watchdog */
	writel(0x10001u, link_base + 0x00);	/* ccu/init */
	writel(lower_32_bits(buf_iova) + 0x1000u,
	       link_base + 0x04);		/* cfg_addr -> descriptor */
	writel(0x1u, link_base + 0x08);		/* link_mode = 1 */
	writel(0x1u, link_base + 0x18);		/* link_en = 1 */
	wmb(); /* commit config before cfg_done kicks the HW */
	writel(0x1u, link_base + 0x0c);		/* cfg_done */

	for (i = 0; i < 200; i++) {
		usleep_range(100, 150);
		status = readl(link_base + 0x4c);
		if (status)
			break;
	}

	/* Teardown: clear irq + status, zero the config registers. */
	writel(0xffff0000u, link_base + 0x48);
	writel(0xffff0000u, link_base + 0x4c);
	writel(0x0u, link_base + 0x00);
	writel(0x0u, link_base + 0x08);
	writel(0x0u, link_base + 0x18);
	wmb();
	writel(0x0u, link_base + 0x58);

	if (i >= 200)
		return -ETIMEDOUT;
	if (status & 0x3fe)	/* err_mask */
		return -EIO;
	return 0;
}
