#!/bin/bash
# ./uninstall.sh [--system] [--purge]  -- see bin/cadthumb (this is a thin wrapper for symmetry with
# install.sh; `cadthumb uninstall` works the same once installed).
set -e
cd "$(dirname "$(readlink -f "$0")")"
exec bin/cadthumb uninstall "$@"
