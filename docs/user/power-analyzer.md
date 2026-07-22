[中文](power-analyzer.zh-CN.md)

# Power Analyzer

Firmware-backed power analysis using a ring buffer with device monotonic
timestamps. The power analyzer captures current waveforms from power rails,
supports multiple trigger types, and provides overlay comparison of up to four
captures.

The power analyzer is available in the Web UI dashboard under the **Power &
current** card, and in the **Advanced & recovery** toolbox for startup power
analysis.

## Capture Capacity

| Board | Capacity |
| --- | --- |
| RP2350 | 2048 samples |

The constraint `pre_samples + post_samples + 1` must not exceed board capacity.

Capture storage uses a firmware-owned global ring buffer because only one
hardware ADC capture can be active at a time. Closing the owning WebSocket
cancels the capture.

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
2. **Begin**: firmware responds with `capture_begin`
3. **Samples**: firmware sends `capture_sample` for each buffered sample (ordered)
4. **Complete**: firmware sends `capture_complete`

Example arm command:

```json
{
  "type": "command",
  "command": "capture_arm",
  "id": "capture-1",
  "trigger": "current",
  "output": "5v_out",
  "threshold_ua": 500000,
  "rate_hz": 100,
  "pre_samples": 100,
  "post_samples": 300
}
```

For manual captures, send `capture_trigger` after arming. Cancel with
`capture_cancel`.

### Timestamp Normalization

Normalize the x-axis against `trigger_offset` in the capture response. Host
receive time is not a reliable sampling clock — use the device monotonic
timestamps provided with each sample.

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

### Export Formats

| Format | Description |
| --- | --- |
| CSV | Device timestamps with current/voltage samples |
| NDJSON | One JSON object per line with device timestamps |

Both formats preserve trigger configuration, source rail, edge/threshold
settings, sampling rate, and pre/post window sizes.

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

Located under **Advanced & recovery** in the Web UI. Records power-on current
waveform and serial boot milestones together.

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

- [Protocol details](../../doc/power-analyzer.md) — capture protocol, WebSocket
  message format, and firmware-side implementation
