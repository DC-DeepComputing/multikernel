// SPDX-License-Identifier: GPL-2.0-only
/*
 * MSIs for an instance's PCI devices, through the host's AMD IOMMUs.
 *
 * An AMD IOMMU that remaps interrupts takes the data of an MSI as an index
 * into the remapping table of the device that sent it, so a message
 * composed for the local APIC is not delivered. The tables belong to the
 * host, the one kernel that drives the IOMMU, and an instance is not
 * trusted with where interrupts go. So it asks over the message ring:
 *
 *  - MK_IO_MSI_ALLOC sets entries of a function's table aside. The answer
 *    names the first one, which is the data the function writes, and the
 *    address it writes it to.
 *  - MK_IO_MSI_MAP delivers an entry to a vector of one of the instance's
 *    CPUs, when the interrupt starts and each time it moves.
 *  - MK_IO_MSI_UNMAP stops an entry, and MK_IO_MSI_FREE gives entries back.
 *
 * Every answer is MK_IO_MSI_ACK. MAP and UNMAP come from interrupt chip
 * callbacks with interrupts off, so the instance polls. The host checks
 * each request against the lease, a mutex, so it answers from a work item.
 *
 * The entries allocated for a function are part of the instance's lease on
 * it. When the lease ends they are freed, and the function interrupts
 * nobody until it is lent again.
 *
 * The host describes each IOMMU under /chosen/multikernel,iommus (iommu.c):
 *
 *   iommu@fedc0000 {
 *       compatible = "multikernel,amd-vi";
 *       reg = <base size>;
 *       linux,pci-domain = <0>;
 *       multikernel,irq-remap;		if it remaps
 *   };
 *
 * Both ends live here. The host's side of the tables is amd_iommu_irte_*()
 * in the AMD IOMMU driver, which an instance never starts: it has no IVRS.
 * The interrupt domain of the instance is amd-ir-mk.
 */

#define pr_fmt(fmt) "mk_amd_vi: " fmt

#include <linux/amd-iommu.h>
#include <linux/bitmap.h>
#include <linux/device.h>
#include <linux/libfdt.h>
#include <linux/msi.h>
#include <linux/multikernel.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <asm/multikernel.h>

#include "iommu.h"

#define MK_AMD_VI_COMPATIBLE	"multikernel,amd-vi"

/*
 * @rid is the function as the instance sees it on its PCI bus, @event an
 * entry of its remapping table.
 */
struct mk_io_msi_payload {
	u64 seq;
	u64 phys_cpu;		/* MAP: target, one of the sender's */
	u32 domain;		/* PCI domain */
	u32 rid;		/* bus << 8 | devfn */
	u32 event;		/* The first of @count, which an ALLOC is answered */
	u32 count;		/* ALLOC and FREE */
	u32 align;		/* ALLOC: as multiple message MSI needs */
	u32 vector;		/* MAP */
	u32 address_lo;		/* ACK of an ALLOC: where the function writes @event */
	u32 address_hi;
	int result;		/* ACK: 0 or -errno */
};

/*
 * Host side
 */

/* A devres of the function, which ends with the lease */
struct mk_amd_vi_lent {
	DECLARE_BITMAP(events, AMD_IOMMU_MAX_IRTES);
};

struct mk_amd_vi_work {
	struct work_struct work;
	struct mk_instance *instance;
	u32 subtype;
	struct mk_io_msi_payload req;
};

int mk_amd_vi_emit_units(void *fdt)
{
	struct amd_iommu_unit_desc unit;
	fdt64_t reg[2];
	char name[32];
	int i, ret = 0;

	for (i = 0; !ret && !amd_iommu_unit_get(i, &unit); i++) {
		reg[0] = cpu_to_fdt64(unit.base);
		reg[1] = cpu_to_fdt64(unit.size);
		snprintf(name, sizeof(name), "iommu@%llx", unit.base);
		ret = fdt_begin_node(fdt, name);
		if (!ret)
			ret = fdt_property_string(fdt, "compatible", MK_AMD_VI_COMPATIBLE);
		if (!ret)
			ret = fdt_property(fdt, "reg", reg, sizeof(reg));
		if (!ret)
			ret = fdt_property_u32(fdt, "linux,pci-domain", unit.segment);
		if (!ret && unit.irq_remap)
			ret = fdt_property(fdt, "multikernel,irq-remap", NULL, 0);
		if (!ret)
			ret = fdt_end_node(fdt);
	}
	return ret;
}

static void mk_amd_vi_unlend(struct device *dev, void *res)
{
	struct mk_amd_vi_lent *lent = res;
	unsigned int event;

	for_each_set_bit(event, lent->events, AMD_IOMMU_MAX_IRTES)
		amd_iommu_irte_free(to_pci_dev(dev), event, 1);
}

