#!/bin/bash
# Environment for building against the rootless ~/sysroot (extracted .deb packages, no sudo used).
export SYSROOT="$HOME/sysroot"
export PATH="$SYSROOT/usr/bin:$SYSROOT/usr/lib/cmake:$PATH"
export LD_LIBRARY_PATH="$SYSROOT/usr/lib/x86_64-linux-gnu:$SYSROOT/lib/x86_64-linux-gnu:$SYSROOT/usr/lib:$LD_LIBRARY_PATH"
export PKG_CONFIG_PATH="$SYSROOT/usr/lib/x86_64-linux-gnu/pkgconfig:$SYSROOT/usr/share/pkgconfig:$PKG_CONFIG_PATH"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export CMAKE_PREFIX_PATH="$SYSROOT/usr"
export CPATH="$SYSROOT/usr/include:$SYSROOT/usr/include/x86_64-linux-gnu"
export LIBRARY_PATH="$SYSROOT/usr/lib/x86_64-linux-gnu:$SYSROOT/lib/x86_64-linux-gnu"
