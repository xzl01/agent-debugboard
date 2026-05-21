---
name: agent-debugboard
description: Use the Agent DebugBoard host CLI to diagnose and operate target-board power rails, ADC current monitors, safe GPIOs, TF/SD routing, and RP2040 BOOTSEL mode. Use this when an Agent needs hardware-control access through agent-debugboardctl.
---

# Agent DebugBoard

Use the skill-local binary for Agent-side hardware control. Prefer JSON output for all automation. Do not parse human-readable command output when `--json` is available.

- macOS/Linux binary: `${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl`
- Windows binary: `%CLAUDE_SKILL_DIR%\scripts\bin\agent-debugboardctl.exe`

## First Checks

1. Check whether the skill-local binary exists and runs:

   macOS/Linux:

   ```sh
   ${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --version
   ```

   Windows CMD:

   ```bat
   "%CLAUDE_SKILL_DIR%\scripts\bin\agent-debugboardctl.exe" --version
   ```

2. Check whether the skill-local binary is new enough for Agent automation:

   macOS/Linux:

   ```sh
   ${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json doctor
   ```

   Windows CMD:

   ```bat
   "%CLAUDE_SKILL_DIR%\scripts\bin\agent-debugboardctl.exe" --json doctor
   ```

3. Treat these outcomes as follows:
   - Exit code `0` with `ok: true`: the CLI and board connection are ready.
   - Valid JSON with `ok: false`: read `error.code` and `error.message`; the skill-local binary is working, but the board or serial path needs attention.
   - Unknown `--json` or `doctor`: the built binary is too old. Rebuild from this checkout.
   - Binary is missing: run the matching installer from the repository checkout.

## Install/Build Repo-Local CLI

This skill uses only repo-local scripts and binaries. Do not modify `PATH`, shell
profiles, or global install directories.

Install the CLI into the repo-local output directory.

macOS/Linux:

```sh
${CLAUDE_SKILL_DIR}/scripts/install.sh
```

Windows PowerShell:

```powershell
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "${env:CLAUDE_SKILL_DIR}\scripts\install.ps1"
```

When the current repository checkout contains `go.mod` and
`cmd/agent-debugboardctl`, and `go` is available, the helper builds from source
with `go build -trimpath`. Otherwise the same skill-local installer downloads
and verifies a release artifact. In both cases it installs only to:

```text
skills/agent-debugboard/scripts/bin/agent-debugboardctl
skills/agent-debugboard/scripts/bin/agent-debugboardctl.exe
```

After installation, run the matching binary.

macOS/Linux:

```sh
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --version
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json doctor
```

Windows CMD:

```bat
"%CLAUDE_SKILL_DIR%\scripts\bin\agent-debugboardctl.exe" --version
"%CLAUDE_SKILL_DIR%\scripts\bin\agent-debugboardctl.exe" --json doctor
```

The helper never modifies `PATH`, shell profiles, or global install locations.

## JSON Contract

Agent automation should expect the top-level fields:

- `schema`: must be `agent-debugboard.v1`
- `ok`: boolean success flag
- `command`: command name
- `error`: present on failure, with `code` and `message`

If `ok` is `false`, do not infer success from partial fields. Handle `error.code` first.

## Common Commands

Diagnose board connection:

```sh
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json doctor
```

Read full board state:

```sh
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json status
```

List power rails:

```sh
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json rail list
```

Control power rails:

```sh
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json rail set 12v_out on
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json rail set 12v_out off
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json rail set 5v_out on
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json rail set 5v_out off
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json rail set 5v_ws on
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json rail set 20v_out on
```

Restart a target board with its normal software reboot or reset interface first.
Use rail power-cycling only as a hard-restart fallback when soft reboot/reset is
unavailable, the target is unresponsive, or no reset line is exposed. Confirm
the rail name first, then turn it off, wait briefly for discharge, and turn it
back on:

```sh
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json rail set 5v_out off
sleep 2
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json rail set 5v_out on
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json rail get 5v_out
```

Use the rail that actually powers the target, for example `5v_out`, `12v_out`,
or `20v_out`. Do not power-cycle unrelated rails.

Read ADC current monitors:

```sh
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json adc read
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json adc read 5v_out
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json adc read 12v_out
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json adc read 20v_out
```

For manual calibration or hardware debugging, use verbose human-readable ADC
output to inspect raw fields:

```sh
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl adc read -v 5v_out
```

Do not parse this text in automation. Agents should use `--json adc read ...`
and consume `readings[].ma_est`, `readings[].raw`, and `readings[].mv`.

Switch TF/SD route:

```sh
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json sd get
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json sd route target
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json sd route usb-reader
```

Use allowlisted GPIOs:

```sh
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json gpio list
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json gpio get GP13
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json gpio set GP13 1
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json gpio input GP13
```

Enter RP2040 BOOTSEL mode for flashing:

```sh
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl bootloader
```

## OpenOCD / JTAG Workflow

Use OpenOCD through the onboard CH347F path when the target board exposes
JTAG/SWD through the debug fixture. CH347F is wired directly to the target debug
connector. The RP2040 firmware controls target power and recovery lines; it does
not sit in the JTAG/SWD path and does not act as a CMSIS-DAP, Picoprobe, or JTAG
probe.

First check OpenOCD availability:

```sh
openocd --version
```

If OpenOCD is missing, install it with the host OS package manager, for example
`brew install open-ocd` on macOS or `sudo apt-get install openocd` on
Ubuntu/Debian.

Power the target first, then start OpenOCD with the CH347F interface script
available in the host OpenOCD installation and the target config for the board
under test:

```sh
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json rail set 5v_out on
${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json rail get 5v_out
openocd -f interface/<ch347-interface>.cfg -f target/<target>.cfg
```

CH347F support depends on the OpenOCD build. If the system OpenOCD package does
not include a CH347F interface script, use the WCH/vendor OpenOCD build or add
the matching interface script.

Use the rail that actually powers the target. If the target uses `12v_out` or
`20v_out`, replace `5v_out` accordingly.

When a reset is needed, prefer a target software reboot or OpenOCD reset command
first:

```text
reset halt
reset run
```

Only use rail power-cycling as a hard-restart fallback when soft reset is not
available or the target is unresponsive.

## Safety Rules

- Prefer `--json` for all non-interactive use.
- Do not use `--raw` unless debugging the firmware shell protocol itself.
- Treat `rail set`, `gpio set`, `gpio input`, `sd route`, and `bootloader` as side-effectful operations. Confirm the target and desired state before running them.
- Prefer soft reboot/reset for target-board restarts. Treat rail power-cycling as a hard-restart fallback that is destructive to target runtime state. Confirm the exact rail and only cycle the rail powering the target.
- `5V_FIN` is an input/source rail. Do not present it as a controllable output.
- Only use allowlisted GPIOs reported by `${CLAUDE_SKILL_DIR}/scripts/bin/agent-debugboardctl --json gpio list`.
- Do not expose board-internal schematic codenames in user-facing output.

## ADC Calibration Notes

- `5v_out` uses a fixed piecewise-linear calibration table from local load
  measurements. The table covers approximately `0A` through `4.3A`; readings at
  and below the 5V zero point are reported as `0mA`.
- `12v_out` and `20v_out` currently use the ideal INA139 linear model
  (`ma_per_mv=50`) until each rail is measured and calibrated. Treat these
  channels as approximate, not precision current measurements.
- When a controllable rail is off, firmware reports `ma_est=0` for that rail
  while still exposing `raw` and `mv` in JSON/verbose output for diagnostics.
