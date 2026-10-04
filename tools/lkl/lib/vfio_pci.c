// SPDX-License-Identifier: GPL-2.0
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <lkl_host.h>
#include <stdio.h>
#include <sys/types.h>
#include <stdint.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <linux/vfio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/vfio.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include "iomem.h"

struct vfio_pci_msi_vector {
	int irq;
	int fd;
};

struct lkl_pci_dev {
	struct lkl_sem *thread_init_sem;
	int thread_init_status;
	int irq;
	lkl_thread_t int_thread;
	int int_thread_running;
	int intx_quit;
	int fd;
	int irq_fd;
	lkl_thread_t msi_thread;
	int msi_thread_running;
	int msi_quit;
	int msi_type;
	int msi_nvec;
	struct vfio_pci_msi_vector *msi_vectors;
	struct vfio_device_info device_info;
	struct vfio_region_info config_reg;
	struct vfio_iommu_type1_dma_map dma_map;
};

/**
 * vfio_pci_add - Create a new pci device
 *
 * The device should be assigned to VFIO by the host in advance.
 *
 * @name - PCI device name (as %x:%x:%x.%x format)
 * @kernel_ram - the start address of kernel memory needed to be mapped for DMA.
 * The address must be aligned to the page size.
 * @ram_size - the size of kernel memory, should be page-aligned as well.
 */

static struct lkl_pci_dev *vfio_pci_add(const char *name, void *kernel_ram,
					lkl_ulong_t ram_size)
{
	struct lkl_pci_dev *dev;
	char path[128], link[128], *l;
	int segn, busn, devn, funcn;
	int i;
	int container_fd = -1, group_fd = -1;
	const char *step = NULL;
	struct vfio_group_status group_status = { .argsz = sizeof(
							  group_status) };
	struct vfio_iommu_type1_info iommu_info = { .argsz = sizeof(
							    iommu_info) };

	dev = malloc(sizeof(*dev));
	if (!dev)
		return NULL;

	memset(dev, 0, sizeof(*dev));
	dev->fd = -1;
	dev->irq_fd = -1;

	dev->device_info.argsz = sizeof(struct vfio_device_info);
	dev->config_reg.argsz = sizeof(struct vfio_region_info);
	dev->dma_map.argsz = sizeof(struct vfio_iommu_type1_dma_map);

	container_fd = open("/dev/vfio/vfio", O_RDWR);
	if (container_fd < 0) {
		step = "open /dev/vfio/vfio";
		goto error;
	}

	step = "check api version / type1 extension";
	if (ioctl(container_fd, VFIO_GET_API_VERSION) != VFIO_API_VERSION ||
	    ioctl(container_fd, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) == 0)
		goto error;

	if (sscanf(name, "vfio%x:%x:%x.%x", &segn, &busn, &devn, &funcn) != 4) {
		step = "parse device name";
		goto error;
	}

	snprintf(path, sizeof(path),
		 "/sys/bus/pci/devices/%04x:%02x:%02x.%01x/iommu_group", segn,
		 busn, devn, funcn);

	step = "readlink iommu_group";
	i = readlink(path, link, sizeof(link) - 1);
	if (i < 0)
		goto error;

	link[i] = '\0';
	l = strrchr(link, '/');
	if (l == NULL) {
		step = "parse iommu_group link";
		goto error;
	}

	snprintf(path, sizeof(path), "/dev/vfio%s", l);

	step = "open iommu group";
	group_fd = open(path, O_RDWR);
	if (group_fd < 0)
		goto error;

	step = "get group status";
	if (ioctl(group_fd, VFIO_GROUP_GET_STATUS, &group_status) < 0)
		goto error;

	if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		step = "group not viable";
		goto error;
	}

	step = "set container";
	if (ioctl(group_fd, VFIO_GROUP_SET_CONTAINER, &container_fd) < 0)
		goto error;

	step = "set iommu type1";
	if (ioctl(container_fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0)
		goto error;

	step = "get iommu info";
	if (ioctl(container_fd, VFIO_IOMMU_GET_INFO, &iommu_info) < 0)
		goto error;

	/* if kernel_ram is null, assume the memory is already initialized
	 * by another device, and skip this step.
	 */
	if (kernel_ram) {
		dev->dma_map.vaddr = (uint64_t)kernel_ram;
		dev->dma_map.size = ram_size;
		dev->dma_map.iova = 0;
		dev->dma_map.flags =
			VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
		step = "map dma";
		if (ioctl(container_fd, VFIO_IOMMU_MAP_DMA, &dev->dma_map) < 0)
			goto error;
	}

	snprintf(path, sizeof(path), "%04x:%02x:%02x.%01x", segn, busn, devn,
		 funcn);
	step = "get device fd";
	dev->fd = ioctl(group_fd, VFIO_GROUP_GET_DEVICE_FD, path);

	if (dev->fd < 0)
		goto error;

	step = "get device info";
	if (ioctl(dev->fd, VFIO_DEVICE_GET_INFO, &dev->device_info) < 0)
		goto error;

	if (dev->device_info.num_regions <= VFIO_PCI_CONFIG_REGION_INDEX) {
		step = "no config region";
		goto error;
	}

	dev->config_reg.index = VFIO_PCI_CONFIG_REGION_INDEX;

	step = "get config region info";
	if (ioctl(dev->fd, VFIO_DEVICE_GET_REGION_INFO, &dev->config_reg) < 0)
		goto error;

	return dev;

error:
	lkl_printf("lkl_vfio_pci: failed to create a PCI device for %s: %s (%d: %s)\n",
		   name, step ? step : "unknown", errno, strerror(errno));
	if (container_fd >= 0)
		close(container_fd);
	if (group_fd >= 0)
		close(group_fd);
	free(dev);
	return NULL;
}

