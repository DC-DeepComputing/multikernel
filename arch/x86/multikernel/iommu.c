// SPDX-License-Identifier: GPL-2.0-only
/*
 * The host's IOMMUs, as a spawn sees them:
 *
 *   /chosen/multikernel,iommus {
 *       #address-cells = <2>; #size-cells = <2>;
 *       iommu@fed90000 { ... };
 *   };
 *
 * and each PCI function node names its unit in multikernel,iommu-base.
 *
 * What a unit's node says, and what a spawn asks the host for the interrupts
 * of a function behind it, is the vendor's: VT-d in dmar.c, AMD-Vi in
 * amd_vi.c. A machine has units of one vendor only.
 */

#include <linux/amd-iommu.h>
#include <linux/dmar.h>
#include <linux/init.h>
#include <linux/libfdt.h>
#include <linux/multikernel.h>
#include <linux/pci.h>

#include "iommu.h"

int mk_arch_manifest_chosen(void *fdt, struct mk_instance *instance)
{
	struct amd_iommu_unit_desc amd;
	struct dmar_unit_desc vtd;
	int ret;

	if (dmar_unit_get(0, &vtd) && amd_iommu_unit_get(0, &amd))
		return 0;
	ret = fdt_begin_node(fdt, "multikernel,iommus");
	if (!ret)
		ret = fdt_property_u32(fdt, "#address-cells", 2);
	if (!ret)
		ret = fdt_property_u32(fdt, "#size-cells", 2);
	if (!ret)
		ret = mk_dmar_emit_units(fdt, instance);
	if (!ret)
		ret = mk_amd_vi_emit_units(fdt);
	return ret ? ret : fdt_end_node(fdt);
}

u64 mk_arch_pci_iommu(struct pci_dev *pdev)
{
	u64 base;

	if (dmar_device_unit(pdev, &base) && amd_iommu_device_unit(pdev, &base))
		return 0;
	return base;
}

static void mk_iommu_msg_handler(struct mk_instance *from, u32 msg_type, u32 subtype,
				 void *payload, u32 payload_len, void *ctx)
{
	switch (subtype) {
	case MK_IO_IRTE_WRITE:
	case MK_IO_IRTE_ACK:
		mk_dmar_msg(subtype, payload, payload_len);
		break;
	case MK_IO_MSI_ALLOC:
	case MK_IO_MSI_FREE:
	case MK_IO_MSI_MAP:
	case MK_IO_MSI_UNMAP:
	case MK_IO_MSI_ACK:
		mk_amd_vi_msg(from, subtype, payload, payload_len);
		break;
	}
}

static int __init mk_iommu_init(void)
{
	return mk_register_msg_handler(MK_MSG_IO, mk_iommu_msg_handler, NULL);
}
fs_initcall(mk_iommu_init);