static struct mk_amd_vi_lent *mk_amd_vi_lent_find(struct pci_dev *pdev)
{
	return devres_find(&pdev->dev, mk_amd_vi_unlend, NULL, NULL);
}

/* Before the first entry is allocated, so that every entry goes with the lease */
static struct mk_amd_vi_lent *mk_amd_vi_lent_get(struct pci_dev *pdev)
{
	struct mk_amd_vi_lent *lent = mk_amd_vi_lent_find(pdev);

	if (lent)
		return lent;
	lent = devres_alloc(mk_amd_vi_unlend, sizeof(*lent), GFP_KERNEL | __GFP_ZERO);
	if (lent)
		devres_add(&pdev->dev, lent);
	return lent;
}

/* Only entries that were allocated under this lease */
static bool mk_amd_vi_owns(struct pci_dev *pdev, u32 first, u32 count)
{
	struct mk_amd_vi_lent *lent = mk_amd_vi_lent_find(pdev);
	u32 end;

	if (!lent || !count || check_add_overflow(first, count, &end) ||
	    end > AMD_IOMMU_MAX_IRTES)
		return false;
	return find_next_zero_bit(lent->events, end, first) == end;
}

static int mk_amd_vi_alloc(struct pci_dev *pdev, struct mk_io_msi_payload *req)
{
	struct mk_amd_vi_lent *lent = mk_amd_vi_lent_get(pdev);
	struct msi_msg msg;
	int first;

	if (!lent)
		return -ENOMEM;
	first = amd_iommu_irte_alloc(pdev, req->count, req->align, &msg);
	if (first < 0)
		return first;

	bitmap_set(lent->events, first, req->count);
	req->event = first;
	req->address_lo = msg.address_lo;
	req->address_hi = msg.address_hi;
	return 0;
}

static int mk_amd_vi_free(struct pci_dev *pdev, const struct mk_io_msi_payload *req)
{
	if (!mk_amd_vi_owns(pdev, req->event, req->count))
		return -EPERM;
	amd_iommu_irte_free(pdev, req->event, req->count);
	bitmap_clear(mk_amd_vi_lent_find(pdev)->events, req->event, req->count);
	return 0;
}

/* Only to a CPU the instance owns */
static int mk_amd_vi_map(struct mk_instance *instance, struct pci_dev *pdev,
			 const struct mk_io_msi_payload *req)
{
	if (!mk_amd_vi_owns(pdev, req->event, 1) ||
	    !mk_cpu_set_contains(instance->cpus, req->phys_cpu))
		return -EPERM;
	if (req->vector > 0xff)
		return -EINVAL;
	return amd_iommu_irte_route(pdev, req->event, req->vector, req->phys_cpu);
}

static int mk_amd_vi_unmap(struct pci_dev *pdev, const struct mk_io_msi_payload *req)
{
	if (!mk_amd_vi_owns(pdev, req->event, 1))
		return -EPERM;
	amd_iommu_irte_unroute(pdev, req->event);
	return 0;
}

/* Only for a function the instance holds a lease on */
static int mk_amd_vi_serve(struct mk_instance *instance, u32 subtype,
			   struct mk_io_msi_payload *req)
{
	struct mk_lease *lease;
	struct pci_dev *pdev;
	int ret = -EPERM;

	pdev = pci_get_domain_bus_and_slot(req->domain, req->rid >> 8,
					   req->rid & 0xff);
	if (!pdev)
		return -ENODEV;

	/* Held across, so that nothing is allocated or routed after the lease */
	lease = mk_lease_lock(instance, pdev);
	if (!lease)
		goto put;

	switch (subtype) {
	case MK_IO_MSI_ALLOC:
		ret = mk_amd_vi_alloc(pdev, req);
		break;
	case MK_IO_MSI_FREE:
		ret = mk_amd_vi_free(pdev, req);
		break;
	case MK_IO_MSI_MAP:
		ret = mk_amd_vi_map(instance, pdev, req);
		break;
	case MK_IO_MSI_UNMAP:
		ret = mk_amd_vi_unmap(pdev, req);
		break;
	}
	mk_lease_unlock(lease);
put:
	pci_dev_put(pdev);
	return ret;
}

static void mk_amd_vi_work_fn(struct work_struct *work)
{
	struct mk_amd_vi_work *w = container_of(work, struct mk_amd_vi_work, work);
	struct mk_io_msi_payload *req = &w->req;

	req->result = mk_amd_vi_serve(w->instance, w->subtype, req);
	mk_send_message_to(w->instance, MK_MSG_IO, MK_IO_MSI_ACK, req, sizeof(*req));
	mk_instance_put(w->instance);
	kfree(w);
}

static void mk_amd_vi_request(struct mk_instance *from, u32 subtype,
			      const struct mk_io_msi_payload *req)
{
	struct mk_amd_vi_work *w = kzalloc_obj(*w, GFP_ATOMIC);

	if (!w)
		return;
	w->instance = mk_instance_get(from);
	w->subtype = subtype;
	w->req = *req;
	INIT_WORK(&w->work, mk_amd_vi_work_fn);
	schedule_work(&w->work);
}

