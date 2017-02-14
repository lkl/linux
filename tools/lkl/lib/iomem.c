#include <string.h>
#include <stdint.h>
#include <lkl_host.h>

#include "iomem.h"

#define IOMEM_OFFSET_BITS		24
#define MAX_IOMEM_REGIONS		256

#define IOMEM_ADDR_TO_INDEX(addr) \
	(((uintptr_t)addr) >> IOMEM_OFFSET_BITS)
#define IOMEM_ADDR_TO_OFFSET(addr) \
	(((uintptr_t)addr) & ((1 << IOMEM_OFFSET_BITS) - 1))
#define IOMEM_INDEX_TO_ADDR(i) \
	(void *)(uintptr_t)(i << IOMEM_OFFSET_BITS)

static struct iomem_region {
	void *data;
	int size;
	const struct lkl_iomem_ops *ops;
} iomem_regions[MAX_IOMEM_REGIONS];

void* register_iomem(void *data, int size, const struct lkl_iomem_ops *ops)
{
	int i;

	if (size > (1 << IOMEM_OFFSET_BITS) - 1)
		return NULL;

	for (i = 1; i < MAX_IOMEM_REGIONS; i++)
		if (!iomem_regions[i].ops)
			break;

	if (i >= MAX_IOMEM_REGIONS)
		return NULL;

	iomem_regions[i].data = data;
	iomem_regions[i].size = size;
	iomem_regions[i].ops = ops;
	return IOMEM_INDEX_TO_ADDR(i);
}

void unregister_iomem(void *base)
{
	unsigned int index = IOMEM_ADDR_TO_INDEX(base);

	if (index >= MAX_IOMEM_REGIONS) {
		lkl_printf("%s: invalid iomem_addr %p\n", __func__, base);
		return;
	}

	iomem_regions[index].size = 0;
	iomem_regions[index].ops = NULL;
}

void *lkl_ioremap(long addr, int size)
{
#ifdef RUMPRUN
	return (void *)addr;
#else
	int index = IOMEM_ADDR_TO_INDEX(addr);
	struct iomem_region *iomem = &iomem_regions[index];

	if (index >= MAX_IOMEM_REGIONS)
		return NULL;

	if (iomem->ops && size <= iomem->size)
		return IOMEM_INDEX_TO_ADDR(index);

	return NULL;
#endif
}

int lkl_iomem_access(const volatile void *addr, void *res, int size, int write)
{
	/* FIXME: should be transparent with platform/ */
#ifdef RUMPRUN
	uint16_t mem = (unsigned long)addr;
	int ret = 0;

	if (write) {
		if (size == 1) {
#ifdef __x86_64__
			uint8_t v = *(uint8_t *)res;

			asm volatile("outb %0, %1" :: "a"(v), "d"(mem));
#elif __arm__
#endif
			return 0;
		} else if (size == 2) {
#ifdef __x86_64__
			uint16_t v = *(uint16_t *)res;

			asm volatile("out %0, %1" :: "a"(v), "d"(mem));
#elif __arm__
#endif
			return 0;
		} else if (size == 4) {
#ifdef __x86_64__
			uint32_t v = *(uint32_t *)res;

			asm volatile("outl %0, %1" :: "a"(v), "d"(mem));
#elif __arm__
#endif
			return 0;
		} else if (size == 8) {
#ifdef __x86_64__
			lkl_printf("not implemented yet\n");
			lkl_host_ops.panic();
#elif __arm__
#endif
		} else {
			lkl_printf("not implemented yet\n");
			lkl_host_ops.panic();
		}
	} else {
		if (size == 1) {
#ifdef __x86_64__
			uint8_t v;

			asm volatile("inb %1,%0" : "=a"(v) : "d"(mem));
			*(uint8_t *)res = v;
#elif __arm__
#endif
			return 0;
		} else if (size == 2) {
#ifdef __x86_64__
			uint16_t v;

			asm volatile("in %1,%0" : "=a"(v) : "d"(mem));
			*(uint16_t *)res = v;
#elif __arm__
#endif
			return 0;
		} else if (size == 4) {
#ifdef __x86_64__
			uint32_t v;

			asm volatile("inl %1,%0" : "=a"(v) : "d"(mem));
			*(uint32_t *)res = v;
#elif __arm__
#endif
			return 0;
		} else if (size == 8) {
#ifdef __x86_64__
			lkl_printf("not implemented yet\n");
			lkl_host_ops.panic();
#elif __arm__
#endif
		} else {
			lkl_printf("not implemented yet\n");
			lkl_host_ops.panic();
		}
	}
#else  /* !RUMPRUN */
	int index = IOMEM_ADDR_TO_INDEX(addr);
	struct iomem_region *iomem = &iomem_regions[index];
	int offset = IOMEM_ADDR_TO_OFFSET(addr);
	int ret;

	if (index > MAX_IOMEM_REGIONS || !iomem_regions[index].ops ||
	    offset + size > iomem_regions[index].size)
		return -1;

	if (write)
		ret = iomem->ops->write(iomem->data, offset, res, size);
	else
		ret = iomem->ops->read(iomem->data, offset, res, size);

#endif
	return ret;
}
