// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: 2026 Flipper FZCO
/*
 * NVMEM layout for the CPU ID in Rockchip OTP memory
 */

#include <crypto/sha2.h>
#include <linux/device-id/of.h>
#include <linux/etherdevice.h>
#include <linux/hex.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <uapi/linux/if_ether.h>

#define ROCKCHIP_CPUID_LEN	16

struct rockchip_cpuid_data {
	unsigned int offset;
};

static int rockchip_cpuid_mac_pp(void *priv, const char *id, int index,
				 unsigned int offset, void *buf, size_t bytes)
{
	char cpuid[ROCKCHIP_CPUID_LEN * 2];
	u8 digest[SHA256_DIGEST_SIZE];
	u8 *mac = buf;

	if (bytes != ROCKCHIP_CPUID_LEN)
		return -EINVAL;

	if (index < 0 || index > 1)
		return -EINVAL;

	/* The buffer still holds the raw CPU ID at this point */
	bin2hex(cpuid, mac, ROCKCHIP_CPUID_LEN);

	sha256(cpuid, sizeof(cpuid), digest);

	memcpy(mac, digest, ETH_ALEN);
	mac[0] &= 0xfe;		/* clear the multicast bit */
	mac[0] |= 0x02;		/* set the locally administered bit */
	mac[5] ^= index;

	if (!is_valid_ether_addr(mac))
		return -EINVAL;

	return 0;
}

static int rockchip_cpuid_add_cells(struct nvmem_layout *layout)
{
	const struct rockchip_cpuid_data *data;
	struct nvmem_cell_info info = {0};
	struct device_node *layout_np;
	int ret;

	data = of_device_get_match_data(&layout->dev);
	if (!data)
		return -EINVAL;

	layout_np = of_nvmem_layout_get_container(layout->nvmem);
	if (!layout_np)
		return -ENOENT;

	info.name = "mac-address";
	info.offset = data->offset;
	info.raw_len = ROCKCHIP_CPUID_LEN;
	info.bytes = ETH_ALEN;
	info.read_post_process = rockchip_cpuid_mac_pp;
	info.np = of_get_child_by_name(layout_np, info.name);

	of_node_put(layout_np);

	ret = nvmem_add_one_cell(layout->nvmem, &info);
	if (ret)
		of_node_put(info.np);

	return ret;
}

static int rockchip_cpuid_probe(struct nvmem_layout *layout)
{
	layout->add_cells = rockchip_cpuid_add_cells;

	return nvmem_layout_register(layout);
}

static void rockchip_cpuid_remove(struct nvmem_layout *layout)
{
	nvmem_layout_unregister(layout);
}

static const struct rockchip_cpuid_data rk3576_cpuid_data = {
	.offset = 0x0a,
};

static const struct of_device_id rockchip_cpuid_of_match_table[] = {
	{
		.compatible = "rockchip,rk3576-otp-cpuid",
		.data = &rk3576_cpuid_data,
	},
	{},
};
MODULE_DEVICE_TABLE(of, rockchip_cpuid_of_match_table);

static struct nvmem_layout_driver rockchip_cpuid_layout = {
	.driver = {
		.name = "rockchip-otp-cpuid-layout",
		.of_match_table = rockchip_cpuid_of_match_table,
	},
	.probe = rockchip_cpuid_probe,
	.remove = rockchip_cpuid_remove,
};
module_nvmem_layout_driver(rockchip_cpuid_layout);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alexey Charkov <alchark@flipper.net>");
MODULE_DESCRIPTION("NVMEM layout driver for the CPU ID in Rockchip OTP memory");
