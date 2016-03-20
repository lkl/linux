#!/bin/sh

# The purpose of this script is to set up Clang/C2 compilation
# under msys2. Requirements are Visual Studio 2015.1 or later
# with C/C++ and clang with microsoft codegen installed.
# First start a Visual Studio developer shell (or if on x64,
# start a x86 cross compilation shell). This will set the 
# variables VCINSTALLDIR, UniversalCRTSdkDir, UCRTVersion
# FrameworkDir, FrameworkVersion, WindowsSdkDir, 
# WindowsSDKLibVersion, WindowsLibPath, 
# WindowsSDK_ExecutablePath_x86, NETFXSDKDir ...
# from within the VS shell, start mingw32_shell.bat
# source this shell script to get all paths set up properly.
# this serves 2 purposes - make sure that the VS tools are 
# first in the path and ensure that path formats are simple
# without weird characters or spaces to avoid build errors.

export Platform=X86   # for now x64 does not work

# put a thest here for host arch. 

mkdir -p /tmp/msvc
mkdir -p /tmp/clang
mkdir -p /tmp/ucrt/{include,lib} 
mkdir -p /tmp/winsdk/{include,lib,libpath,tools} 
mkdir -p /tmp/netfxsdk

mount -o bind "${VCINSTALLDIR}" /tmp/msvc
# avoid space in clang path
mount -o bind "${VCINSTALLDIR}"/"Clang 3.7"  /tmp/clang 

mount -o bind "${UniversalCRTSdkDir}"/Lib/"${UCRTVersion}" /tmp/ucrt/lib 
mount -o bind "${UniversalCRTSdkDir}"/Include/"${UCRTVersion}" /tmp/ucrt/include 

mount -o bind "${WindowsSdkDir}"/lib/"${WindowsSDKLibVersion}"/um/x86 /tmp/winsdk/lib 
mount -o bind "${WindowsSdkDir}"/include /tmp/winsdk/include 
mount -o bind "${WindowsLibPath}" /tmp/winsdk/libpath
mount -o bind "${VS140COMNTOOLS}" /tmp/winsdk/tools

mount -o bind "${NETFXSDKDir}" /tmp/netfxsdk


export PATH="/tmp/clang/bin/x86:/tmp/clang/bin/x86/x86:/tmp/msvc/bin/amd64_x86:/tmp/msvc/bin/amd64:/tmp/msvc/vcpackages:/tmp/msvc/bin:/tmp/winsdk/tools:/mingw32/bin:/usr/bin:/usr/bin/site_perl:/usr/bin/vendor_perl:/usr/bin/core_perl"
export INCLUDE="/tmp/clang/include:/tmp/msvc/include:/tmp/ucrt/include/ucrt:/tmp/ucrt/include/um:/tmp/ucrt/include/shared:/tmp/netfxsdk/include/um:/tmp/winsdk/include/shared:/tmp/winsdk/include/um:/tmp/winsdk/include/winrt"
export LIB="/tmp/clang/lib/x86:/tmp/msvc/lib:/tmp/msvc/atlmfc/lib:/tmp/ucrt/lib/ucrt/x86:/tmp/ucrt/lib/um/x86:/tmp/netfxsdk/lib/um/x86:/tmp/winsdk/lib"
export LIBPATH="/tmp/clang/lib/x86:/tmp/msvc/lib:/tmp/msvc/atlmfc/lib:/tmp/winsdk/libpath"

export CC=clang
export HOSTCC=clang
export LD=link
export HOSTLD=link
export AR=lib
export NM="dumpbin /export"
export OBJCOPY="lib /extract"
export RANLIB=echo
export STRIP=echo

export C2FLAGS="-O2 -I/tmp/clang/include -I/tmp/msvc/include -I/tmp/ucrt/include/ucrt -I/tmp/ucrt/include/um -I/tmp/ucrt/include/shared -I/tmp/winsdk/include/um -I/tmp/winsdk/include/shared -I/tmp/netfxsdk/include/um"








