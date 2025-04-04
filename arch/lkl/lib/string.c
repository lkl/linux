// SPDX-License-Identifier: GPL-2.0

#include <linux/string.h>
#include <linux/export.h>

#undef memcpy
#undef memset
#undef memmove

__visible void *memcpy(void *dest, const void *src, size_t count)
{
	return __memcpy(dest, src, count);
}
EXPORT_SYMBOL(memcpy);

__visible void *memset(void *s, int c, size_t count)
{
	return __memset(s, c, count);
}
EXPORT_SYMBOL(memset);

__visible void *memmove(void *dest, const void *src, size_t count)
{
	return __memmove(dest, src, count);
}
EXPORT_SYMBOL(memmove);