static int check_irq_status(struct lkl_pci_dev *dev)
{
	unsigned short status;

	if (pread(dev->fd, &status, 2, dev->config_reg.offset + 6) != 2)
		return 0;
	return (status & (1 << 3)) ? 1 : 0;
}

static int vfio_pci_disable_irq_index(struct lkl_pci_dev *dev,
				      unsigned int index)
{
	struct vfio_irq_set irq_set = {
		.argsz = sizeof(irq_set),
		.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER,
		.index = index,
		.start = 0,
		.count = 0,
	};

	return ioctl(dev->fd, VFIO_DEVICE_SET_IRQS, &irq_set);
}

static void vfio_int_thread(void *_dev)
{
	eventfd_t icount;
	struct lkl_pci_dev *dev = (struct lkl_pci_dev *)_dev;
	struct timespec req = { 0, 1000 * 1000 };
	struct vfio_irq_set *irq_set;
	char irq_set_buf[sizeof(struct vfio_irq_set) + sizeof(int)];
	fd_set rfds;

	irq_set = (struct vfio_irq_set *)irq_set_buf;
	irq_set->argsz = sizeof(irq_set_buf);
	irq_set->count = 1;
	irq_set->flags =
		VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
	irq_set->index = VFIO_PCI_INTX_IRQ_INDEX;
	irq_set->start = 0;
	dev->irq_fd = eventfd(0, EFD_CLOEXEC);
	if (dev->irq_fd < 0)
		goto init_error;
	*(int *)&irq_set->data = dev->irq_fd;

	if (ioctl(dev->fd, VFIO_DEVICE_SET_IRQS, irq_set))
		goto init_error;

	dev->thread_init_status = 0;
	lkl_host_ops.sem_up(dev->thread_init_sem);

	while (1) {
		/* We should wait until the driver actually handles
		 * an interrupt by monitoring the PCI interrupt status bit.
		 */
		while (check_irq_status(dev) && !dev->intx_quit) {
			lkl_trigger_irq(dev->irq);
			nanosleep(&req, NULL);
		}

		if (dev->intx_quit)
			return;

		/* unmask interrupts */
		irq_set->argsz = sizeof(*irq_set);
		irq_set->count = 1;
		irq_set->flags =
			VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_UNMASK;
		irq_set->index = VFIO_PCI_INTX_IRQ_INDEX;
		irq_set->start = 0;
		if (ioctl(dev->fd, VFIO_DEVICE_SET_IRQS, irq_set))
			goto handling_error;

		/* Wait for next interrupt. */
		while (1) {
			struct timeval tv;
			int rc;

			FD_ZERO(&rfds);
			FD_SET(dev->irq_fd, &rfds);
			tv.tv_sec = 0;
			tv.tv_usec = 100 * 1000;
			rc = select(dev->irq_fd + 1, &rfds, NULL, NULL, &tv);
			if (rc == -1)
				goto handling_error;
			else if (rc)
				if (read(dev->irq_fd, &icount, sizeof(icount)) <
				    0)
					goto handling_error;
				else
					break;
			else if (dev->intx_quit)
				return;
		}
	}

init_error:
	lkl_printf("lkl_vfio_pci: failed to setup INTx for a device\n");
	dev->thread_init_status = -1;
	lkl_host_ops.sem_up(dev->thread_init_sem);
	return;
handling_error:
	lkl_printf("lkl_vfio_pci: unknown error in the interrupt handler\n");
}

