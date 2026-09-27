#!/bin/bash
# Bootstrap: builds cadthumb-thumbnailer if needed, then installs it and registers it with your file
# manager. Safe to re-run (e.g. to add --stl later, or to promote a per-user install to --system).
#
#   ./install.sh              for the current user, STEP + 3MF
#   ./install.sh --stl        also .stl
#   ./install.sh --system     for all users (asks for sudo)
#
# All the actual work (and the `cadthumb` command you'll use afterwards for settings/status/uninstall,
# since there is no tray icon here) lives in bin/cadthumb -- this script just makes sure a build
# exists first.
set -e
cd "$(dirname "$(readlink -f "$0")")"

if [ ! -x build/cadthumb-thumbnailer ]; then
    echo "Building cadthumb-thumbnailer (first time only)..."
    command -v cmake >/dev/null || {
        echo "cmake not found. Install build dependencies first, e.g. on Debian/Ubuntu:" >&2
        echo "  sudo apt install cmake g++ pkg-config libpugixml-dev libocct-data-exchange-dev" >&2
        exit 1
    }
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j"$(nproc)"
fi

exec bin/cadthumb install "$@"
