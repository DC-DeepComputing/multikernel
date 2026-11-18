/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ARCH_X86_MULTIKERNEL_IOMMU_H
#define _ARCH_X86_MULTIKERNEL_IOMMU_H

#include <linux/types.h>

struct mk_instance;

#define MK_IOMMUS_PATH		"/chosen/multikernel,iommus"

#ifdef CONFIG_DMAR_TABLE
int mk_dmar_emit_units(void *fdt, struct mk_instance *instance);
void mk_dmar_msg(u32 subtype, void *payload, u32 payload_len);
#else
static inline int mk_dmar_emit_units(void *fdt, struct mk_instance *instance)
{
	return 0;
}
static inline void mk_dmar_msg(u32 subtype, void *payload, u32 payload_len)
{
}
#endif

#ifdef CONFIG_MULTIKERNEL_AMD_VI_PROXY
int mk_amd_vi_emit_units(void *fdt);
void mk_amd_vi_msg(struct mk_instance *from, u32 subtype, void *payload,
		   u32 payload_len);
#else
static inline int mk_amd_vi_emit_units(void *fdt)
{
	return 0;
}
static inline void mk_amd_vi_msg(struct mk_instance *from, u32 subtype,
				 void *payload, u32 payload_len)
{
}
#endif

#endif /* _ARCH_X86_MULTIKERNEL_IOMMU_H */
