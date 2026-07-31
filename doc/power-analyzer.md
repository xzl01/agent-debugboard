# Power analyzer capture protocol

The power analyzer uses the existing dedicated live WebSocket session.
Single-sample ADC telemetry keeps `sequence` and `uptime_us` and also includes a
64-bit `sample_sequence` plus `device_t_mono_us`, the MCU monotonic uptime at
the ADC sampling instant. Compact batch samples carry `sequence` and
`uptime_us`; host clients normalize them to the same timing aliases while also
accepting explicit aliases under the `radxa-linkr-debugger.v1` schema.

## Capture ownership

Firmware is a trigger detector, not the recording store. It keeps only the
armed trigger configuration and the trigger sample's monotonic timestamp and
sequence. ADC telemetry flows to the host continuously in batches; the Web UI
keeps bounded pre-trigger history and writes the complete record to IndexedDB.

Firmware retains a 256-sample transport ring per WebSocket cursor only to absorb
short host or network stalls. This is not capture storage. `dropped_samples`
makes an overrun explicit instead of silently presenting an incomplete energy
result. Only one trigger can be armed at a time; closing its owning WebSocket
cancels it.

The configured rate is a requested upper bound. ADC and transport throughput
may deliver fewer samples, so the UI reports the effective rate and integrates
energy from device monotonic timestamps instead of assuming the requested
interval was achieved.

## Arm a capture

```json
{"type":"command","command":"capture_arm","id":"capture-1","mode":"host-stream-v1","trigger":"current","output":"5v_out","threshold_ua":500000,"rate_hz":100}
```

Supported triggers are `manual`, `current`, `power_on`, and `gpio`. GPIO capture
uses an allowlisted `gpio` plus `edge` set to `rising`, `falling`, or `either`.
For manual capture send `capture_trigger`. When the trigger fires, firmware
sends `capture_triggered` immediately. While the capture is recording,
`capture_stop` releases the trigger after the host finishes its timed or manual
recording; `capture_cancel` disarms it without completing a host archive.
The `mode` field is mandatory. Status responses and WebSocket snapshots advertise
the matching `power_capture_protocol`; clients must reject a mismatch before
operating the target hardware.

`capture_triggered` contains `device_t_mono_us`, `sample_sequence`, and the
owning telemetry cursor's cumulative `dropped_samples`. Firmware does not emit
`capture_begin`, `capture_sample`, or `capture_complete`; consumers identify
the exact trigger sample from `sample_sequence` and use telemetry device time,
not host receive time, as the sampling clock.

The Web UI keeps a bounded, decimated preview of four captures for overlays.
Every raw sample is persisted in host IndexedDB as it arrives; CSV or NDJSON
exports are rebuilt from that archive. Charge, energy, average current, and peak
current are accumulated incrementally, so multi-hour records do not grow React
memory with the full sample count. The page and WebSocket must remain connected
until the timed or manual stop completes.
Continuous CLI recording preserves `sample_sequence`, `uptime_us`, and
`device_t_mono_us` in `metadata.device_timing`; a `.csv` output path selects CSV
instead of NDJSON and uses `device_t_mono_us` first, then `uptime_us`, then `0`
for the CSV time column.

## Startup workflow

The Web UI exposes a startup power-analysis task inside its advanced toolbox.
The workflow requires the selected UART0 or UART1 session to already be
connected, powers the selected rail off, arms a `power_on` capture, and only
then restores the rail. Serial RX
is recorded independently so the first post-power UART byte, U-Boot or UEFI,
kernel, and login markers can span an arbitrary host-side recording window. The
first byte is used as the portable Boot milestone because many Boot
ROMs have no stable banner. Automatic boot-firmware detection can be pinned to
U-Boot or UEFI when the target platform is known.

Only bytes received after the power-on request are retained. Input drained while
the rail is off is treated as stale console data, so it cannot satisfy a marker
or pollute the downloaded raw log. ANSI control sequences and NUL bytes are
removed only from the matching view; exported text remains unchanged. A capture
with no post-power UART data, a lost serial connection, or no Login signature is
reported as partial rather than complete.

`power_on` captures begin at the switching event. Pre-trigger history and the
post-trigger recording window are both owned by the host; an off-rail baseline
is normally zero, so startup analysis focuses on the post-power waveform.

The latest two completed runs for the same rail are aligned to the firmware
trigger and overlaid. Summary values are derived from nominal rail voltage and
the captured current samples: peak current is the maximum enabled-rail sample,
energy uses trapezoidal integration, and average power is energy divided by the
capture duration. Energy therefore describes the fixed capture window, not the
entire serial timeout when boot continues beyond that window. These are analysis
values rather than calibrated protection
thresholds.
