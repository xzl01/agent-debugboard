#!/bin/sh
set -eu

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORKSPACE="${LINKR_ZEPHYR_WORKSPACE:-$REPO_ROOT/.zephyr-workspace}"
MANIFEST_DIR="$WORKSPACE/manifest"

info() { echo "[setup-zephyr] $*"; }
fail() {
  echo "[setup-zephyr] ERROR: $*" >&2
  exit 1
}

if [ -e "$WORKSPACE" ] && [ ! -d "$WORKSPACE" ]; then
  fail "$WORKSPACE exists but is not a directory"
fi

mkdir -p "$MANIFEST_DIR"
cp "$REPO_ROOT/west.yml" "$MANIFEST_DIR/west.yml"

if [ ! -d "$MANIFEST_DIR/.git" ]; then
  git -C "$MANIFEST_DIR" init --quiet
fi

git -C "$MANIFEST_DIR" add west.yml
if ! git -C "$MANIFEST_DIR" diff --cached --quiet; then
  git -C "$MANIFEST_DIR" \
    -c user.name="Linkr Zephyr Workspace" \
    -c user.email="noreply@localhost" \
    commit --quiet -m "Update workspace manifest"
fi

if [ -d "$WORKSPACE/.west" ]; then
  configured_manifest="$(
    cd "$WORKSPACE"
    west config manifest.path 2>/dev/null || true
  )"
  if [ "$configured_manifest" != "manifest" ]; then
    fail "$WORKSPACE is managed by a different west manifest: $configured_manifest"
  fi
  info "Using existing workspace: $WORKSPACE"
else
  info "Initializing isolated workspace: $WORKSPACE"
  (cd "$WORKSPACE" && west init -l manifest)
fi

info "Updating the minimal project set ..."
(cd "$WORKSPACE" && west update --narrow -o=--depth=1)

info "Exporting Zephyr CMake package ..."
(cd "$WORKSPACE" && west zephyr-export)

info "Done.  Build with:"
info "  scripts/build-firmware.sh"
