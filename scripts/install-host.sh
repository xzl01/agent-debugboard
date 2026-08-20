#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later

set -eu

DRY_RUN=0
START_NOW=1
AUTOSTART=1
PREFIX=""

usage() {
  cat <<'USAGE'
Usage: scripts/install-host.sh [--prefix DIR] [--no-start] [--no-autostart] [--dry-run]

Builds and installs one per-user Radxa Linkr desktop stack:
  - linkr-tray: login item, status icon and service supervisor
  - linkr-host: Web UI, board gateway, Serial Broker and HTTP MCP endpoint
  - radxa-linkr-debuggerctl: CLI/TUI

The installer writes only below the selected per-user prefix and the current
user's login autostart directory. It never uses sudo or modifies PATH.
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --prefix)
      [ "$#" -ge 2 ] || { echo "--prefix requires a value" >&2; exit 2; }
      PREFIX="$2"
      shift 2
      ;;
    --no-start)
      START_NOW=0
      shift
      ;;
    --no-autostart)
      AUTOSTART=0
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

unset CDPATH
script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
if [ -f "$script_dir/../host-tools/Cargo.toml" ]; then
  repo_root=$(CDPATH='' cd -- "$script_dir/.." && pwd)
else
  repo_root="$script_dir"
fi
os=$(uname -s)

if [ -z "$PREFIX" ]; then
  case "$os" in
    Darwin)
      PREFIX="${HOME:?}/Library/Application Support/Radxa Linkr Debugger"
      ;;
    Linux)
      PREFIX="${XDG_DATA_HOME:-${HOME:?}/.local/share}/radxa-linkr-debugger"
      ;;
    *)
      echo "unsupported OS: $os (use install-host.ps1 on Windows)" >&2
      exit 1
      ;;
  esac
fi

bin_dir="$PREFIX/bin"
web_dir="$PREFIX/share/radxa-linkr-debugger/web"
mcp_info="$PREFIX/mcp-endpoint.json"
case "$os" in
  Darwin)
    autostart_path="${HOME:?}/Library/LaunchAgents/com.radxa.linkr-debugger.plist"
    tray_lock="${HOME:?}/Library/Application Support/Radxa Linkr Debugger/tray.lock"
    ;;
  Linux)
    autostart_path="${XDG_CONFIG_HOME:-${HOME:?}/.config}/autostart/radxa-linkr-debugger.desktop"
    tray_lock="${XDG_DATA_HOME:-${HOME:?}/.local/share}/Radxa Linkr Debugger/tray.lock"
    ;;
esac

if [ "$DRY_RUN" -eq 1 ]; then
  cat <<EOF
Radxa Linkr unified installer dry-run
source:       $repo_root
install root: $PREFIX
tray:         $bin_dir/linkr-tray
host:         $bin_dir/linkr-host
CLI:          $bin_dir/radxa-linkr-debuggerctl
Web assets:   $web_dir
MCP endpoint: http://127.0.0.1:8787/mcp
UART archive: enabled by tray (64 MiB segments, 2 GiB quota, 30 days)
autostart:    $(if [ "$AUTOSTART" -eq 1 ]; then echo "$autostart_path"; else echo disabled; fi)
start now:    $(if [ "$START_NOW" -eq 1 ]; then echo yes; else echo no; fi)
EOF
  exit 0
fi

if [ -f "$repo_root/host-tools/Cargo.toml" ]; then
  command -v npm >/dev/null 2>&1 || { echo "npm is required" >&2; exit 1; }
  command -v cargo >/dev/null 2>&1 || { echo "cargo is required" >&2; exit 1; }
  echo "Building Web UI"
  npm --prefix "$repo_root/web" ci
  npm --prefix "$repo_root/web" run build

  echo "Building Linkr Host, tray and CLI"
  cargo build --locked --release --manifest-path "$repo_root/host-tools/Cargo.toml"
  cargo build --locked --release --manifest-path "$repo_root/cmd-ng/Cargo.toml"
  source_bin="$repo_root/host-tools/target/release"
  source_cli="$repo_root/cmd-ng/target/release/radxa-linkr-debuggerctl"
  source_web="$repo_root/web/dist"