static int vfio_pci_start_intx(struct lkl_pci_dev *dev)
{
	struct vfio_irq_info irq = { .argsz = sizeof(irq) };

	if (dev->device_info.num_irqs <= VFIO_PCI_INTX_IRQ_INDEX)
		return 0;

	irq.index = VFIO_PCI_INTX_IRQ_INDEX;

	if (ioctl(dev->fd, VFIO_DEVICE_GET_IRQ_INFO, &irq) || irq.count != 1)
		/*
		 * Some devices (e.g. QEMU's emulated NVMe) only support
		 * MSI/MSI-X and have no INTx. That is fine as long as the
		 * driver enables MSI/MSI-X, so do not fail device setup.
		 */
		return 0;

	dev->thread_init_sem = lkl_host_ops.sem_alloc(0);
	if (!dev->thread_init_sem)
		return -1;

	dev->thread_init_status = -1;
	dev->intx_quit = 0;

	dev->int_thread =
		lkl_host_ops.thread_create(vfio_int_thread, (void *)dev);
	if (!dev->int_thread) {
		lkl_host_ops.sem_free(dev->thread_init_sem);
		return -1;
	}

	/* wait until the interrupt handler thread is ready */
	lkl_host_ops.sem_down(dev->thread_init_sem);
	lkl_host_ops.sem_free(dev->thread_init_sem);
	dev->thread_init_sem = NULL;

	if (dev->thread_init_status < 0) {
		lkl_host_ops.thread_join(dev->int_thread);
		dev->int_thread = 0;
		if (dev->irq_fd >= 0) {
			close(dev->irq_fd);
			dev->irq_fd = -1;
		}
		return -1;
	}

	dev->int_thread_running = 1;
	return 0;
}

static void vfio_pci_stop_intx(struct lkl_pci_dev *dev)
{
	if (dev->int_thread_running) {
		dev->intx_quit = 1;
		lkl_host_ops.thread_join(dev->int_thread);
		dev->int_thread = 0;
		dev->int_thread_running = 0;
	}

	if (dev->irq_fd >= 0) {
		vfio_pci_disable_irq_index(dev, VFIO_PCI_INTX_IRQ_INDEX);
		close(dev->irq_fd);
		dev->irq_fd = -1;
	}

	dev->intx_quit = 0;
}

static int vfio_pci_irq_init(struct lkl_pci_dev *dev, int irq)
{
	dev->irq = irq;
	return vfio_pci_start_intx(dev);
}

static unsigned int vfio_pci_msi_index(int type)
{
	return type == LKL_PCI_IRQ_MSIX ? VFIO_PCI_MSIX_IRQ_INDEX :
					  VFIO_PCI_MSI_IRQ_INDEX;
}

