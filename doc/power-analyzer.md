# Power analyzer capture protocol

The power analyzer uses the existing dedicated live WebSocket session. Normal
ADC telemetry includes `sample_sequence` and `device_t_mono_us`, the MCU
monotonic uptime at the ADC sampling instant.

## Capture capacity

- G3 / RP2350: 2048 samples
- G2 / RP2040: 512 samples

`pre_samples + post_samples + 1` must not exceed the board capacity. Capture
storage is a firmware-owned global ring buffer because only one hardware ADC
capture can be active at a time. Closing the owning WebSocket cancels it.

## Arm a capture

```json
{"type":"command","command":"capture_arm","id":"capture-1","trigger":"current","output":"5v_out","threshold_ua":500000,"rate_hz":100,"pre_samples":100,"post_samples":300}
```

Supported triggers are `manual`, `current`, `power_on`, and `gpio`. GPIO capture
uses an allowlisted `gpio` plus `edge` set to `rising`, `falling`, or `either`.
For manual capture send `capture_trigger`; cancel with `capture_cancel`.

After the post-trigger window, firmware sends `capture_begin`, one ordered
`capture_sample` per buffered sample, then `capture_complete`. Consumers should
normalize the x-axis against `trigger_offset`; host receive time is not a
reliable sampling clock.

The Web UI keeps four captures for overlays and exports CSV or NDJSON. Both
formats preserve trigger, source, edge, threshold, sampling rate, and pre/post
window sizes so an exported run retains its experiment conditions.
Continuous CLI recording also preserves `device_t_mono_us`; a `.csv` output
path selects CSV instead of NDJSON.

## Startup workflow

The Web UI exposes a startup power-analysis task inside its advanced toolbox.
The workflow requires the selected UART0 or UART1 session to already be
connected, powers the selected rail off, arms a `power_on` capture, and only
then restores the rail. Serial RX
is recorded independently so the first post-power UART byte, U-Boot or UEFI,
kernel, and login markers can span longer than the fixed firmware capture
buffer. The first byte is used as the portable Boot milestone because many Boot
ROMs have no stable banner. Automatic boot-firmware detection can be pinned to
U-Boot or UEFI when the target platform is known.

Only bytes received after the power-on request are retained. Input drained while
the rail is off is treated as stale console data, so it cannot satisfy a marker
or pollute the downloaded raw log. ANSI control sequences and NUL bytes are
removed only from the matching view; exported text remains unchanged. A capture
with no post-power UART data, a lost serial connection, or no Login signature is
reported as partial rather than complete.

The startup preset assigns the complete capture capacity to post-trigger
samples. `power_on` captures begin at the switching event and do not return the
configured pre-trigger ring on current firmware; an off-rail baseline would be
zero and shortening the post-trigger window is less useful for startup work.

The latest two completed runs for the same rail are aligned to the firmware
trigger and overlaid. Summary values are derived from nominal rail voltage and
the captured current samples: peak current is the maximum enabled-rail sample,
energy uses trapezoidal integration, and average power is energy divided by the
capture duration. Energy therefore describes the fixed capture window, not the
entire serial timeout when boot continues beyond that window. These are analysis
values rather than calibrated protection
thresholds.
