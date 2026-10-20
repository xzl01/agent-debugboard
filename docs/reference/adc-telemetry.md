# ADC Telemetry Contract

This document is the authoritative contract for ADC telemetry in
`radxa-linkr-debugger`. It describes the five descriptors the firmware
exposes, the HTTP read shape, the WebSocket telemetry shapes (single
sample and batch), the Web UI rendering scope, and the related `GP29`
ownership rules. CLI, Web, Agent skill, firmware app, and HIL
documentation reference this file rather than restating the same shape.

> **Status:** This contract describes the current firmware, Rust host, and
> Web UI implementation for the four-INA226 plus ADC3 telemetry topology.
> It is not a HIL report and makes no board-level validation claim. The
> executable board checklist and the current validation status live in
> [the HIL functional test spec](../testing/hil-functional-test-spec.md).
> Do not cite an older ADC telemetry HIL run as evidence for this
> five-descriptor shape.

## 1. Descriptors And Types

The firmware reports exactly five ADC descriptors, in this fixed order,
on both `GET /api/v1/adc/read` and live WebSocket telemetry:

| `name`    | `kind`    | `unit` (HTTP) | `unit` (WS compact) | Physical source | Notes |
|-----------|-----------|---------------|---------------------|-----------------|-------|
| `5v_out`  | `current` | `A`           | `uA`                | INA226, signal `INA226_5V`  | controllable 5 V output rail |
| `12v_out` | `current` | `A`           | `uA`                | INA226, signal `INA226_12V` | controllable 12 V output rail |
| `20v_out` | `current` | `A`           | `uA`                | INA226, signal `INA226_20V` | controllable 20 V output rail |
| `vdd_5v`  | `current` | `A`           | `uA`                | INA226, signal `INA226_VDD_5V` | controllable auxiliary 5 V rail |
| `adc3`    | `voltage` | `V`           | `uV`                | ADC3 / `GP29`, signal `GPIO29_ADC3` | monitor-only; no power state |

All four current descriptors are backed by INA226 devices. Each INA226
reading carries both the current-channel sample and the bus voltage
sampled from the same device. `adc3` is a separate ADC voltage channel;
it is not a rail-current sensor and has no `power_enabled` state.

Descriptor order is part of the wire contract: it is also the order of
`readings[]` and `channels[]`, and the current descriptors are the base
for the compact current-channel indexes used by the batch masks. Clients
must consume the descriptor metadata from the payload instead of
hardcoding a count or assuming a different order.

### 1.1 Units, Ranges, And Array Bounds

- HTTP uses base-unit strings: `A` for current and `V` for voltage. The
  rich HTTP scalar fields `current_ua`, `voltage_uv`, and
  `sensor_value` use micro-units or Zephyr `sensor_value` pairs as
  described below.
- WebSocket compact values use micro-units: `uA` for current and `uV`
  for voltage. Every scalar is a signed 32-bit integer in the declared
  unit. Hosts must not assume it is non-negative and must not apply
  their own ADC calibration or zero-point correction.
- The firmware does not clamp samples to a display range. The Web UI's
  ADC3 chart uses a fixed `0..3,300,000 uV` (nominal `0..3.3 V`) scale;
  current, bus-voltage, and power traces auto-scale. That chart scale is
  a rendering choice, not a calibrated accuracy or validity claim.
- In the current batch shape, `values` has one entry per descriptor
  (five) and `voltages_uv` has one entry per current descriptor (four).
  The two masks use the low four bits; higher bits are zero.
- `voltage_valid` is the authority for a bus-voltage sample. A current
  reading can legitimately carry a `voltage_uv` slot while
  `voltage_valid` is `false`; that slot is not a measurement.

## 2. Wire Shapes

### 2.1 HTTP read shape (`GET /api/v1/adc/read`)

The HTTP read shape is the rich diagnostics surface. It keeps the raw,
millivolt, and `sensor_value` fields that the compact WebSocket stream
does not repeat. A current reading has this shape:

```json
{
  "name": "5v_out",
  "signal": "INA226_5V",
  "power_enabled": true,
  "raw": null,
  "mv": 5010,
  "voltage_valid": true,
  "voltage_uv": 5010000,
  "sensor_channel": "current",
  "unit": "A",
  "sensor_value": {"val1": 0, "val2": 540000},
  "current_ua": 540000
}
```

For the four current descriptors:

