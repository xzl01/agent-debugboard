#!/bin/sh
set -eu

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORKSPACE="${LINKR_ZEPHYR_WORKSPACE:-$REPO_ROOT/.zephyr-workspace}"

if [ ! -d "$WORKSPACE/.west" ] || \
   ! cmp -s "$REPO_ROOT/west.yml" "$WORKSPACE/manifest/west.yml"; then
  "$REPO_ROOT/scripts/setup-zephyr.sh"
fi

export ZEPHYR_BASE="$WORKSPACE/zephyr"

cd "$WORKSPACE"
west build \
  -p always \
  -b rpi_pico2/rp2350a/m33/mcuboot \
  --sysbuild "$REPO_ROOT/apps/radxa_linkr_debugger" \
  -d "$REPO_ROOT/build/radxa_linkr_debugger"
