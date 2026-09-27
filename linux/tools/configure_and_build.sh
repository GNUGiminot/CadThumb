#!/bin/bash
set -e
cd "$(dirname "$0")"
. ./env.sh
export CADTHUMB_TEST_SYSROOT="$SYSROOT"
cd ..
rm -rf build-linux
# CPATH/LIBRARY_PATH would inject the sysroot a second time as plain -I/-L entries, which breaks the
# #include_next chain the C++ standard headers rely on once --sysroot is already redirecting GCC's own
# default search paths. Explicit -I/-L (OpenCASCADE, miniz, stb, pkg-config) stay unaffected below.
unset CPATH LIBRARY_PATH
# --sysroot: needed so ld resolves libc.so's internal linker-script GROUP() paths (e.g.
# libc_nonshared.a) against our rootless sysroot instead of the real (dev-package-less) /usr.
# Extra -B on the real /lib, /usr/lib: --sysroot would otherwise also hide libm.so.6 etc., which this
# WSL image genuinely has installed for real (only the -dev/static packages were never sudo-installed).
FLAGS="--sysroot=$SYSROOT"
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="$FLAGS" -DCMAKE_EXE_LINKER_FLAGS="$FLAGS"
cmake --build build-linux -j"$(nproc)"
