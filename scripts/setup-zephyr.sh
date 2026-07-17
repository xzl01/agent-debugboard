#!/bin/sh
set -eu

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

info() { echo "[setup-zephyr] $*"; }

if [ -d "$REPO_ROOT/.west" ]; then
  info ".west/ already exists, skipping west init"
else
  info "Initializing west workspace ..."
  west init -l "$REPO_ROOT"
fi

info "west update ..."
west update

info "west zephyr-export ..."
west zephyr-export

info "Done.  Build with:"
info "  nix-shell --run 'west build -p always -b rpi_pico2/rp2350a/m33/mcuboot --sysbuild apps/radxa_linkr_debugger -d build/radxa_linkr_debugger'"
