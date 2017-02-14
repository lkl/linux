/* Reused from src-netbsd/sys/rump/dev/lib/libpci/pci_user.h */
/*
 * Possible userfeature macro flags:
 *
 *   RUMPCOMP_USERFEATURE_PCI_DMAFREE:
 *	Support free'ing DMA memory.  If not, panic() when free() is called.
 *
 *   RUMPCOMP_USERFEATURE_PCI_IOSPACE
 *	Support for PCI I/O space.  If yes, rumpcomp_pci_iospace_init()
 *	must be provided.
 */


void *rumpcomp_pci_map(unsigned long addr, unsigned long len);
int rumpcomp_pci_confread(unsigned int bus, unsigned int dev,
			  unsigned int fun, int reg, unsigned int *value);
int rumpcomp_pci_confwrite(unsigned int bus, unsigned int dev,
			   unsigned int fun, int reg, unsigned int value);
int rumpcomp_pci_irq_map(unsigned int bus, unsigned int device,
			 unsigned int fun, int intrline, unsigned int cookie);
void *rumpcomp_pci_irq_establish(unsigned int cookie,
				 int (*handler)(void *), void *data);

/* XXX: needs work: support boundary-restricted allocations */
int rumpcomp_pci_dmalloc(size_t size, size_t align,
			 unsigned long *pap, unsigned long *vap);
#ifdef RUMPCOMP_USERFEATURE_PCI_DMAFREE
void rumpcomp_pci_dmafree(unsigned long mem, size_t size);
#endif

struct rumpcomp_pci_dmaseg {
	unsigned long ds_pa;
	unsigned long ds_len;
	unsigned long ds_vacookie;
};
int rumpcomp_pci_dmamem_map(struct rumpcomp_pci_dmaseg *dss, size_t nseg,
			    size_t totlen, void **vap);

unsigned long rumpcomp_pci_virt_to_mach(void *virt);

#ifdef RUMPCOMP_USERFEATURE_PCI_IOSPACE
int rumpcomp_pci_iospace_init(void);
#endif
