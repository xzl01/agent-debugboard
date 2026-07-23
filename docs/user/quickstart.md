[中文](quickstart.zh-CN.md)

# Quick Start

Get the board up and running in 5 minutes.

## 1. Install the CLI

Download the archive for your OS from
[GitHub Releases](https://github.com/xzl01/agent-debugboard/releases):

| OS | Archive |
|----|---------|
| Windows x64 | `radxa-linkr-debuggerctl-rust_windows_amd64.zip` |
| Linux x64 | `radxa-linkr-debuggerctl-rust_linux_amd64.tar.gz` |
| macOS Apple Silicon | `radxa-linkr-debuggerctl-rust_darwin_arm64.tar.gz` |

Extract and put the binary somewhere on your PATH:

```sh
# Linux / macOS
sudo install -m 0755 ./radxa-linkr-debuggerctl /usr/local/bin/

# macOS — if Gatekeeper complains
xattr -dr com.apple.quarantine ./radxa-linkr-debuggerctl
```

Verify:

```sh
radxa-linkr-debuggerctl --version
```

Full install guide: [Install CLI](install.md)

## 2. Connect the board

Plug the USB-C cable into the board and your PC. The board enumerates as a
composite USB device — no driver install needed on Linux or macOS.

Wait a few seconds for the network interface to come up, then check:

```sh
radxa-linkr-debuggerctl doctor
```

You should see all checks pass. If `doctor` reports connection issues, see
[Install CLI](install.md) or [Board Overview](board-overview.md).

## 3. Open the TUI

Run without arguments:

```sh
radxa-linkr-debuggerctl
```

The interactive terminal UI shows power outputs, switch routes, and GPIO
status. Arrow keys to navigate, Space/Enter to toggle.

More: [TUI Guide](tui.md)

## 4. Open the Web UI

Open a browser and go to:

```
http://172.29.203.1/
```

On most systems the board's captive portal detection will prompt the browser
to open automatically. If not, navigate manually.

The dashboard shows power controls, ADC readings, switch routes, and GPIO.
The Terminal workspace has the logic analyzer and serial console.

More: [Web UI Guide](webui.md)

## 5. Try a few commands

```sh
# Board status
radxa-linkr-debuggerctl status

# Power on the target
radxa-linkr-debuggerctl power set 5v_out on

# Read current draw
radxa-linkr-debuggerctl adc read

# Route SD card to host for reading
radxa-linkr-debuggerctl switch route sd usb-reader

# Drive a GPIO high
radxa-linkr-debuggerctl gpio set GP13 1
```

## What's next

- [CLI Reference](cli.md) — all subcommands
- [HTTP API Reference](api.md) — for scripting and automation
- [Logic Analyzer](logic-analyzer.md) — high-speed GPIO capture
- [Power Analyzer](power-analyzer.md) — triggered current capture