- `sensor_channel="current"` and `unit="A"`.
- `current_ua` is the canonical signed integer current sample in
  microamps. `sensor_value` is the matching Zephyr value: `val1` is
  whole amps and `val2` is millionths of an amp.
- `power_enabled` is the live state of the matching `power/<name>`
  output. It is present for every current descriptor, including
  `vdd_5v`.
- `mv` is the INA226 bus voltage in millivolts. `voltage_valid` says
  whether the same INA226 sample produced a usable bus voltage.
  `voltage_uv` is the signed bus voltage in microvolts when
  `voltage_valid` is `true`, and JSON `null` when it is `false`.
- `raw` is an ADC-pipeline diagnostic, not an INA226 register value.
  The INA226 current path does not populate it, so current descriptors
  emit `"raw": null`. Clients must accept `null` and must not render it
  as a zero measurement.

The `adc3` reading has a different shape:

```json
{
  "name": "adc3",
  "signal": "GPIO29_ADC3",
  "raw": 2048,
  "mv": 1650,
  "sensor_channel": "voltage",
  "unit": "V",
  "sensor_value": {"val1": 1, "val2": 650000}
}
```

- `adc3` has `sensor_channel="voltage"` and `unit="V"`.
- The canonical scalar is reconstructed by clients from
  `sensor_value` as `val1 * 1,000,000 + val2` microvolts.
- HTTP does not add `value`, `current_ua`, `power_enabled`,
  `voltage_valid`, or `voltage_uv` to the `adc3` reading. `raw`, `mv`,
  and `sensor_value` remain present for pipeline diagnostics; a
  successful `adc3` read populates both `raw` and `mv`.

Host clients do not apply any host-side ADC calibration table. They
surface the firmware's direct readings. The host CLI may present concise
(`5v_out=0.540000A voltage=5.010000V`) or verbose
(`signal=… power=on current=0.540000A current_ua=540000
voltage=5.010000V voltage_uv=5010000 raw=null mv=5010`) output. The Web
UI parses the rich HTTP shape to keep the Power card consistent with the
embedded WebSocket samples.

### 2.2 WebSocket single-sample frame (`type: "telemetry"`)

The compact single-sample frame carries one reading per descriptor with
only the fields the high-rate WebSocket path needs:

```json
{
  "type": "telemetry",
  "topic": "adc",
  "schema": "radxa-linkr-debugger.v1",
  "sequence": 17,
  "uptime_us": 123456,
  "sample_sequence": 17,
  "device_t_mono_us": 123456,
  "readings": [
    {"name": "5v_out",  "signal": "INA226_5V",     "kind": "current", "unit": "uA", "power_enabled": true,  "current_ua": 540000, "voltage_valid": true,  "voltage_uv": 5010000, "value": 540000},
    {"name": "12v_out", "signal": "INA226_12V",    "kind": "current", "unit": "uA", "power_enabled": false, "current_ua": 120000, "voltage_valid": true,  "voltage_uv": 12020000, "value": 120000},
    {"name": "20v_out", "signal": "INA226_20V",    "kind": "current", "unit": "uA", "power_enabled": true,  "current_ua": 20000,  "voltage_valid": true,  "voltage_uv": 20030000, "value": 20000},
    {"name": "vdd_5v",  "signal": "INA226_VDD_5V", "kind": "current", "unit": "uA", "power_enabled": true,  "current_ua": 150000, "voltage_valid": true,  "voltage_uv": 5000000,  "value": 150000},
    {"name": "adc3",    "signal": "GPIO29_ADC3",   "kind": "voltage", "unit": "uV", "value": 1234000}
  ]
}
```

Rules:

- `value` is always a signed integer in `unit` (`uA` for current, `uV`
  for voltage). For current descriptors, the firmware currently emits
  the same sample in both `value` and `current_ua`; `value` is the
  common host-model scalar and `current_ua` is the explicit current
  alias.
- Every current reading carries `power_enabled`, `current_ua`,
  `voltage_valid`, and `voltage_uv`. `voltage_uv` is always an integer
  on this frame; it is meaningful only when `voltage_valid` is `true`.
  When `voltage_valid` is `false`, clients must ignore the voltage slot
  rather than graph it as a real zero.
- The voltage descriptor (`adc3`) never carries `power_enabled`,
  `current_ua`, `voltage_valid`, or `voltage_uv`. Its `value` is the
  reconstructed signed microvolt sample.
