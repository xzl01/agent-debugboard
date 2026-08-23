[中文](power-analyzer.zh-CN.md)

# Power Analyzer

Power analysis with device monotonic timestamps and continuous host-side
recording. The power analyzer captures current waveforms from power rails,
supports multiple trigger types, and can record for hours without retaining the
complete sample set in debugger RAM or page memory.

The power analyzer is available in the Web UI's right-hand **Power analysis**
workspace, with live capture and startup analysis provided as two local modes.

## Capture Storage

Firmware detects the exact trigger and reports its device timestamp and sample
sequence; it does not retain the waveform. Samples are sent to the host
continuously, in batches,
and written as compact IndexedDB chunks. A 256-sample debugger RAM ring absorbs
short transport stalls. If that ring overruns, the result shows the reported
`dropped_samples` count, marks the archive incomplete, and disables battery
sizing so a partial waveform cannot be mistaken for a complete energy result.

Pre-trigger history is different from the hours-long record after the trigger:
it must remain immediately available in page memory. The Web UI therefore caps
it at 60,000 samples (120 seconds at the Web UI's 500 Hz continuous-recording
limit). Post-trigger samples continue to
stream to IndexedDB and are not subject to that page-memory limit.

Each active archive also carries a browser-session lease renewed by live sample
traffic. A second tab will not recover or modify the archive while that lease is
valid. After an actual page or browser failure, recovery rebuilds the bounded
preview, trigger offset, current/power summary, charge, and energy from the raw
IndexedDB chunks and keeps the result explicitly marked as interrupted.

The selected rate is a requested upper bound. Reports show the effective rate,
and energy is integrated from device monotonic timestamps when ADC or transport
throughput is lower than requested.

## Trigger Types

| Trigger | Description | Extra Parameters |
| --- | --- | --- |
| `manual` | Send `capture_trigger` command to fire | — |
| `current` | Fires when current exceeds threshold | `threshold_ua` (microamps) |
| `power_on` | Fires at rail power-on event | — |
| `gpio` | Fires on GPIO edge | `gpio` (from allowlist) + `edge` (`rising`/`falling`/`either`) |

## Arm Workflow

The capture lifecycle uses WebSocket commands on the existing live session:

1. **Arm**: send `capture_arm` command with trigger configuration
2. **Triggered**: firmware sends `capture_triggered` with device time and sample sequence
3. **Record**: the host writes incoming ADC telemetry to IndexedDB
4. **Stop**: the configured duration ends or the user stops manually; the host sends `capture_stop`
5. **Complete**: the host finalizes the archive and report

Example arm command:

```json
{
  "type": "command",
  "command": "capture_arm",
  "id": "capture-1",
  "mode": "host-stream-v1",
  "trigger": "current",
  "output": "5v_out",
  "threshold_ua": 500000,
  "rate_hz": 100
}
```

The Web UI verifies that status reports
`power_capture_protocol: "host-stream-v1"` before arming. A missing or different
value means the firmware and Web UI are incompatible, so no hardware action is
performed.

For manual captures, send `capture_trigger` after arming. Send `capture_stop`
after the host finishes recording. `capture_cancel` disarms the trigger.

### Timestamp Normalization

Match `capture_triggered.sample_sequence` to telemetry and normalize the x-axis
at that sample. Host receive time is not a reliable sampling clock — use device
monotonic timestamps.

## Web UI

### Power & Current Card

The dashboard **Power & current** card provides:

- Live current readings for each power rail
- Triggered power analysis with manual, current-threshold, GPIO-edge, and
  power-on triggers
- Overlay of the latest 4 captures for comparison
- Export to CSV or NDJSON with device timestamps
- Duration, mAh, and Wh reporting for the latest capture using trapezoidal
  integration over device monotonic timestamps
- Timed recordings from seconds to hours, or manual stop
- Continuous host-side persistence with a bounded decimated chart preview
- Storage preflight with a safety reserve before a timed capture touches hardware
- Atomic IndexedDB updates for binary sample chunks and archive progress, so a
  terminated page can retain the portion that reached storage
- Cursor-based CSV/NDJSON streaming directly to a file without materializing the
  full sample set or output text in page memory
- Immediate stop and explicit received/persisted/queued/dropped counters when
  host storage cannot keep up

Keep the Web page and debugger connection open while recording. The complete
raw record is stored on the host running the browser, not in debugger flash.

### Export Formats

| Format | Description |
| --- | --- |
| CSV | Device timestamps with current/voltage samples |
| NDJSON | One JSON object per line with device timestamps |

Both formats preserve trigger configuration, source rail, edge/threshold
settings, sampling rate, and pre/post window sizes.
Large exports require a Chromium-based browser with direct file-stream support.
Other browsers only use the bounded in-memory fallback for small exports.

## CLI: `adc record`

Continuous ADC recording from the command line:

```sh
radxa-linkr-debuggerctl adc record OUTPUT_PATH [MAX_SAMPLES] [--rate-hz HZ]
```

Examples:

```sh
# Record to NDJSON (default)
radxa-linkr-debuggerctl adc record /tmp/adc.ndjson 1000 --rate-hz 250

# Record to CSV
radxa-linkr-debuggerctl adc record /tmp/adc.csv 1000 --rate-hz 250
```

### Output Details

- **Format**: NDJSON by default; CSV when the output path ends in `.csv`
- **Device timing**: preserved under `metadata.device_timing` with fields
  `sample_sequence`, `uptime_us`, and `device_t_mono_us`
- **CSV time column**: uses `device_t_mono_us` first, falls back to `uptime_us`,
  then `0`
- **Ring overruns**: reported as `metadata.dropped_samples` on the first affected
  row
- **Rate**: defaults to 1000 Hz websocket subscription; `--rate-hz` accepts lower
  rates. Requests above 100 Hz use batch JSON on the wire, then the recorder
  expands each device sample into its own row.

## Startup Power Analysis

Located in the **Startup analysis** mode of the Web UI **Power analysis**
workspace. Records power-on current waveform and serial boot milestones together.

### Requirements

- An active UART0 or UART1 serial session must be connected
- An idle power-capture session

### Workflow

1. Clears and records the target serial console
2. Powers the selected rail off and waits for the configured discharge delay
3. Arms a firmware `power_on` capture before restoring the rail
4. Records serial RX independently after power-on
5. Timestamps the first post-power UART byte, U-Boot/UEFI markers, kernel
   markers, and login markers
6. Reports peak current, average power, and integrated energy

### Boot Firmware Detection

Automatic detection identifies the boot firmware from serial output. Can be
pinned manually to **U-Boot** or **UEFI** when the target platform is known
(e.g., Radxa O6N, Q6A).

### Results

- **Peak current**: maximum sample on the enabled rail
- **Average power**: energy divided by capture duration
- **Integrated energy**: trapezoidal integration over device monotonic timestamps
- **Power curve overlay**: the latest two completed runs for the same rail are
  trigger-aligned and overlaid

Energy covers only the firmware capture window, not serial activity beyond that
window.

### Limitations

- Only bytes received after the power-on request are retained; input drained
  while the rail is off is treated as stale data
- A capture with no post-power UART data, a lost serial connection, or no Login
  signature is reported as partial
- `power_on` captures use all capture capacity for post-trigger samples;
  pre-trigger ring is not returned on current firmware
- Serial milestones use the browser monotonic clock (host-observed timings),
  while power curves use the firmware monotonic clock

## Further Reading

- [Protocol details](../reference/power-analyzer.md) — capture protocol, WebSocket
  message format, and firmware-side implementation
