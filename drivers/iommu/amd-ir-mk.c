// SPDX-License-Identifier: GPL-2.0-only
/*
 * MSI domain of a multikernel instance behind AMD IOMMUs that remap
 * interrupts.
 *
 * The IOMMUs belong to the kernel that spawned this one, and so do their
 * remapping tables. This is what is left of the remapping driver when
 * someone else writes the tables: an MSI parent domain on top of the
 * vector domain that asks the host for the entries of a function
 * (mk_msi_proxy_alloc()) and to deliver each one to the vector this kernel
 * picked (mk_msi_proxy_map()). The message a function raises an entry with
 * comes with the answer, because only the host knows the size of the table.
 * The function is programmed from here as usual.
 */

#include <linux/device.h>
#include <linux/init.h>
#include <linux/irqchip/irq-msi-lib.h>
#include <linux/irqdomain.h>
#include <linux/msi.h>
#include <linux/multikernel.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <asm/apic.h>
#include <asm/hw_irq.h>
#include <asm/irqdomain.h>

struct mk_ir_irq {
	struct pci_dev *pdev;
	u32 event;
	struct msi_msg msg;
};

static struct irq_domain *mk_ir_domain;

static int mk_ir_map(struct irq_data *irq_data)
{
	struct mk_ir_irq *irq = irq_data->chip_data;
	struct irq_cfg *cfg = irqd_cfg(irq_data);

	return mk_msi_proxy_map(irq->pdev, irq->event, cfg->dest_apicid, cfg->vector);
}

/* The old vector stays until the host delivers to the new one */
static int mk_ir_set_affinity(struct irq_data *irq_data,
			      const struct cpumask *mask, bool force)
{
	struct irq_data *parent = irq_data->parent_data;
	int ret;

	ret = parent->chip->irq_set_affinity(parent, mask, force);
	if (ret < 0 || ret == IRQ_SET_MASK_OK_DONE)
		return ret;

	ret = mk_ir_map(irq_data);
	if (ret)
		return ret;
	vector_schedule_cleanup(irqd_cfg(irq_data));
	return IRQ_SET_MASK_OK_DONE;
}

static void mk_ir_compose_msi_msg(struct irq_data *irq_data, struct msi_msg *msg)
{
	struct mk_ir_irq *irq = irq_data->chip_data;

	*msg = irq->msg;
}

static struct irq_chip mk_ir_chip = {
	.name			= "AMD-IR-MK",
	.irq_ack		= apic_ack_irq,
	.irq_set_affinity	= mk_ir_set_affinity,
	.irq_compose_msi_msg	= mk_ir_compose_msi_msg,
};

static void mk_ir_free(struct irq_domain *domain, unsigned int virq,
		       unsigned int nr_irqs)
{
	struct irq_data *irq_data;
	struct mk_ir_irq *irq;
	unsigned int i;

	for (i = 0; i < nr_irqs; i++) {
		irq_data = irq_domain_get_irq_data(domain, virq + i);
		irq = irq_data ? irq_data->chip_data : NULL;
		if (!irq)
			continue;
		mk_msi_proxy_free(irq->pdev, irq->event, 1);
		kfree(irq);
	}
	irq_domain_free_irqs_common(domain, virq, nr_irqs);
}

static int mk_ir_alloc(struct irq_domain *domain, unsigned int virq,
		       unsigned int nr_irqs, void *arg)
{
	struct irq_alloc_info *info = arg;
	struct irq_data *irq_data;
	struct mk_ir_irq *irq;
	struct pci_dev *pdev;
	struct msi_msg msg;
	unsigned int i;
	int first, ret;

	if (!info || (info->type != X86_IRQ_ALLOC_TYPE_PCI_MSI &&
		      info->type != X86_IRQ_ALLOC_TYPE_PCI_MSIX))
		return -EINVAL;
	if (nr_irqs > 1 && info->type != X86_IRQ_ALLOC_TYPE_PCI_MSI)
		return -EINVAL;
	pdev = msi_desc_to_pci_dev(info->desc);

	ret = irq_domain_alloc_irqs_parent(domain, virq, nr_irqs, arg);
	if (ret < 0)
		return ret;

	first = mk_msi_proxy_alloc(pdev, nr_irqs,
				   info->type == X86_IRQ_ALLOC_TYPE_PCI_MSI, &msg);
	if (first < 0) {
		pci_warn(pdev, "no remapped MSI from the host: %d\n", first);
		irq_domain_free_irqs_common(domain, virq, nr_irqs);
		return first;
	}

	for (i = 0; i < nr_irqs; i++) {
		irq_data = irq_domain_get_irq_data(domain, virq + i);
		irq = kzalloc_obj(*irq);
		if (!irq)
			goto free;
		irq->pdev = pdev;
		irq->event = first + i;
		irq->msg = msg;
		irq->msg.data += i;
		irq_data->hwirq = (irq_hw_number_t)pci_dev_id(pdev) << 16 | irq->event;
		irq_data->chip = &mk_ir_chip;
		irq_data->chip_data = irq;
	}
	return 0;

free:
	/* Those past @i have no chip data to be freed by */
	mk_msi_proxy_free(pdev, first + i, nr_irqs - i);
	mk_ir_free(domain, virq, nr_irqs);
	return -ENOMEM;
}

static int mk_ir_activate(struct irq_domain *domain, struct irq_data *irq_data,
			  bool reserve)
{
	return mk_ir_map(irq_data);
}

static void mk_ir_deactivate(struct irq_domain *domain, struct irq_data *irq_data)
{
	struct mk_ir_irq *irq = irq_data->chip_data;

	mk_msi_proxy_unmap(irq->pdev, irq->event);
}

static const struct irq_domain_ops mk_ir_domain_ops = {
	.alloc		= mk_ir_alloc,
	.free		= mk_ir_free,
	.activate	= mk_ir_activate,
	.deactivate	= mk_ir_deactivate,
};

static const struct msi_parent_ops mk_ir_msi_parent_ops = {
	.supported_flags	= X86_VECTOR_MSI_FLAGS_SUPPORTED | MSI_FLAG_MULTI_PCI_MSI,
	.bus_select_token	= DOMAIN_BUS_AMDVI,
	.bus_select_mask	= MATCH_PCI_MSI,
	.prefix			= "IR-",
	.init_dev_msi_info	= msi_parent_init_dev_msi_info,
};

/* After the arch has given the function its default MSI domain */
static int mk_ir_bus_notify(struct notifier_block *nb, unsigned long action,
			    void *data)
{
	if (action == BUS_NOTIFY_ADD_DEVICE && mk_msi_proxied(to_pci_dev(data)))
		dev_set_msi_domain(data, mk_ir_domain);
	return NOTIFY_OK;
}

static struct notifier_block mk_ir_bus_nb = {
	.notifier_call = mk_ir_bus_notify,
};

/* Before the PCI buses of an instance are scanned, at subsys_initcall */
static int __init mk_ir_init(void)
{
	struct irq_domain_info info = {
		.ops	= &mk_ir_domain_ops,
		.parent	= x86_vector_domain,
	};

	if (!mk_msi_proxied(NULL))
		return 0;

	info.fwnode = irq_domain_alloc_named_fwnode("AMD-IR-MK");
	if (!info.fwnode)
		return -ENOMEM;
	mk_ir_domain = msi_create_parent_irq_domain(&info, &mk_ir_msi_parent_ops);
	if (!mk_ir_domain) {
		irq_domain_free_fwnode(info.fwnode);
		return -ENOMEM;
	}
	return bus_register_notifier(&pci_bus_type, &mk_ir_bus_nb);
}
arch_initcall(mk_ir_init);