elif [ -f "$repo_root/bin/linkr-host" ] && [ -f "$repo_root/bin/linkr-tray" ]; then
  source_bin="$repo_root/bin"
  source_cli="$repo_root/bin/radxa-linkr-debuggerctl"
  source_web="$repo_root/share/radxa-linkr-debugger/web"
else
  echo "installer is neither inside a source checkout nor a desktop release bundle" >&2
  exit 1
fi

stop_existing_tray() {
  [ -r "$tray_lock" ] || return 0
  tray_pid=$(sed -n '1{s/[^0-9].*$//;p;}' "$tray_lock")
  if [ -z "$tray_pid" ] && command -v lsof >/dev/null 2>&1; then
    tray_pid=$(lsof -t "$tray_lock" 2>/dev/null | sed -n '1p')
  fi
  if [ -z "$tray_pid" ] && command -v fuser >/dev/null 2>&1; then
    tray_pid=$(fuser "$tray_lock" 2>/dev/null | awk '{print $1}')
  fi
  [ -n "$tray_pid" ] || return 0
  case "$tray_pid" in
    *[!0-9]*) return 0 ;;
  esac
  kill -0 "$tray_pid" 2>/dev/null || return 0
  tray_name=$(ps -p "$tray_pid" -o comm= 2>/dev/null | sed 's,.*/,,')
  [ "$tray_name" = "linkr-tray" ] || {
    echo "refusing to stop PID $tray_pid from stale tray lock ($tray_name)" >&2
    return 1
  }
  echo "Stopping the existing Radxa Linkr tray"
  kill -TERM "$tray_pid"
  attempts=0
  while kill -0 "$tray_pid" 2>/dev/null && [ "$attempts" -lt 120 ]; do
    sleep 0.1
    attempts=$((attempts + 1))
  done
  if kill -0 "$tray_pid" 2>/dev/null; then
    echo "existing tray did not stop cleanly; installation aborted" >&2
    return 1
  fi
}

stop_existing_tray

mkdir -p "$bin_dir" "$web_dir"
install -m 0755 "$source_bin/linkr-host" "$bin_dir/linkr-host"
install -m 0755 "$source_bin/linkr-tray" "$bin_dir/linkr-tray"
install -m 0755 "$source_cli" "$bin_dir/radxa-linkr-debuggerctl"
cp -R "$source_web/." "$web_dir/"

cat > "$mcp_info" <<EOF
{
  "name": "radxa-linkr-debugger",
  "transport": "streamable-http",
  "url": "http://127.0.0.1:8787/mcp",
  "stdio_compatibility": {
    "command": "$bin_dir/linkr-host",
    "args": ["mcp", "--no-autostart"]
  }
}
EOF

if [ "$os" = "Darwin" ]; then
  xattr -dr com.apple.quarantine "$PREFIX" 2>/dev/null || true
fi

if [ "$AUTOSTART" -eq 1 ]; then
  mkdir -p "$(dirname "$autostart_path")"
  if [ "$os" = "Darwin" ]; then
    cat > "$autostart_path" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>com.radxa.linkr-debugger</string>
  <key>ProgramArguments</key><array><string>$bin_dir/linkr-tray</string></array>
  <key>RunAtLoad</key><true/>
  <key>ProcessType</key><string>Interactive</string>
</dict></plist>
EOF
  else
    cat > "$autostart_path" <<EOF
[Desktop Entry]
Type=Application
Name=Radxa Linkr Debugger
Comment=Run the Web, Serial Broker and MCP host with tray status
Exec=$bin_dir/linkr-tray
Terminal=false
X-GNOME-Autostart-enabled=true
EOF
  fi
fi

if [ "$START_NOW" -eq 1 ]; then
  if [ "$os" = "Darwin" ] && [ "$AUTOSTART" -eq 1 ]; then
    launchctl bootout "gui/$(id -u)" "$autostart_path" >/dev/null 2>&1 || true
    launchctl bootstrap "gui/$(id -u)" "$autostart_path"
  else
    nohup "$bin_dir/linkr-tray" >"$PREFIX/tray-launch.log" 2>&1 &
  fi
fi

echo "Installed Radxa Linkr desktop stack to $PREFIX"
echo "Web console: http://127.0.0.1:8787/"
echo "MCP endpoint: http://127.0.0.1:8787/mcp"
echo "UART archive: enabled by tray; manage it from the Web serial console"
echo "CLI: $bin_dir/radxa-linkr-debuggerctl"