static void vfio_msi_thread(void *_dev)
{
	struct lkl_pci_dev *dev = (struct lkl_pci_dev *)_dev;
	struct pollfd *pfds;
	int *vectors;
	int active = 0;
	int i;

	for (i = 0; i < dev->msi_nvec; i++)
		if (dev->msi_vectors[i].fd >= 0)
			active++;

	pfds = calloc(active, sizeof(*pfds));
	vectors = calloc(active, sizeof(*vectors));
	if (!pfds || !vectors) {
		dev->thread_init_status = -1;
		lkl_host_ops.sem_up(dev->thread_init_sem);
		free(pfds);
		free(vectors);
		return;
	}

	for (i = 0, active = 0; i < dev->msi_nvec; i++) {
		if (dev->msi_vectors[i].fd < 0)
			continue;
		pfds[active].fd = dev->msi_vectors[i].fd;
		pfds[active].events = POLLIN;
		vectors[active] = i;
		active++;
	}

	dev->thread_init_status = 0;
	lkl_host_ops.sem_up(dev->thread_init_sem);

	while (!dev->msi_quit) {
		int rc = poll(pfds, active, 100);

		if (rc < 0)
			break;
		if (!rc)
			continue;

		for (i = 0; i < active; i++) {
			eventfd_t icount;
			int vector;

			if (!(pfds[i].revents & POLLIN))
				continue;

			if (read(pfds[i].fd, &icount, sizeof(icount)) < 0)
				continue;

			vector = vectors[i];
			lkl_trigger_irq(dev->msi_vectors[vector].irq);
		}
	}

	free(pfds);
	free(vectors);
}

static void vfio_pci_free_msi_vectors(struct lkl_pci_dev *dev)
{
	int i;

	if (!dev->msi_vectors)
		return;

	for (i = 0; i < dev->msi_nvec; i++) {
		if (dev->msi_vectors[i].fd >= 0)
			close(dev->msi_vectors[i].fd);
	}

	free(dev->msi_vectors);
	dev->msi_vectors = NULL;
	dev->msi_nvec = 0;
	dev->msi_type = 0;
}

static void vfio_pci_stop_msi(struct lkl_pci_dev *dev, int restart_intx)
{
	if (!dev->msi_vectors)
		return;

	if (dev->msi_thread_running) {
		dev->msi_quit = 1;
		lkl_host_ops.thread_join(dev->msi_thread);
		dev->msi_thread = 0;
		dev->msi_thread_running = 0;
	}

	vfio_pci_disable_irq_index(dev, vfio_pci_msi_index(dev->msi_type));
	vfio_pci_free_msi_vectors(dev);
	dev->msi_quit = 0;

	if (restart_intx && dev->irq > 0)
		vfio_pci_start_intx(dev);
}

static int vfio_pci_start_msi_thread(struct lkl_pci_dev *dev)
{
	dev->thread_init_sem = lkl_host_ops.sem_alloc(0);
	if (!dev->thread_init_sem)
		return -1;

	dev->thread_init_status = -1;
	dev->msi_quit = 0;

	dev->msi_thread = lkl_host_ops.thread_create(vfio_msi_thread,
						    (void *)dev);
	if (!dev->msi_thread) {
		lkl_host_ops.sem_free(dev->thread_init_sem);
		dev->thread_init_sem = NULL;
		return -1;
	}

	lkl_host_ops.sem_down(dev->thread_init_sem);
	lkl_host_ops.sem_free(dev->thread_init_sem);
	dev->thread_init_sem = NULL;

	if (dev->thread_init_status < 0) {
		lkl_host_ops.thread_join(dev->msi_thread);
		dev->msi_thread = 0;
		return -1;
	}

	dev->msi_thread_running = 1;
	return 0;
}