- The single-sample frame never carries `raw`, `mv`, or `sensor_value`.
- Companion timing fields (`uptime_us`, `sample_sequence`,
  `device_t_mono_us`, optional `dropped_samples`) stay on the envelope,
  not on each reading.

### 2.3 WebSocket batch frame (`type: "telemetry-batch"`)

Above a configurable rate the firmware switches to a batch frame. The
batch shape declares channels once and then carries `values: i32[]` per
sample, positionally aligned with `channels`:

```json
{
  "type": "telemetry-batch",
  "topic": "adc",
  "schema": "radxa-linkr-debugger.v1",
  "dropped_samples": 0,
  "channels": [
    {"name": "5v_out",  "signal": "INA226_5V",     "kind": "current", "unit": "uA"},
    {"name": "12v_out", "signal": "INA226_12V",    "kind": "current", "unit": "uA"},
    {"name": "20v_out", "signal": "INA226_20V",    "kind": "current", "unit": "uA"},
    {"name": "vdd_5v",  "signal": "INA226_VDD_5V", "kind": "current", "unit": "uA"},
    {"name": "adc3",    "signal": "GPIO29_ADC3",   "kind": "voltage", "unit": "uV"}
  ],
  "samples": [
    {
      "sequence": 18,
      "uptime_us": 234567,
      "power_enabled_mask": 15,
      "voltage_valid_mask": 15,
      "values": [540000, 120000, 20000, 150000, 1234000],
      "voltages_uv": [5010000, 12020000, 20030000, 5000000]
    }
  ]
}
```

Rules:

- `channels[]` declares kind and unit per channel; the host validates
  the pair (`current` ↔ `uA`, `voltage` ↔ `uV`) before trusting the
  frame.
- Each `samples[].values[]` entry is a single signed integer, so
  `values.len() == channels.len()`. The batch carries one scalar per
  channel; no rich diagnostics are repeated on the batch wire.
- `power_enabled_mask` is an unsigned integer mask whose bit index is
  the **current-channel index**, not the raw `channels[]` index in
  general. Bit 0 is the first current-kind channel, bit 1 the second,
  and so on. With the current fixed order, bits 0..3 are `5v_out`,
  `12v_out`, `20v_out`, and `vdd_5v`. The voltage channel (`adc3`) has
  no power state and contributes no bit; bits 4..7 are zero.
- `voltage_valid_mask` and `voltages_uv[]` use that same current-channel
  index. `voltages_uv` always has one slot per current-kind channel
  (four in the current order). A cleared mask bit means the matching
  array slot is not a measurement and must not be presented as one; it
  does not change the indexing of the other slots.
- Batch samples carry `sequence` and `uptime_us` but not
  `sample_sequence` or `device_t_mono_us`. The Rust recorder expands
  those two aliases from `sequence` and `uptime_us` when it writes
  single-sample records.
- `dropped_samples`, when non-zero, is emitted once on the batch envelope. It
  counts both sampling-ring overwrite and source periods skipped when the
  absolute-deadline sampler cannot complete a slot at the requested rate. The
  250 Hz ceiling is a maximum request rate, not an assertion that every slot is
  lossless under simultaneous USB-NCM transmission. The recorder writes it
  under `metadata.dropped_samples` for the first affected row and
  removes it on subsequent rows of the same batch.

### 2.4 Sampling Rate Limits

The firmware ADC sampler accepts `rate_hz` in the inclusive range
`1..250` for both `subscribe` and `capture_arm`. A value above 250 is
rejected with an `invalid_rate` error; it is not clamped. An omitted
`subscribe.rate_hz` defaults to 10, and an omitted `capture_arm.rate_hz`
defaults to 100. The Web UI's live preview requests 60 Hz, while the CLI
`adc record` command defaults to 250 Hz. See section 6 for the matching
CLI range.

### 2.5 Compact versus rich shapes

The HTTP diagnostics (`raw`, `mv`, `sensor_value`) carry no benefit on
a high-rate telemetry stream, where every byte costs bandwidth and the
host only needs to graph the latest reading or write it to a recorder.
Re-emitting the full HTTP shape over WebSocket would have multiplied
frame bytes without adding chart-resolution information.

