# ADC Telemetry Contract

This document is the authoritative contract for ADC telemetry in
`radxa-linkr-debugger`. It describes the four descriptors the firmware
exposes, the HTTP read shape, the WebSocket telemetry shapes (both single
sample and batch), the Web UI rendering scope, and the related `GP29`
ownership rules. CLI, Web, Agent skill, firmware app, and HIL
documentation all reference this file rather than restating the same
shape.

> **Status:** The contract below describes the ADC3 compact telemetry
> scope that the in-progress firmware, host CLI, and Web UI implement
> together. The 2026-07-31 ADC3 telemetry HIL covers the
> directly-observed evidence on the canonical build for HIL spec 4b.1,
> 4b.2, 4b.3, 4b.4, the direct GP29 ownership subcase of 4b.5, and the
> combined-UF2 dual BOOTSEL recovery guards from sections 9 and 10; see
> [the dated report](../testing/results/2026-07-31-adc3-telemetry-hil.md).
> The historical v1 snapshot compatibility subcases of 4b.5, the full HIL
> spec 4b.6 boundary, and the rolling nightly checks (HIL spec section
> 12h) are out of scope for that report. Local `app adc` unit tests and
> Web `adc.test.ts` boundaries exist as proof-of-shape only and do not
> substitute for the real-board run. The post-token Web-only one-line
> CSS source edit (`text-[9px]` to `text-[11px]` in
> `web/src/components/PowerSparkline.tsx`) was not reflashed, so this
> contract still reflects the pre-token firmware; its local production
> build and visual QA pass is documented as a post-HIL addendum in the
> dated report and is not hardware HIL. Do not claim any nightly has
> passed because of a contract edit here. Nightly now runs only on a
> push to the `dev` branch, and each actual dev-push run stays
> `pending` until fresh GitHub execution evidence (a real Actions run
> on the new trigger) exists; that evidence does not exist yet at the
> time of this contract edit.

## 1. Descriptors And Types

The firmware reports exactly four ADC descriptors, in this fixed
order, on both `GET /api/v1/adc/read` and live WebSocket telemetry:

| `name`   | `kind`     | `unit` (HTTP) | `unit` (WS compact) | Physical source | Notes |
|----------|------------|---------------|---------------------|------------------|-------|
| `5v_out` | `current`  | `A`           | `uA`                | INA139 + S_C_5V  | current sense, `power_enabled` meaningful |
| `12v_out`| `current`  | `A`           | `uA`                | INA139 + S_C_12V | current sense, `power_enabled` meaningful |
| `20v_out`| `current`  | `A`           | `uA`                | INA139 + S_C_20V | current sense, `power_enabled` meaningful |
| `adc3`   | `voltage`  | `V`           | `uV`                | GP29 / ADC3      | monitor-only, no `power_enabled`, no probe scaling |

Both descriptors and kinds come from firmware and should not be
hardcoded by clients. The kinds shown above describe the implementation
on the current G3 (RP2350A) build; older firmware without `adc3`
returns only the three current descriptors and the host clients must
keep working without it.

The HTTP `unit` field is always a base-unit string (`A` for current,
`V` for voltage). The WebSocket compact shape always uses the
equivalent micro-unit (`uA` for current, `uV` for voltage). Both shapes
encode the scalar sample as a signed integer in the unit shown, so the
host must not re-scale before display.

## 2. Wire Shapes

### 2.1 HTTP read shape (`GET /api/v1/adc/read`)

The rich HTTP shape stays unchanged from the previous release and is
the place where raw, millivolt, and `sensor_value` diagnostics remain
visible. Each reading has at minimum:

```json
{
  "name": "5v_out",
  "signal": "S_C_5V",
  "sensor_channel": "current",
  "unit": "A",
  "current_ua": 540000,
  "power_enabled": true,
  "raw": 12345,
  "mv": 1234,
  "sensor_value": {"val1": 0, "val2": 540000}
}
```

For the four descriptors:

- `5v_out`, `12v_out`, `20v_out`: `sensor_channel="current"`, `unit="A"`,
  the canonical HTTP scalar is `current_ua` (signed integer µA);
  `power_enabled` is the live state of the
  matching `power/<name>` output (boolean).
- `adc3`: `sensor_channel="voltage"`, `unit="V"`, the canonical scalar
  is reconstructed by clients in signed integer µV from `sensor_value`;
  HTTP does not add a `value`, `current_ua`, or `power_enabled` field.
  `raw`, `mv`, and `sensor_value` remain present so the host can still
  cross-check against the firmware ADC pipeline.

Host clients do not apply any host-side ADC calibration table or
zero-point correction. They surface the firmware's direct readings.
The host CLI may present concise (`5v_out=0.540000A`) or verbose
(`signal=… power=on current=0.540000A current_ua=540000 raw=12345
mv=1234`) output. The Web UI parses the rich HTTP shape to keep the
Power card consistent with the embedded WebSocket samples.

### 2.2 WebSocket single-sample frame (`type: "telemetry"`)

