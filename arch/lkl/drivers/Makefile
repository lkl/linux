
obj-y = rumpdev_pci.o

# need to build with
librumpdev_linux_pci.a: ${RUMP_BMK_PCI_HYPERCALLS}
	rm -f $@
	$(AR) rc $@ ${RUMP_BMK_PCI_HYPERCALLS}

install: librumpdev_linux_pci.a
	install -D librumpdev_linux_pci.a ${DESTDIR}/lib/


.PHONY: ${RUMP_BMK_PCI_HYPERCALLS}