The compact wire shape is the only WebSocket ADC telemetry shape. There
is no dual emission or alternate verbose WebSocket ADC shape. Clients
that need raw diagnostics use `GET /api/v1/adc/read`; clients on the
live path consume the fields defined in sections 2.2 and 2.3.

## 3. Reading Model Across Surfaces

The Rust CLI and the Web UI keep an internal `AdcReading` model with
`kind`, `unit`, `name`, `signal`, `value`, plus optional
`power_enabled` (current only), current-rail voltage state, and the
HTTP diagnostics (`raw`, `mv`, `sensor_value`, `current_ua`). The Web
model retains `voltage_valid` next to `voltage_uv`; a model that keeps
only `voltage_uv` must treat it as absent when the flag is false or the
HTTP field is `null`. Boundary parsers
(`parseHttpAdcReadings`, `parseCompactAdcReadings`,
`parseCaptureCurrentReadings`, and the Rust equivalents) normalize the
three wire inputs into that model instead of assigning `msg.readings`
directly. Each parser owns its kind/unit validation and rejects
mismatches instead of inventing fields.

Shape-specific readers:

- HTTP `/api/v1/adc/read` reader keeps the rich shape (`raw`, `mv`,
  `sensor_value`) so the Web Power card and the CLI `-v` mode can show
  diagnostics when the user asks for them. It accepts `raw: null` and
  `voltage_uv: null`; neither means zero.
- WS single reader accepts the compact frame, reconstructs the primary
  scalar from `value`, and retains `voltage_valid`/`voltage_uv` for
  current rails. When `voltage_valid` is false, the voltage must be
  treated as unknown even though the frame carries an integer slot.
- Batch reader expands each sample by channel position, maps
  `power_enabled_mask`, `voltage_valid_mask`, and `voltages_uv` through
  the compact current-channel index, and then applies the same current
  reading rules as the single-sample path.
- The capture reader is current-only and consumes all four current
  descriptors (`current_ua` plus `power_enabled`) from the live stream.

## 4. Web UI Scope (Power & Measurements)

The Power & measurements card is the visible surface for compact
telemetry. It keeps:

- One power row per controllable current rail (`5v_out`, `12v_out`,
  `20v_out`, `vdd_5v`), with shared `MeasurementSparkline` showing
  current, measured bus voltage, and power from the 60 Hz live
  subscription with a 90-sample window (about 1.5 seconds of history).
  The traces share the sample history but auto-scale independently.
- A monitor-only ADC3 (`adc3`) section between the current rows and
  the PowerAnalyzer, rendered only when the firmware emits an `adc3`
  reading. ADC3 uses a fixed `0..3,300,000 uV` scale (nominal
  `0..3.3 V`). The shared sparkline component reuses the same 90-sample
  SVG history with the same animation-frame cadence; only the Y scale
  is fixed.
- PowerAnalyzer remains a current-only triggered capture story. The
  capture stream retains all four current descriptors, while the
  analyzer's comparison and its Web capture CSV export are scoped to
  the three user rails `5v_out`, `12v_out`, and `20v_out`. NDJSON export
  preserves the full capture readings. Trigger modes remain manual,
  current-threshold, GPIO edge, and power-on.

The live WebSocket subscription stays at 60 Hz with `batch_size` 1 on
the wire. The Web UI coalesces that stream onto animation frames: while
the tab is visible, only the newest readings are published to the Power
card once per frame; while the tab is hidden the preview pauses, and an
armed or active streaming capture keeps ingesting every sample
losslessly because capture ingestion never routes through the
animation-frame preview.

GP29 appears in the GPIO catalog, but the Web UI must treat it as
input-only while also showing its ADC3 voltage.

<a id="gp29-ownership"></a>
## 5. GP29 Ownership

GP29 (`ADC3`) is in the persisted/safe catalog but is owned by the
voltage monitor on this firmware, so it is **input-only**. Any
`gpio set GP29 ...` output attempt fails at the firmware layer with a
firmware-owned error; the host CLI / TUI / Web UI must surface that
failure rather than retrying.

Snapshot behaviour:

- A snapshot entry that records GP29 as `direction="input"` decodes and
  applies normally. The firmware treats an input snapshot as the safe
  default and auto-applies it after firmware defaults on the next boot.
- A snapshot entry that records GP29 as `direction="output"` remains
  decodable, but applying that entry fails at GP29 with the input-only
  error. The existing partial-apply rule remains authoritative: entries
  before the failure stay applied, and GP29 plus any later entries stay
  pending. There is no hidden rollback.

