/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _ASM_UAPI_LKL_LONG_H
#define _ASM_UAPI_LKL_LONG_H

/*
 * lkl_long_t / lkl_ulong_t: pointer-width integer types for the
 * LKL cross-world interface (host_ops, syscall dispatch, UAPI structs).
 *
 * These must be the same size on both sides of the kernel<->host boundary:
 * - Kernel side (LP64 native GCC with -mabi=ms): long = 8 bytes
 * - Host side (LLP64 MinGW-w64): long = 4 bytes, need long long
 * - Host side (LP64 Linux/Cygwin/macOS): long = 8 bytes
 *
 * The headers_install.py script replaces all 'long' in generated user
 * headers with these types to ensure ABI compatibility across data models.
 */
#if defined(_WIN64) && !defined(__LP64__)
/* LLP64: MinGW-w64 x64 user-space */
typedef long long          lkl_long_t;
typedef unsigned long long lkl_ulong_t;
#else
/* LP64: kernel side, Linux, Cygwin, macOS, or any ILP32 system */
typedef long               lkl_long_t;
typedef unsigned long      lkl_ulong_t;
#endif

#endif /* _ASM_UAPI_LKL_LONG_H */