static int vfio_pci_msi_init(struct lkl_pci_dev *dev, int type, int nvec,
			     int *irqs)
{
	unsigned int index = vfio_pci_msi_index(type);
	struct vfio_irq_info irq = { .argsz = sizeof(irq), .index = index };
	struct vfio_irq_set *irq_set = NULL;
	int *fds = NULL;
	int active = 0;
	int ret = -1;
	int i;

	if (type != LKL_PCI_IRQ_MSI && type != LKL_PCI_IRQ_MSIX)
		return -1;
	if (nvec <= 0 || dev->msi_vectors)
		return -1;
	if (dev->device_info.num_irqs <= index)
		return -1;
	if (ioctl(dev->fd, VFIO_DEVICE_GET_IRQ_INFO, &irq))
		return -1;

	if (!(irq.flags & VFIO_IRQ_INFO_EVENTFD) || irq.count < (unsigned int)nvec)
		return -1;

	dev->msi_vectors = calloc(nvec, sizeof(*dev->msi_vectors));
	fds = malloc(sizeof(*fds) * nvec);
	irq_set = malloc(sizeof(*irq_set) + sizeof(*fds) * nvec);
	if (!dev->msi_vectors || !fds || !irq_set)
		goto out_free;
	dev->msi_nvec = nvec;

	for (i = 0; i < nvec; i++) {
		dev->msi_vectors[i].irq = irqs[i];
		dev->msi_vectors[i].fd = -1;
		fds[i] = -1;

		if (irqs[i] <= 0)
			continue;

		dev->msi_vectors[i].fd = eventfd(0, EFD_CLOEXEC);
		if (dev->msi_vectors[i].fd < 0)
			goto out_free;

		fds[i] = dev->msi_vectors[i].fd;
		active++;
	}

	if (!active)
		goto out_free;

	vfio_pci_stop_intx(dev);

	irq_set->argsz = sizeof(*irq_set) + sizeof(*fds) * nvec;
	irq_set->flags =
		VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
	irq_set->index = index;
	irq_set->start = 0;
	irq_set->count = nvec;
	memcpy(irq_set->data, fds, sizeof(*fds) * nvec);

	if (ioctl(dev->fd, VFIO_DEVICE_SET_IRQS, irq_set))
		goto out_restart_intx;

	dev->msi_type = type;

	if (vfio_pci_start_msi_thread(dev))
		goto out_disable_msi;

	ret = 0;
	goto out;

out_disable_msi:
	vfio_pci_disable_irq_index(dev, index);
out_restart_intx:
	if (dev->irq > 0)
		vfio_pci_start_intx(dev);
out_free:
	vfio_pci_free_msi_vectors(dev);
out:
	free(irq_set);
	free(fds);
	return ret;
}

static void vfio_pci_msi_teardown(struct lkl_pci_dev *dev, int type)
{
	if (!dev->msi_vectors || dev->msi_type != type)
		return;

	vfio_pci_stop_msi(dev, 1);
}

static void vfio_pci_remove(struct lkl_pci_dev *dev)
{
	vfio_pci_stop_msi(dev, 0);
	vfio_pci_stop_intx(dev);
	close(dev->fd);
	free(dev);
}

static unsigned long long vfio_map_page(struct lkl_pci_dev *dev, void *vaddr,
					lkl_ulong_t size)
{
	return (unsigned long long)vaddr - dev->dma_map.vaddr;
}

static void vfio_unmap_page(struct lkl_pci_dev *dev,
			    unsigned long long dma_handle, lkl_ulong_t size)
{
}

struct vfio_pci_resource {
	struct lkl_pci_dev *dev;
	void *mmap_addr;
	unsigned long size;
	unsigned long offset;
};

static int vfio_pci_read(struct lkl_pci_dev *dev, int where, int size,
			 void *val)
{
	return pread(dev->fd, val, size, dev->config_reg.offset + where);
}

static int vfio_pci_write(struct lkl_pci_dev *dev, int where, int size,
			  void *val)
{
	return pwrite(dev->fd, val, size, dev->config_reg.offset + where);
}

static int pci_resource_read(void *data, int offset, void *res, int size)
{
	struct vfio_pci_resource *resource = data;
	unsigned long end = (unsigned long)offset + (unsigned long)size;
	void *addr;

	if (offset < 0 || size <= 0)
		return -LKL_EINVAL;
	if (end > resource->size)
		return -LKL_EINVAL;

	if (!resource->mmap_addr)
		return pread(resource->dev->fd, res, size,
			     resource->offset + offset) == size ? 0 :
								   -LKL_EIO;

	addr = resource->mmap_addr + offset;

	switch (size) {
	case 8:
		*(uint64_t *)res = *(uint64_t *)addr;
		break;
	case 4:
		*(uint32_t *)res = *(uint32_t *)addr;
		break;
	case 2:
		*(uint16_t *)res = *(uint16_t *)addr;
		break;
	case 1:
		*(uint8_t *)res = *(uint8_t *)addr;
		break;
	default:
		return -LKL_EOPNOTSUPP;
	}
	return 0;
}

