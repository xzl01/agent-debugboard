[中文](workflows.zh-CN.md)

# Common Workflows

Step-by-step guides for typical tasks.

## Power cycle a target board

```sh
# Turn off
radxa-linkr-debuggerctl power set 5v_out off

# Wait a moment for discharge
sleep 2

# Turn on
radxa-linkr-debuggerctl power set 5v_out on

# Verify
radxa-linkr-debuggerctl power list
```

For a hard reset with serial monitoring:
```sh
# Start recording serial output
# (open the embedded Web Terminal, or run: nc <board-ip> 5555)

radxa-linkr-debuggerctl power set 5v_out off
sleep 2
radxa-linkr-debuggerctl power set 5v_out on
# Watch boot output in the serial terminal
```

## Read or write the target's SD card

Route the SD card to the host, access it as a USB drive, then route it back:

```sh
# Route SD to host
radxa-linkr-debuggerctl switch route sd usb-reader

# The SD card now appears as a USB mass storage device on your PC
# Mount it, copy files, unmount it

# Route SD back to target
radxa-linkr-debuggerctl switch route sd target
```

On Linux, the card appears as `/dev/sdX`. On macOS, it mounts automatically.
Always unmount/eject before switching the route back.

## Capture startup current waveform

Use the Web UI's startup power analysis in the **Power Analysis** workspace, or
do it manually via the CLI:

```sh
# 1. Start recording ADC data
radxa-linkr-debuggerctl adc record /tmp/startup.ndjson 2000 --rate-hz 500 &

# 2. Power cycle the target
radxa-linkr-debuggerctl power set 5v_out off
sleep 2
radxa-linkr-debuggerctl power set 5v_out on

# 3. Wait for boot to complete, then stop recording (Ctrl+C)
```

The NDJSON file contains timestamped current readings for the full boot cycle.

## Capture a logic analyzer trace

Via the Web UI:

1. Open http://<board-ip>/ and go to the **Terminal** workspace.
2. Select pins (e.g. GP13, GP15) and set the sample rate.
3. Choose a trigger mode (none, rising, falling, either).
4. Click **Arm**, then trigger the event you want to capture.
5. Preview the waveform and export as CSV or PulseView `.sr`.

Via the API:

```sh
# Arm capture on GP13, 1 MHz, 256 samples, rising edge trigger
curl -X POST http://<board-ip>/api/v1/logic-analyzer \
  -d '{"selected_pins":[13],"sample_rate_hz":1000000,"post_samples":256,"trigger":"rising","trigger_pin":13}'

# Wait for capture, then retrieve
curl http://<board-ip>/api/v1/logic-analyzer/capture
```

## Update firmware via OTA

```sh
# Check current OTA state
radxa-linkr-debuggerctl ota status

# Upload the new firmware
radxa-linkr-debuggerctl ota upload radxa-linkr-debugger-rp2350-ota.bin

# Request test boot
radxa-linkr-debuggerctl ota test

# Wait for the board to come back (watchdog auto-confirms after ~16s)
# Or confirm manually:
radxa-linkr-debuggerctl ota confirm
```

Do NOT upload `.uf2` or `.elf` files. Use the `-ota.bin` release asset.

## Recover a bricked board

If the board won't respond to HTTP or serial:

1. Unplug USB.
2. Hold the BOOTSEL button on the RP2350.
3. While holding, plug USB back in.
4. The board appears as `RPI-RP2` USB mass storage.
5. Flash the recovery firmware:

```sh
picotool load -v -x radxa-linkr-debugger-rp2350.uf2
```

Or drag-and-drop the `.uf2` file onto the `RPI-RP2` volume.

## Monitor a target's serial output while controlling power

Use the embedded Web UI's auto-connected xterm.js J4 terminal alongside the
dashboard power controls. Or use the firmware raw-TCP path in parallel:

```sh
# Terminal 1: binary J4 console (replace with the board's current NCM address)
nc <board-ip> 5555

# Terminal 2: power control
radxa-linkr-debuggerctl power set 5v_out off
sleep 2
radxa-linkr-debuggerctl power set 5v_out on
```

J4 is one logical serial port. Firmware fans its shared 4096-byte RX ring out to
CDC ACM4, up to four `serial.j4` WebSocket sessions, and up to two raw-TCP
clients. Raw TCP is binary and performs no Telnet negotiation, echo, banner, or
line-ending conversion. A slow client only advances or drops its own sink.

When a logic-analyzer capture selects GP10 or GP11, firmware pauses J4, waits
for any active UART TX to reach wire-complete (with bounded timeout), cancels
queued writes, and transfers pin ownership to PIO2. Capture cleanup restores
the UART mux and resumes fresh sink epochs. Expect silence and explicit write
rejection during capture, then verify that the same J4 path works afterward.

## Run automated tests

For CI or scripted testing, use `--json` output and parse the envelope:

```sh
# Health check
radxa-linkr-debuggerctl --json doctor | jq '.ok'

# Power cycle and verify
radxa-linkr-debuggerctl --json power set 5v_out off
sleep 2
radxa-linkr-debuggerctl --json power set 5v_out on
radxa-linkr-debuggerctl --json power list | jq '.power_outputs[] | select(.name=="5v_out") | .state'

# ADC reading
radxa-linkr-debuggerctl --json adc read | jq '.readings[] | select(.name=="5v_out") | .current_ua'
```

The JSON envelope always has `schema`, `ok`, `command`, and `error` (on failure).
Parse `ok` for pass/fail, not the human-readable text.
