# Developer Guide

[中文](README.zh-CN.md)

Welcome to the Radxa Linkr Debugger developer guide. This section covers everything you need to build, flash, and contribute to the project. For the full documentation tree, see the [Documentation Index](../README.md).

## Contents

- [Getting Started](getting-started.md) — Environment setup and first build
- [Build Firmware](build.md) — Detailed build instructions (Nix and manual)
- [Flashing](flashing.md) — BOOTSEL and OTA flashing procedures
- [Contributing](contributing.md) — Testing, HIL, CI, and coding conventions
- [Versioning](versioning.md) — Single-source version updates and release gates
- [Debugging](debugging.md) — Diagnose and change Linkr itself, starting from the failing layer
- [Hardware Mapping](hardware-mapping.md) — RP2350A pin assignments and schematic references

## Repository Layout

```text
apps/radxa_linkr_debugger/        Zephyr application
apps/radxa_linkr_debugger/src/    Firmware source and shared board model
apps/radxa_linkr_debugger/tests/  Unit tests
cmd-ng/                          Primary Rust host CLI/TUI
web/                             Web UI (served by Linkr Host on GitHub Pages)
docs/                            Documentation tree (user, developer, reference, testing, hardware, and assets)
skills/radxa-linkr-debugger/     Agent-facing skill and operating guide
west.yml                         Zephyr workspace manifest
```
