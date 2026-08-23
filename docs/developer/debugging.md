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
| Shared UART ownership, cursors, or archived RX data | [Serial Broker](../reference/serial-broker.md) |
| ADC, GP29, or telemetry wire shape | [ADC Telemetry](../reference/adc-telemetry.md) |
| Logic analyzer transport or capture semantics | [Logic Analyzer](../reference/logic-analyzer.md) |
| OpenOCD/JTAG adapter or target reset | [OpenOCD](../user/openocd.md) |
| Required board-level acceptance | [HIL Functional Test Spec](../testing/hil-functional-test-spec.md) |

## Browser And Bridge Changes

For a board-hosted page, a navigation or resource-load timeout identifies the
USB NCM, HTTP service, or browser setup boundary. An assertion failure after
the page renders identifies a Web UI regression. Preserve this distinction in
test reports and bug triage.

The board page is served over HTTP. Direct Web Serial therefore needs its
documented secure-origin override, a user gesture, and the browser chooser.
Browser automation can cover setup UI and bridge behavior, but it cannot
replace manual chooser acceptance or serial I/O. The supported bridge and
permissions paths are documented in [Web UI](../user/webui.md).

When a hosted Web page needs board access, diagnose the bridge process and its
REST, OTA, and WebSocket forwarding through that page before changing firmware
or protocol code. Do not document a development bridge command in the operator
skill.

## Control Plane Isolation

Use the firmware response envelope as the boundary between transport and
operation failures. A reachable endpoint with `ok: false` is a firmware
rejection; a timeout, connection refusal, or route failure is transport or USB
NCM state. Protocol fields and error meanings remain authoritative in the
[HTTP API](../user/api.md).

MCP and the Web UI share serial infrastructure. Diagnose cursor expiry, shared
subscriber behavior, exclusive writes, and RX archive completeness against the
[Serial Broker](../reference/serial-broker.md), not by adding host-side retry
or replay behavior. Mutating operations require a new operator decision after
an error.

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
  [Sigrok Linkr v1](../reference/sigrok-linkr-v1.md) own their protocol detail.
