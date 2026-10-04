#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Win64 compiler shim, used as x86_64-w64-mingw32-gcc through the links in
# tools/lkl/bin, which is in PATH for the kernel build. The kernel must be
# built for LP64 (long is 64 bit) while Win64 is LLP64 (long is 32 bit), so:
#
# - kernel code and any link is routed to x86_64-pc-cygwin-gcc: same MS x64
#   calling convention and PE/COFF output as MinGW, but LP64; MinGW ld also
#   overflows IMAGE_REL_AMD64_REL32 relocations on the large kernel object
# - user space code is compiled with the real x86_64-w64-mingw32-gcc
#
# The LKL API uses lkl_long_t (always 64 bit) to bridge the difference.

LKL_BIN_DIR="$(cd "$(dirname "$0")" && pwd)"

for arg in "$@"; do
	if [[ "$arg" == "-D__KERNEL__" || "$arg" == *lkl.o ]]; then
		exec x86_64-pc-cygwin-gcc "$@"
	fi
done

case " $* " in
*" -c "*|*" -S "*|*" -E "*) ;;
*) exec x86_64-pc-cygwin-gcc "$@" ;;
esac

# Find the real MinGW gcc, skipping this shim
for gcc in $(which -a x86_64-w64-mingw32-gcc); do
	if [[ "$(cd "$(dirname "$gcc")" && pwd)" != "$LKL_BIN_DIR" ]]; then
		exec "$gcc" "$@"
	fi
done

echo "$0: x86_64-w64-mingw32-gcc not found" >&2
exit 1
