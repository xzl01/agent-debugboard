# Development Debugging

[中文](debugging.zh-CN.md)

This page is for diagnosing and changing Radxa Linkr Debugger itself. It is
not an operator procedure for a connected target board. Target operations,
power control, recovery, and serial use belong in the agent skill and the
[user guide](../user/README.md).

## Start With The Failing Layer

Separate failures by the first boundary that does not work. Do not use a
browser symptom to infer a firmware failure, and do not use a local test as
evidence of board behavior.

| First failing boundary | Canonical next step |
| --- | --- |
| Build, generated assets, or artifact selection | [Build Guide](build.md) |
| ROM BOOTSEL or MCUboot image recovery | [Flashing](flashing.md) |
| USB NCM, HTTP, captive portal, or hosted Web UI | [Web UI](../user/webui.md) and [Troubleshooting](../user/troubleshooting.md) |
| Raw HTTP response schema or status endpoint | [HTTP API](../user/api.md) |
| MCP availability, tool result, or progress protocol | [MCP Server](../reference/mcp-server.md) |
| MCU J4 state, sinks, framing, raw TCP, or capture pause | [J4 Serial Transport](../reference/serial-broker.md) |
| Legacy host `uart0`/`uart1` broker, MCP cursors, or archives | [MCP Server](../reference/mcp-server.md) and the retired-path section of [J4 Serial Transport](../reference/serial-broker.md) |
| ADC, GP29, or telemetry wire shape | [ADC Telemetry](../reference/adc-telemetry.md) |
| Logic analyzer transport or capture semantics | [Logic Analyzer](../reference/logic-analyzer.md) |
| OpenOCD/JTAG adapter or target reset | [OpenOCD](../user/openocd.md) |
| Required board-level acceptance | [HIL Functional Test Spec](../testing/hil-functional-test-spec.md) |

## Browser And Gateway Changes

For a board-hosted page, a navigation or resource-load timeout identifies the
USB NCM, HTTP service, or browser setup boundary. An assertion failure after
the page renders identifies a Web UI regression. Preserve this distinction in
test reports and bug triage.

The production board page talks directly to firmware HTTP/WebSocket. Its J4
xterm.js terminal creates a live session and subscribes to `serial.j4`; it does
not use a browser serial API, chooser, CH347 port, or host broker. Diagnose
auto-connect, sticky manual disconnect, binary framing, and pause/resume against
the MCU path documented in [Web UI](../user/webui.md).

The separate loopback Host gateway on port 8787 still exists for packaged Web
gateway/MCP use. Diagnose its REST, OTA, and WebSocket forwarding only when that
gateway is actually in the failing path. Its legacy serial broker is not J4.
Do not document a development gateway command in the operator skill.

## Control Plane Isolation

Use the firmware response envelope as the boundary between transport and
operation failures. A reachable endpoint with `ok: false` is a firmware
rejection; a timeout, connection refusal, or route failure is transport or USB
NCM state. Protocol fields and error meanings remain authoritative in the
[HTTP API](../user/api.md).

The embedded Web UI and J4 share the MCU serial runtime. Diagnose J4 sink
overruns, accepted/completed events and raw TCP against the
[J4 Serial Transport](../reference/serial-broker.md). Separately, MCP still has
legacy `uart0`/`uart1` cursor/archive tools backed by the host broker; diagnose
those against [MCP Server](../reference/mcp-server.md) without claiming they are
J4. Mutating operations require a new operator decision after an error.

## Firmware And Recovery Changes

Keep image construction, build-directory policy, release artifact naming, and
source-build instructions in the [Build Guide](build.md). Keep flashing
procedures and the combined-UF2 versus OTA-binary distinction in
[Flashing](flashing.md). The physical ROM BOOTSEL recovery route remains the
fallback when the normal control plane is unavailable.

For target-side JTAG/SWD work, Linkr controls power and recovery lines but is
not a JTAG probe. Follow the target-focused [OpenOCD](../user/openocd.md)
workflow and its CH347 adapter requirements.

## Verification Boundary

Unit, contract, and browser tests identify local regressions only. Changes to
firmware behavior or hardware-interactive host control require the board-level
acceptance defined by the [HIL Functional Test Spec](../testing/hil-functional-test-spec.md).
Keep dated measurements and historical reports in `docs/testing/`; do not copy
them into skills or development troubleshooting notes.

## Related Documentation

- [Contributing](contributing.md) owns local validation, CI, and HIL policy.
- [Versioning](versioning.md) owns release and version-gate procedures.
- [Persistent Configuration](../reference/persistent-configuration.md) owns
  snapshot format and restore semantics.
- [Power Analyzer](../reference/power-analyzer.md) and
  [Sigrok Linkr v2](../reference/sigrok-linkr-v2.md) own their protocol detail.
