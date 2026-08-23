# Documentation

The canonical documentation tree for Radxa Linkr Debugger lives under
`docs/`. Six subtrees cover user-facing guides, developer-facing guides,
stable contracts, board-level testing, hardware design references, and
shared artwork. Start here when you want to find a specific document
or decide where a new one belongs.

## User

Hands-on guides for daily use of the board and host tools. Read these
if you are connecting a board, running captures, or operating the
firmware through MCP, CLI, TUI, Web UI, or curl.

- [User Guide](user/README.md): quick start, CLI, TUI, Web UI, OTA,
  OpenOCD, Logic Analyzer, Power Analyzer, troubleshooting
- [用户指南](user/README.zh-CN.md)

## Developer

Build, flash, contribute, and hardware mapping for people working on
Linkr itself. These documents describe Linkr's own development, not
how to operate a target device.

- [Developer Guide](developer/README.md): build, flash, contribute,
  hardware mapping, versioning
  - [Debugging](developer/debugging.md): diagnose and change Linkr itself
- [开发者指南](developer/README.zh-CN.md)
  - [调试](developer/debugging.zh-CN.md)

## Reference

Stable contracts and shared specifications that the firmware and host
honor. Treat these documents as the single source of truth for wire
shapes, persistence, capture behavior, and inter-service protocols.

- [Logic Analyzer](reference/logic-analyzer.md): packed arena, WIDE11
  capture, raw-TCP Sigrok
- [ADC Telemetry](reference/adc-telemetry.md): ADC3 wire shapes,
  GP29 ownership rules
- [Persistent Configuration](reference/persistent-configuration.md):
  firmware-owned v1 snapshot contract
- [Capture Trigger Architecture](reference/capture-trigger-architecture.md)
- [MCP Server](reference/mcp-server.md)
- [Serial Broker](reference/serial-broker.md)
- [Sigrok Linkr v1](reference/sigrok-linkr-v1.md)
- [Power Analyzer](reference/power-analyzer.md)
- [Ring Buffer Gap Analysis](reference/ring-buffer-gap-analysis.md)
- [Web UI Sigrok Bridge](reference/webui-sigrok-bridge.md)
- [OpenOCD](reference/openocd/README.md): example CH347 config

## Testing

Board-level functional-test specification and dated HIL reports. Local
unit tests and CI gates are not HIL; the documents here describe what
counts as HIL and where dated board-level evidence lives.

- [HIL Functional Test Spec](testing/hil-functional-test-spec.md):
  board-level test procedures and gates
- [Reports](testing/reports/): narrative test reports
- [Results](testing/results/): dated per-run HIL evidence

## Hardware

Schematics and design references for the G3 board.

- [Schematic (rev X1.1)](hardware/radxa-linkr-debugger-schematic-x1.1.pdf)
- [Reference Designators](hardware/RA051_X11_20260618位号图.pdf)

## Assets

Marketing and architecture artwork used across the documentation.

- [Promo Image](assets/marketing/radxa-linkr-debugger-promo.png)
- [Board Top View (SVG)](assets/marketing/radxa-linkr-debugger-board-top-view.svg)
- [Installation Flow](assets/architecture/current-installation-flow.png)
- [System Architecture](assets/architecture/current-system-architecture.png)