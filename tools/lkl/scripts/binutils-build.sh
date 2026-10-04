#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Build GNU binutils with the PE/COFF NT weak externals fixes needed to
# link LKL for Windows hosts. The patches live in binutils-patches/.
#
# usage: binutils-build.sh [target...]
#
# The default target is x86_64-w64-mingw32. The tools are installed in
# $BINUTILS_PREFIX (default: $HOME/lkl-binutils); put $BINUTILS_PREFIX/bin
# in PATH before building LKL.

set -e

version=2.47
sha256=154ab23b60070e8f27013c22977f1129425d67d1e8acd6e13010e617811e4cff
mirror=${GNU_MIRROR:-https://ftpmirror.gnu.org/gnu}

patches=$(cd "$(dirname "$0")/binutils-patches" && pwd)
prefix=${BINUTILS_PREFIX:-$HOME/lkl-binutils}
targets=${*:-x86_64-w64-mingw32}
jobs=$(nproc 2>/dev/null || echo 4)

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cd "$tmp"

curl -fsSL --retry 3 -o binutils-$version.tar.xz \
	$mirror/binutils/binutils-$version.tar.xz
echo "$sha256  binutils-$version.tar.xz" | sha256sum -c -
tar -xf binutils-$version.tar.xz

for p in "$patches"/*.patch; do
	patch -d binutils-$version -p1 < "$p"
done

for target in $targets; do
	mkdir build-$target
	cd build-$target
	../binutils-$version/configure --target=$target --prefix="$prefix" \
		--disable-nls --disable-werror --disable-gdb --disable-gdbserver \
		--disable-sim --disable-gprof --disable-gprofng
	make -j$jobs all-gas all-ld all-binutils
	make install-gas install-ld install-binutils
	cd ..
done