void mk_amd_vi_msg(struct mk_instance *from, u32 subtype, void *payload,
		   u32 payload_len)
{
	struct mk_io_msi_payload *p = payload;

	if (payload_len < sizeof(*p))
		return;

	if (subtype == MK_IO_MSI_ACK)
		mk_request_answer(p->seq, p, sizeof(*p));
	else
		mk_amd_vi_request(from, subtype, p);
}

/*
 * Instance side, for the interrupt domain in drivers/iommu/amd-ir-mk.c
 */

static int mk_amd_vi_ask(u32 subtype, struct pci_dev *pdev,
			 struct mk_io_msi_payload *req)
{
	struct mk_instance *host = READ_ONCE(host_instance);
	struct mk_io_msi_payload ack;
	int ret;

	/* A fenced host never answers */
	if (!host || mk_host_fenced())
		return -ENODEV;
	req->seq = mk_request_seq();
	req->domain = pci_domain_nr(pdev->bus);
	req->rid = pci_dev_id(pdev);
	ret = mk_request_sync(host, MK_MSG_IO, subtype, req->seq, req,
			      sizeof(*req), &ack, sizeof(ack));
	if (ret)
		return ret;
	*req = ack;
	return ack.result;
}

/**
 * mk_msi_proxy_alloc - Have the host set MSIs aside for a function
 * @pdev: the function
 * @count: how many, consecutive
 * @align: as multiple message MSI needs
 * @msg: set to the message the function raises the first one with; the
 *	 data of each one after is one more
 *
 * Returns the first event, which names it to the calls below, or a negative
 * error code.
 */
int mk_msi_proxy_alloc(struct pci_dev *pdev, unsigned int count, bool align,
		       struct msi_msg *msg)
{
	struct mk_io_msi_payload req = { .count = count, .align = align };
	int ret = mk_amd_vi_ask(MK_IO_MSI_ALLOC, pdev, &req);

	if (ret)
		return ret;
	msg->address_lo = req.address_lo;
	msg->address_hi = req.address_hi;
	msg->data = req.event;
	return req.event;
}

/**
 * mk_msi_proxy_free - Give back events from mk_msi_proxy_alloc()
 * @pdev: the function
 * @event: the first one
 * @count: how many
 */
void mk_msi_proxy_free(struct pci_dev *pdev, u32 event, unsigned int count)
{
	struct mk_io_msi_payload req = { .event = event, .count = count };

	mk_amd_vi_ask(MK_IO_MSI_FREE, pdev, &req);
}

/**
 * mk_msi_proxy_map - Have an event delivered to a vector of a CPU
 * @pdev: the function
 * @event: the event
 * @apicid: target, one of this kernel's CPUs
 * @vector: the vector
 *
 * Returns once the host delivers there. Any context. Returns 0 or a
 * negative error code.
 */
int mk_msi_proxy_map(struct pci_dev *pdev, u32 event, u32 apicid, u8 vector)
{
	struct mk_io_msi_payload req = {
		.event = event,
		.phys_cpu = apicid,
		.vector = vector,
	};

	return mk_amd_vi_ask(MK_IO_MSI_MAP, pdev, &req);
}

/**
 * mk_msi_proxy_unmap - Have an event delivered nowhere
 * @pdev: the function
 * @event: the event, which stays allocated
 *
 * Any context.
 */
void mk_msi_proxy_unmap(struct pci_dev *pdev, u32 event)
{
	struct mk_io_msi_payload req = { .event = event };

	mk_amd_vi_ask(MK_IO_MSI_UNMAP, pdev, &req);
}

/* Whether the host's IOMMU at @base, or with a zero @base any of them, remaps */
static bool mk_amd_vi_remaps(u64 base)
{
	struct device_node *units __free(device_node) =
		of_find_node_by_path(MK_IOMMUS_PATH);
	u64 reg;

	for_each_child_of_node_scoped(units, np) {
		if (of_device_is_compatible(np, MK_AMD_VI_COMPATIBLE) &&
		    of_property_read_bool(np, "multikernel,irq-remap") &&
		    !of_property_read_u64(np, "reg", &reg) &&
		    (!base || reg == base))
			return true;
	}
	return false;
}

/**
 * mk_msi_proxied - Whether the host routes the MSIs of a function
 * @pdev: the function, or NULL for whether it routes those of any
 */
bool mk_msi_proxied(struct pci_dev *pdev)
{
	struct device_node *np;
	u64 base;

	if (!mk_spawned())
		return false;
	if (!pdev)
		return mk_amd_vi_remaps(0);
	np = pci_device_to_OF_node(pdev);
	return np && !of_property_read_u64(np, "multikernel,iommu-base", &base) &&
	       mk_amd_vi_remaps(base);
}