static int pci_resource_write(void *data, int offset, void *res, int size)
{
	struct vfio_pci_resource *resource = data;
	unsigned long end = (unsigned long)offset + (unsigned long)size;
	void *addr;

	if (offset < 0 || size <= 0)
		return -LKL_EINVAL;
	if (end > resource->size)
		return -LKL_EINVAL;

	if (!resource->mmap_addr)
		return pwrite(resource->dev->fd, res, size,
			      resource->offset + offset) == size ? 0 :
								    -LKL_EIO;

	addr = resource->mmap_addr + offset;

	switch (size) {
	case 8:
		*(uint64_t *)addr = *(uint64_t *)res;
		break;
	case 4:
		*(uint32_t *)addr = *(uint32_t *)res;
		break;
	case 2:
		*(uint16_t *)addr = *(uint16_t *)res;
		break;
	case 1:
		*(uint8_t *)addr = *(uint8_t *)res;
		break;
	default:
		return -LKL_EOPNOTSUPP;
	}
	return 0;
}

static const struct lkl_iomem_ops pci_resource_ops = {
	.read = pci_resource_read,
	.write = pci_resource_write,
};

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

static void *vfio_resource_alloc(struct lkl_pci_dev *dev,
				 lkl_ulong_t resource_size,
				 int resource_index)
{
	unsigned int region_index_list[] = {
		VFIO_PCI_BAR0_REGION_INDEX, VFIO_PCI_BAR1_REGION_INDEX,
		VFIO_PCI_BAR2_REGION_INDEX, VFIO_PCI_BAR3_REGION_INDEX,
		VFIO_PCI_BAR4_REGION_INDEX, VFIO_PCI_BAR5_REGION_INDEX,
	};
	struct vfio_region_info reg = { .argsz = sizeof(reg) };
	struct vfio_pci_resource *resource;
	void *mmio_addr;

	if ((unsigned int)resource_index >= ARRAY_SIZE(region_index_list))
		return NULL;

	reg.index = region_index_list[resource_index];

	if (dev->device_info.num_regions <= reg.index)
		return NULL;

	/* We assume the resource is a memory space. */

	if (ioctl(dev->fd, VFIO_DEVICE_GET_REGION_INFO, &reg) < 0)
		return NULL;

	if (reg.size < resource_size)
		return NULL;

	resource = malloc(sizeof(*resource));
	if (!resource)
		return NULL;

	mmio_addr = mmap(NULL, resource_size, PROT_READ | PROT_WRITE,
			 MAP_SHARED, dev->fd, reg.offset);
	/*
	 * A resource can still be accessed through pread/pwrite if mmap() fails.
	 * Store NULL in mmap_addr so the access callbacks fall back to
	 * pread()/pwrite(). mmio_addr is then reused for the address returned by
	 * register_iomem().
	 */
	if (mmio_addr == MAP_FAILED)
		mmio_addr = NULL;

	resource->dev = dev;
	resource->mmap_addr = mmio_addr;
	resource->size = resource_size;
	resource->offset = reg.offset;

	mmio_addr = register_iomem(resource, resource_size, &pci_resource_ops);
	if (!mmio_addr) {
		if (resource->mmap_addr)
			munmap(resource->mmap_addr, resource_size);
		free(resource);
	}

	return mmio_addr;
}

struct lkl_dev_pci_ops vfio_pci_ops = {
	.add = vfio_pci_add,
	.remove = vfio_pci_remove,
	.irq_init = vfio_pci_irq_init,
	.msi_init = vfio_pci_msi_init,
	.msi_teardown = vfio_pci_msi_teardown,
	.read = vfio_pci_read,
	.write = vfio_pci_write,
	.resource_alloc = vfio_resource_alloc,
	.map_page = vfio_map_page,
	.unmap_page = vfio_unmap_page,
};
