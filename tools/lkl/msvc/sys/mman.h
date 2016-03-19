/* hack header to compile fixdep etc */

#ifndef __MSVC_HACK_SYS_MMAN_H
#define __MSVC_HACK_SYS_MMAN_H

#define PROT_READ
#define MAP_PRIVATE

void  *mmap(void *, size_t, int, int, int, off_t);
int    munmap(void *, size_t);

#endif /* __MSVC_HACK_SYS_MMAN_H */
