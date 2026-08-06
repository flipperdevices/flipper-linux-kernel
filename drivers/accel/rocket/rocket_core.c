// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2024-2025 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/iommu.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include "rocket_core.h"
#include "rocket_job.h"

int rocket_core_init(struct rocket_core *core)
{
	struct device *dev = core->dev;
	struct platform_device *pdev = to_platform_device(dev);
	u32 version;
	int err = 0;

	/* RK3576 has no per-core hclk reset, so it takes srst_a alone. */
	core->resets[0].id = "srst_a";
	core->resets[1].id = "srst_h";
	err = devm_reset_control_bulk_get_exclusive(&pdev->dev, core->soc->num_resets,
						    core->resets);
	if (err)
		return dev_err_probe(dev, err, "failed to get resets for core %d\n", core->index);

	core->clks[0].id = "aclk";
	core->clks[1].id = "hclk";
	core->clks[2].id = "npu";
	core->clks[3].id = "pclk";
	/* RK3576 clocks the CBUF separately; the compute path stalls without these. */
	core->clks[4].id = "aclk_cbuf";
	core->clks[5].id = "hclk_cbuf";
	err = devm_clk_bulk_get(dev, core->soc->num_clks, core->clks);
	if (err)
		return dev_err_probe(dev, err, "failed to get clocks for core %d\n", core->index);

	core->pc_iomem = devm_platform_ioremap_resource_byname(pdev, "pc");
	if (IS_ERR(core->pc_iomem)) {
		dev_err(dev, "couldn't find PC registers %ld\n", PTR_ERR(core->pc_iomem));
		return PTR_ERR(core->pc_iomem);
	}

	core->cna_iomem = devm_platform_ioremap_resource_byname(pdev, "cna");
	if (IS_ERR(core->cna_iomem)) {
		dev_err(dev, "couldn't find CNA registers %ld\n", PTR_ERR(core->cna_iomem));
		return PTR_ERR(core->cna_iomem);
	}

	core->core_iomem = devm_platform_ioremap_resource_byname(pdev, "core");
	if (IS_ERR(core->core_iomem)) {
		dev_err(dev, "couldn't find CORE registers %ld\n", PTR_ERR(core->core_iomem));
		return PTR_ERR(core->core_iomem);
	}

	dma_set_max_seg_size(dev, UINT_MAX);

	err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(40));
	if (err)
		return err;

	/*
	 * RK3576 spans two power domains, and a multi-domain device is skipped
	 * by the driver-core single-domain auto-attach, so attach the list here.
	 * This goes before the first thing that would have to be unwound, so a
	 * failure can simply return.
	 */
	if (core->soc->multi_power_domain) {
		struct dev_pm_domain_list *pd_list;

		err = devm_pm_domain_attach_list(dev, NULL, &pd_list);
		if (err < 0)
			return dev_err_probe(dev, err,
					     "failed to attach NPU power domains\n");
	}

	core->iommu_group = iommu_group_get(dev);

	err = rocket_job_init(core);
	if (err) {
		iommu_group_put(core->iommu_group);
		core->iommu_group = NULL;
		return err;
	}

	pm_runtime_use_autosuspend(dev);

	/*
	 * As this NPU will be most often used as part of a media pipeline that
	 * ends presenting in a display, choose 50 ms (~3 frames at 60Hz) as an
	 * autosuspend delay as that will keep the device powered up while the
	 * pipeline is running.
	 */
	pm_runtime_set_autosuspend_delay(dev, 50);

	pm_runtime_enable(dev);

	err = pm_runtime_resume_and_get(dev);
	if (err) {
		rocket_core_fini(core);
		return err;
	}

	version = rocket_pc_readl(core, VERSION);
	version += rocket_pc_readl(core, VERSION_NUM) & 0xffff;

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	dev_info(dev, "Rockchip NPU core %d version: %d\n", core->index, version);

	return 0;
}

void rocket_core_fini(struct rocket_core *core)
{
	pm_runtime_dont_use_autosuspend(core->dev);
	pm_runtime_disable(core->dev);
	iommu_group_put(core->iommu_group);
	core->iommu_group = NULL;
	rocket_job_fini(core);
}

void rocket_core_reset(struct rocket_core *core)
{
	reset_control_bulk_assert(core->soc->num_resets, core->resets);

	udelay(10);

	reset_control_bulk_deassert(core->soc->num_resets, core->resets);
}
