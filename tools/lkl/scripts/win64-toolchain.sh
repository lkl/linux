#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Install a Linux hosted toolchain for Win64 (x86_64-w64-mingw32) builds:
#
# - x86_64-pc-cygwin-gcc (kernel, LP64) and x86_64-w64-mingw32-gcc (user
#   space, LLP64) from msys2-cross (https://github.com/xdqi/msys-cross)
# - binutils with the NT weak externals fixes, see binutils-build.sh
#
# Then build with patched binutils first in PATH:
#
#   PATH=$BINUTILS_PREFIX/bin:$MSYS_CROSS_PREFIX/bin:$PATH \
#     make -C tools/lkl CROSS_COMPILE=x86_64-w64-mingw32-
#
# When running in GitHub Actions the directories are added to $GITHUB_PATH.

set -e

release=${MSYS_CROSS_RELEASE:-build-20261001.1}
url=https://github.com/xdqi/msys-cross/releases/download/$release
prefix=${MSYS_CROSS_PREFIX:-/opt/msys2-cross}
binutils_prefix=${BINUTILS_PREFIX:-$HOME/lkl-binutils}
script_dir=$(cd "$(dirname "$0")" && pwd)

if ! mkdir -p "$prefix" 2>/dev/null; then
	sudo mkdir -p "$prefix"
	sudo chown "$(id -u):$(id -g)" "$prefix"
fi

if [ ! -x "$prefix/bin/msys-pacman" ]; then
	curl -fsSL --retry 3 $url/bootstrap.tar.xz |
		tar -xJ -C "$prefix" --strip-components=1
fi

# Serve the package database locally and download the packages from the
# release assets: pacman tries the servers in order for each file.
repo=$prefix/var/lib/msys-cross-repo
mkdir -p "$repo"
curl -fsSL --retry 3 -o "$repo/msys-cross.db" $url/msys-cross.db.tar.gz
sed -i -e '/^\[msys-cross\]/,/^\[/ { /^Server = /d }' \
	-e "/^\[msys-cross\]/a Server = file://$repo\nServer = $url" \
	"$prefix/etc/pacman.d/pacman.conf"

"$prefix/bin/msys-pacman" -Sy --noconfirm --needed \
	msys-cross-cygwin-gcc msys-cross-mingw64-gcc

if [ ! -x "$binutils_prefix/bin/x86_64-w64-mingw32-ld" ]; then
	BINUTILS_PREFIX=$binutils_prefix "$script_dir/binutils-build.sh" \
		x86_64-w64-mingw32
fi

# Entries added later take precedence
if [ -n "$GITHUB_PATH" ]; then
	echo "$prefix/bin" >> "$GITHUB_PATH"
	echo "$binutils_prefix/bin" >> "$GITHUB_PATH"
fi