GP29 must not be described as a freely drivable safe GPIO. Text that
lists the output-capable safe GPIOs must call out the ADC3 ownership and
the input-only behaviour.

## 6. CLI And TUI Examples

The host CLI mirrors the contract. `adc read` and `adc read adc3`
return the five-descriptor list. `adc read -v <channel>` adds the
diagnostic fields. `adc record` creates a live session and writes one
record per device sample, using the compact shape as the source of
truth and surfacing it through the internal `AdcReading` model.

```sh
# Inspect all five descriptors
radxa-linkr-debuggerctl --json adc read

# Read the voltage monitor
radxa-linkr-debuggerctl --json adc read adc3
radxa-linkr-debuggerctl --json adc read adc3 -v

# Record all five descriptors as NDJSON at the default 250 Hz
radxa-linkr-debuggerctl adc record /tmp/adc.ndjson 500

# Record the fixed current-rail CSV table at 250 Hz
radxa-linkr-debuggerctl adc record /tmp/adc.csv 500 --rate-hz 250
```

`adc record` defaults to 250 Hz. `--rate-hz HZ` accepts the inclusive
range `1..250`; the CLI and firmware reject higher values instead of
silently clamping them.

NDJSON records preserve every descriptor, including `adc3`. CSV output
is a fixed per-rail table for the four current rails `5v_out`,
`12v_out`, `20v_out`, and `vdd_5v`; `adc3` is not a CSV column. Each
rail contributes `power_enabled`, `current_ua`, and `voltage_uv`
columns. Any unknown per-rail field, including an unknown power state,
current, or bus voltage, is written as an empty CSV cell, never as
`false` or `0`, so an unknown value cannot be confused with a real zero.
The exact column order is owned by the Rust recorder; consumers should
use the header row rather than positional assumptions.

The `raw` diagnostics are HTTP-only and are not exposed by the WS
compact frame. Clients that want raw diagnostics must use the HTTP path
explicitly.

## 7. Implementation Pointers

This contract is enforced through a small, consistent set of files:

- `apps/radxa_linkr_debugger/src/linkr_debugger_model.c` defines the
  five-descriptor catalog and each descriptor's fixed order.
- `apps/radxa_linkr_debugger/src/linkr_debugger_control.c` samples the
  four INA226 devices and the ADC3 channel and fills `current_ua`,
  `voltage_valid`, `voltage_uv`, and the raw ADC diagnostics.
- `apps/radxa_linkr_debugger/src/linkr_debugger_http.c` emits the rich
  HTTP readings, including JSON `null` for unavailable `raw` and
  invalid `voltage_uv`.
- `apps/radxa_linkr_debugger/src/linkr_debugger_ws.c` emits the compact
  single and batch frames, including the masks and `voltages_uv`.
- `cmd-ng/src/adc/model.rs`, `cmd-ng/src/adc/normalize.rs`, and
  `cmd-ng/src/ws_client.rs` carry the Rust boundary parsers and batch
  expansion.
- `cmd-ng/src/recorder.rs` owns the NDJSON and CSV record shapes.
- `web/src/lib/adc.ts` and `web/src/lib/boardTelemetry.ts` mirror the
  boundary parsers for HTTP, WS compact, batch, and capture shapes.
- `web/src/components/PowerSparkline.tsx` carries the shared
  `MeasurementSparkline` rendered in both `mode="power"` and
  `mode="voltage"` shapes.
- `web/src/components/PowerCard.tsx` renders the four current rows and
  inserts the ADC3 section between those rows and the PowerAnalyzer.
- `skills/radxa-linkr-debugger/SKILL.md` keeps the curl-first examples
  and points users back here for the wire shapes.
- `docs/testing/hil-functional-test-spec.md` carries the board-level
  checklist required before any claim of validation.

## 8. Things This Contract Does Not Cover

- Logic analyzer (`type="sigrok"`) WebSocket frames use a separate
  binary protocol; they are not part of ADC telemetry and are not
  affected by this contract.
- The HTTP/WS status frame still carries `board_monitoring`. ADC
  telemetry is independent of `board_monitoring` and lives on its own
  ADC topic (`topic="adc"` for both single and batch frames).
- Firmware flash/RAM sizing and the HIL pass/fail status are outside
  this wire contract. Do not infer a board-level validation result from
  a documentation change.
