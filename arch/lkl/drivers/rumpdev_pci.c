/*
 * rumprun PCI access (reused from src-netbsd/.../rumpdev_pci.c)
 */

/*
 * Copyright (c) 2013 Antti Kantee.  All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/types.h>
#include <linux/io.h>
#include <asm/host_ops.h>

#include "pci_user.h"

struct rump_pci_sysdata {
	int domain; /* PCI domain */
};

/* stubs: should not called */
int __weak rumpcomp_pci_confread(unsigned int bus, unsigned int dev,
				 unsigned int fun,
				 int reg, unsigned int *value)
{
	return 0;
}

int __weak rumpcomp_pci_confwrite(unsigned int bus, unsigned int dev,
				  unsigned int fun,
				  int reg, unsigned int value)
{
	return 0;
}

void * __weak rumpcomp_pci_map(unsigned long addr, unsigned long len)
{
	return NULL;
}

int __weak rumpcomp_pci_irq_map(unsigned int bus, unsigned int device,
				unsigned int fun,
				int intrline, unsigned int cookie)
{
	return 0;
}

void * __weak rumpcomp_pci_irq_establish(unsigned int cookie,
					 int (*handler)(void *), void *data)
{
	return NULL;
}

void __iomem *__pci_ioport_map(struct pci_dev *dev,
			       unsigned long port, unsigned int nr)
{
	/* XXX: no care at the moment */
	return rumpcomp_pci_map(port, nr);
}

/* from arch/x86/pci/common.c  */
void pcibios_fixup_bus(struct pci_bus *b)
{
	pci_read_bridge_bases(b);
}

/* from arch/x86/pci/i386.c */
resource_size_t
pcibios_align_resource(void *data, const struct resource *res,
		       resource_size_t size, resource_size_t align)
{
	return 0;
}

/* from drivers/pci/access.c
 *
 * @bus: PCI bus to scan
 * @devfn: slot number to scan (must have zero function.)
 */
void *rump_pci_map_bus(struct pci_bus *bus, unsigned int devfn, int where)
{
	unsigned long addr;

	addr =  (1 << 31) | (bus->number << 16) | (PCI_SLOT(devfn) << 11) |
		(PCI_FUNC(devfn) << 8) | (where & 0xfc);

	/* FIXME: length? */
	return rumpcomp_pci_map(addr, 0);
}

int rump_pci_generic_read(struct pci_bus *bus, unsigned int devfn,
			  int where, int size, u32 *val)
{

	rumpcomp_pci_confread(bus->number, PCI_SLOT(devfn),
			      PCI_FUNC(devfn), where, val);
	if (size <= 2)
		*val = (*val >> (8 * (where & 3))) & ((1 << (size * 8)) - 1);

	return PCIBIOS_SUCCESSFUL;
}

int rump_pci_generic_write(struct pci_bus *bus, unsigned int devfn,
			    int where, int size, u32 val)
{
	u32 mask, tmp;

	if (size == 4) {
		rumpcomp_pci_confwrite(bus->number, PCI_SLOT(devfn),
				       PCI_FUNC(devfn), where, val);
		return PCIBIOS_SUCCESSFUL;
	}

	mask = ~(((1 << (size * 8)) - 1) << ((where & 0x3) * 8));

	/* This brings the way much overhead though I picked this
	 * code from access.c.. maybe should come up with single
	 * write method to avoid that.
	 */

	rumpcomp_pci_confread(bus->number, PCI_SLOT(devfn),
			      PCI_FUNC(devfn), where, &tmp);
	tmp &= mask;
	tmp |= val << ((where & 0x3) * 8);

	rumpcomp_pci_confwrite(bus->number, PCI_SLOT(devfn),
			       PCI_FUNC(devfn), where, tmp);

	return PCIBIOS_SUCCESSFUL;
}


#ifdef __HAVE_PCIIDE_MACHDEP_COMPAT_INTR_ESTABLISH
#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pciidereg.h>
#include <dev/pci/pciidevar.h>

void *
pciide_machdep_compat_intr_establish(device_t dev,
	const struct pci_attach_args *pa, int chan,
	int (*func)(void *), void *arg)
{
	pci_intr_handle_t ih;
	struct pci_attach_args mypa = *pa;

	mypa.pa_intrline = PCIIDE_COMPAT_IRQ(chan);
	if (pci_intr_map(&mypa, &ih) != 0)
		return NULL;
	return rumpcomp_pci_irq_establish(ih, func, arg);
}

__strong_alias(pciide_machdep_compat_intr_disestablish, pci_intr_disestablish);
#endif /* __HAVE_PCIIDE_MACHDEP_COMPAT_INTR_ESTABLISH */


/* from drivers/pci/xen-pcifront.c */
static int pci_lib_claim_resource(struct pci_dev *dev, void *data)
{
	int i;
	struct resource *r;

	for (i = 0; i < PCI_NUM_RESOURCES; i++) {
		r = &dev->resource[i];

		if (!r->parent && r->start && r->flags) {
			dev_info(&dev->dev, "claiming resource %s/%d\n",
				pci_name(dev), i);
			if (pci_claim_resource(dev, i)) {
				dev_err(&dev->dev, "Could not claim resource "
					"%s/%d! Device offline. Try using "
					"e820_host=1 in the guest config.\n",
					pci_name(dev), i);
			}
		}
	}

	return 0;
}

static int rump_trigger_irq(void *arg)
{
	struct irq_data *data = arg;

	lkl_trigger_irq(data->irq);
	return 0;
}

int rump_pci_irq_request(struct irq_data *data)
{
	int ret, int_irq;
	struct irq_desc *desc = irq_to_desc(data->irq);
	const char *name = desc->name ? desc->name : "null"; /* XXX */

	static int intr[5] = {9, 10, 11, 14, 15};
	static int intnum = 0;

	/* setup IRQ */
	int_irq = lkl_get_free_irq(name);

	ret = rumpcomp_pci_irq_map(0, 0, 0, intr[intnum++], int_irq);
	rumpcomp_pci_irq_establish(int_irq, rump_trigger_irq, data);

	return 0;
}

void rump_pci_irq_release(struct irq_data *data)
{
	/* XXX: NOP */
}

struct pci_ops rump_pci_root_ops = {
	.map_bus = rump_pci_map_bus,
	.read = rump_pci_generic_read,
	.write = rump_pci_generic_write,
};


static int __init rump_pci_init(void)
{
	struct pci_bus *bus;
	struct rump_pci_sysdata *sd;
	int busnum = 0;

	sd = kzalloc(sizeof(*sd), GFP_KERNEL);
	if (!sd)
		return -1;

	pr_info("PCI: root bus %02x: using default resources\n", busnum);
	bus = pci_scan_bus(busnum, &rump_pci_root_ops, sd);
	if (!bus) {
		kfree(sd);
		return -1;
	}
	pci_walk_bus(bus, pci_lib_claim_resource, NULL);
	pci_bus_add_devices(bus);

	return 0;
}
subsys_initcall(rump_pci_init);
