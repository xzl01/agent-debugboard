[中文](quickstart.zh-CN.md)

# Quick Start

Get the board up and running in 5 minutes.

## 1. Install the CLI

Download the archive for your OS from
[GitHub Releases](https://github.com/xzl01/agent-debugboard/releases):

| OS | Archive |
|----|---------|
| Windows x64 | `radxa-linkr-debuggerctl-rust_windows_amd64.zip` |
| Linux x64 / AMD64 | `radxa-linkr-debuggerctl-rust_linux_amd64.tar.gz` |
| Linux ARM64 / AArch64 | `radxa-linkr-debuggerctl-rust_linux_arm64.tar.gz` |
| macOS Apple Silicon | `radxa-linkr-debuggerctl-rust_darwin_arm64.tar.gz` |

Each archive contains `radxa-linkr-debuggerctl` and its short `rdb` alias.
Extract both names and put them somewhere on your PATH:

```sh
# Linux / macOS
sudo install -m 0755 ./radxa-linkr-debuggerctl /usr/local/bin/
sudo ln -sfn radxa-linkr-debuggerctl /usr/local/bin/rdb

# macOS — if Gatekeeper complains
xattr -dr com.apple.quarantine ./radxa-linkr-debuggerctl
```

Verify:

```sh
radxa-linkr-debuggerctl --version
rdb --version
```

Full install guide: [Install CLI](install.md)

## 2. Connect the board

Plug the USB-C cable into the board and your PC. The board enumerates as a
composite USB device — no driver install needed on Linux or macOS.

Wait a few seconds for the network interface to come up. The host NCM address is
`.2` of the board `/30`; set the board URL before running the examples:

```sh
BOARD_URL=http://<board-ip>   # host NCM address minus one
```

Then check:

```sh
radxa-linkr-debuggerctl --url "$BOARD_URL" doctor
```

You should see all checks pass. If `doctor` reports connection issues, see
[Install CLI](install.md) or [Board Overview](board-overview.md).

## 3. Open the TUI

Run without arguments:

```sh
radxa-linkr-debuggerctl --url "$BOARD_URL"
```

The interactive terminal UI opens on the Controls page. Its borderless table
shows one power, switch, or GPIO object per row with a full-row selection; live
current rows appear above it only when data exists, and the last row contains
htop-style key/action blocks. Use the arrow keys to choose `5v_out`, then press
Space. Tab and Shift+Tab move between Controls, Saved Config, and Status.
The first press opens the red-bordered confirmation box without changing the
rail; press Space again within three seconds to turn it on.

More: [TUI Guide](tui.md)

## 4. Open the Web UI

The host NCM address is `.2` of the board's private `/30`; the board is `.1`.
Read the host address with `ip -brief addr show` (Linux) and open the board at
`http://<host-address-minus-1>/`, or read the URL from the CDC shell `ip`
command. For example, a host `10.20.30.2/30` uses `http://10.20.30.1/`. On most systems
the board's captive-portal detection will prompt the browser to open
automatically. If not, navigate to the URL printed by `ip` manually.

The dashboard shows power controls, ADC readings, switch routes, and GPIO.
Try it: click the `5v_out` toggle in the Power Controls card, then watch the
ADC card update with the current reading. Switch to the **Terminal** workspace
to find the logic analyzer and the auto-connected xterm.js J4 console. The
embedded terminal uses the MCU `serial.j4` WebSocket; it does not open a host
serial device or display a browser chooser. Raw clients can alternatively use
`nc <board-ip> 5555`.

More: [Web UI Guide](webui.md)

## 5. Try a few commands

```sh
# Board status
radxa-linkr-debuggerctl --url "$BOARD_URL" status
# → shows power states, switch routes, GPIO values, uptime

# Power on the target
radxa-linkr-debuggerctl --url "$BOARD_URL" power set 5v_out on
# → "5v_out: on"

# Read current draw
radxa-linkr-debuggerctl --url "$BOARD_URL" adc read
# → "5v_out=0.123000A  12v_out=0.000000A  20v_out=0.000000A"

# Route SD card to host for reading
radxa-linkr-debuggerctl --url "$BOARD_URL" switch route sd usb-reader
# → SD card now appears as USB storage on your PC

# Drive a GPIO high
radxa-linkr-debuggerctl --url "$BOARD_URL" gpio set GP13 1
# → "GP13: 1 (output)"
```

## What's next

- [CLI Reference](cli.md) — all subcommands
- [HTTP API Reference](api.md) — for scripting and automation
- [Logic Analyzer](logic-analyzer.md) — high-speed GPIO capture
- [Power Analyzer](power-analyzer.md) — triggered current capture