The compact single-sample frame carries one reading per channel with
only the fields the high-rate WebSocket path actually needs:

```json
{
  "type": "telemetry",
  "topic": "adc",
  "schema": "radxa-linkr-debugger.v1",
  "sequence": 17,
  "readings": [
    {"name": "5v_out", "signal": "S_C_5V", "kind": "current", "unit": "uA", "value": 540000, "power_enabled": true},
    {"name": "12v_out", "signal": "S_C_12V", "kind": "current", "unit": "uA", "value": 120000, "power_enabled": false},
    {"name": "20v_out", "signal": "S_C_20V", "kind": "current", "unit": "uA", "value": 20000, "power_enabled": true},
    {"name": "adc3", "signal": "GPIO29_ADC3", "kind": "voltage", "unit": "uV", "value": 1234000}
  ],
  "uptime_us": 123456,
  "sample_sequence": 17,
  "device_t_mono_us": 123456
}
```

Rules:

- `value` is always a signed integer in `unit` (uA for current, uV for
  voltage). Hosts must not assume it is non-negative.
- The current channels carry `power_enabled: bool`; the voltage
  channel (`adc3`) never carries `power_enabled`.
- The single-sample frame never carries `raw`, `mv`, `current_ua`,
  `sensor_value`, or any other diagnostic field. Removing them keeps
  the WebSocket path byte-light, which is the whole reason the firmware
  switched to a compact shape.
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
    {"name": "5v_out",  "signal": "S_C_5V",  "kind": "current", "unit": "uA"},
    {"name": "12v_out", "signal": "S_C_12V", "kind": "current", "unit": "uA"},
    {"name": "20v_out", "signal": "S_C_20V", "kind": "current", "unit": "uA"},
    {"name": "adc3",    "signal": "GPIO29_ADC3", "kind": "voltage", "unit": "uV"}
  ],
  "samples": [
    {
      "sequence": 18,
      "uptime_us": 234567,
      "power_enabled_mask": 5,
      "values": [540000, 120000, 20000, 1234000]
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
- `power_enabled_mask` is an unsigned 8-bit mask. Bit i corresponds to
  channels[i] only when channels[i].kind == "current"; the voltage
  channel has no power state and its bit is undefined. Hosts must
  resolve bit i through `channels[i].kind`, not assume four current
  slots.
- `dropped_samples`, when non-zero, is emitted once at the head of the
  batch to flag a sampling-ring overrun. The recorder writes it under
  `metadata.dropped_samples` for the first affected row and removes it
  on subsequent rows of the same batch.

### 2.4 Why the WebSocket wire shape changed

The HTTP diagnostics (`raw`, `mv`, `sensor_value`) carry no benefit on
a high-rate telemetry stream, where every byte costs bandwidth and the
host only needs to graph the latest reading or write it to a recorder.
Re-emitting the full HTTP shape over WebSocket would have multiplied
frame bytes without adding chart-resolution information.

The compact wire shape is a deliberate, atomic contract change. There
is no dual-emission shim: clients that build on the previous WebSocket
shape (verbose readings with `raw` and `mv`) must update. Old
client-side parsers that expected `readings[].raw` or
`readings[].current_ua` on the WS path will not find those fields and
must instead consume the HTTP `GET /api/v1/adc/read` rich shape, or
migrate to the compact shape.

## 3. Reading Model Across Surfaces

The Rust CLI and the Web UI keep an internal `AdcReading` model with
`kind`, `unit`, `name`, `signal`, `value`, plus optional
`power_enabled` (current only) and the derived legacy HTTP fields
(`raw`, `mv`, `sensor_value`, `current_ua`, `voltage_uv`). Boundary
parsers (`parseHttpAdcReadings`, `parseCompactTelemetryReading`,
`parseCaptureSample`) normalize the three wire inputs into that model
without ever assigning `msg.readings` directly; each parser owns its
kind/unit validation and rejects mismatches instead of inventing
fields.

Three shape-specific readers:

- HTTP `/api/v1/adc/read` reader keeps the rich shape (`raw`, `mv`,
  `sensor_value`) so the Web Power card and the CLI `-v` mode can show
  diagnostics when the user asks for them.
- WS single reader accepts the compact frame and reconstructs
  `current_ua` or `voltage_uv` from `value` based on `kind`.
- Capture reader remains current-only and continues to consume the
  `current_ua`/`power_enabled` shape used by triggered power capture
  across the three current rails.

## 4. Web UI Scope (Power & Measurements)

The Power & measurements card is the visible surface for compact
telemetry. It keeps:

- One power row per controllable current rail (`5v_out`, `12v_out`,
  `20v_out`), with shared `MeasurementSparkline` showing current and
  power from the 60 Hz live subscription with a 90-sample window (about
  1.5 seconds of history). The window is shared between current and
  power; both values auto-scale.
- A monitor-only ADC3 (`adc3`) section between the current rows and
  the PowerAnalyzer, rendered only when the firmware emits an `adc3`
  reading. ADC3 uses a fixed 0..3,300,000 µV scale (nominal 0..3.3 V).
  The shared sparkline component reuses the same 90-sample SVG
  history with the same animation-frame cadence; only the Y scale is
  fixed.
- PowerAnalyzer remains a current-only triggered capture story: 2048
  samples, three current channels, manual / current-threshold / GPIO
  edge / power-on triggers, four-overlay comparison, CSV/NDJSON export.

The live WebSocket subscription stays at 60 Hz with `batch_size` 1 on
the wire. The Web UI coalesces that stream onto animation frames: while
the tab is visible, only the newest readings are published to the Power
card once per frame; while the tab is hidden the preview pauses, and an
armed or active streaming capture keeps ingesting every sample
losslessly because capture ingestion never routes through the
animation-frame preview.

GP29 appears in the GPIO catalog for snapshot compatibility, but the
Web UI must treat it as input-only while also showing its ADC3 voltage.

<a id="gp29-ownership"></a>
## 5. GP29 Ownership

GP29 (`ADC3`) is in the persisted/safe catalog but is owned by the
voltage monitor on this firmware, so it is **input-only**. Any
`gpio set GP29 ...` output attempt fails at the firmware layer with a
firmware-owned error; the host CLI / TUI / Web UI must surface that
failure rather than retrying.

Persistence behaviour for v1 snapshots:

- A v1 snapshot that records GP29 as `direction="input"` continues to
  decode and apply normally. The firmware treats an input snapshot as
  the safe default and auto-applies it after firmware defaults on the
  next boot.
- A v1 snapshot that records GP29 as `direction="output"` remains
  decodable so historical boards do not corrupt their snapshots, but
  applying that entry fails at GP29. The existing partial-apply rule
  remains authoritative: entries before the failure stay applied, and
  GP29 plus any later entries stay pending. There is no hidden rollback.

Old "GP29 is an ordinary output-capable GPIO" wording is replaced by
this contract. Any text that lists GP29 alongside GP10..GP20 as a
freely drivable safe GPIO must be updated to call out the ADC3
ownership and the input-only behaviour.

## 6. CLI And TUI Examples

The host CLI mirrors the contract. `adc read` and `adc read adc3`
return the four-descriptor list. `adc read -v <channel>` adds the
diagnostic fields. `adc record` creates a live session and writes one
JSON record per device sample, using the compact shape as the source
of truth and surfacing it through the internal `AdcReading` model.

```sh
# Inspect all four descriptors
radxa-linkr-debuggerctl --json adc read

# Read the new voltage monitor
radxa-linkr-debuggerctl --json adc read adc3
radxa-linkr-debuggerctl --json adc read adc3 -v

# Domain-specific CSV or NDJSON record (1000 Hz by default)
radxa-linkr-debuggerctl adc record /tmp/adc.ndjson 1000 --rate-hz 250
radxa-linkr-debuggerctl adc record /tmp/adc.ndjson
```

The `raw` HTTP-only mode is not exposed by the WS compact frame.
Clients that want raw diagnostics must use the HTTP path explicitly.

## 7. Implementation Pointers

This contract is enforced through a small, consistent set of files:

- `cmd-ng/src/adc.rs` boundary parsers and the `AdcReading`
  discriminated union (current vs voltage with shared `kind`/`unit`/
  `value`).
- `cmd-ng/src/ws_client.rs` frame dispatch, with the single
  sample and batch frames normalised into the same `AdcReading` list.
- `web/src/lib/adc.ts` mirrors: strict boundary parsers for HTTP,
  WS compact, and capture shapes.
- `web/src/components/PowerSparkline.tsx` carries the shared
  `MeasurementSparkline` rendered in both `mode="power"` and
  `mode="voltage"` shapes.
- `web/src/components/PowerCard.tsx` selects current rows for the
  sparkline rows and inserts the ADC3 section between those rows and
  the PowerAnalyzer.
- `apps/radxa_linkr_debugger/src/...` emits the compact WS frames,
  the rich HTTP responses, and the four-descriptor runtime catalog.
- `skills/radxa-linkr-debugger/SKILL.md` keeps the curl-first examples
  above and points users back here for the wire shapes.
- `docs/testing/hil-functional-test-spec.md` carries the ADC3-specific
  checklist items required before any claim of validation.

## 8. Things This Contract Does Not Cover

- Logic analyzer (`type="sigrok"`) WebSocket frames use a separate
  binary protocol; they are not part of ADC telemetry and not
  affected by this contract.
- The HTTP/WS status frame still carries `board_monitoring`. ADC
  telemetry is independent of `board_monitoring` and lives on its own
  ADC topic (`topic="adc"` for both single and batch frames).
- Flash / arena growth: the compact WebSocket shape is expected to
  keep current-meter API reuse and avoid raw+mv bandwidth on the WS
  path, but the saved arena baseline in dated reports stays valid
  only for the dated build. Do not claim a new baseline until the
  next canonical `west build` runs and its size is measured.
