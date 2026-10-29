// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

use crate::app::write_json_error;
use crate::cli::{parse_timeout, Cli};
use crate::discovery::{self, DiscoveryBackend, DiscoveryReport, ResolutionError};
use crate::json_contract::JSON_SCHEMA;
use anyhow::Result;
use serde_json::json;
use std::io::Write;

pub fn run(
    cli: &Cli,
    backend: &impl DiscoveryBackend,
    stdout: &mut dyn Write,
    stderr: &mut dyn Write,
) -> Result<u8> {
    let json_output = cli.json || cli.command_args.iter().any(|arg| arg == "--json");
    let options = (|| {
        let mut timeout = cli.timeout;
        let mut args = cli.command_args.iter().skip(1);
        while let Some(arg) = args.next() {
            match arg.as_str() {
                "--json" => {}
                "--timeout" => {
                    timeout = parse_timeout(args.next().ok_or("--timeout requires a value")?)?;
                }
                option if option.starts_with("--timeout=") => {
                    timeout = parse_timeout(&option["--timeout=".len()..])?;
                }
                _ => {
                    return Err(
                        "usage: radxa-linkr-debuggerctl discover [--json] [--timeout TIMEOUT]"
                            .into(),
                    )
                }
            }
        }
        Ok::<_, String>(timeout)
    })();
    let timeout = match options {
        Ok(timeout) => timeout,
        Err(message) => {
            if json_output {
                write_json_error(stdout, "discover", "usage", &message)?;
            } else {
                writeln!(stderr, "{message}")?;
            }
            return Ok(2);
        }
    };
    let report = match discovery::discover(backend, timeout) {
        Ok(report) => report,
        Err(err) => {
            if json_output {
                write_json_error(stdout, "discover", "discovery_failed", &format!("{err:#}"))?;
            } else {
                writeln!(stderr, "discovery_failed: {err:#}")?;
            }
            return Ok(1);
        }
    };
    if json_output {
        writeln!(
            stdout,
            "{}",
            json!({
                "schema": JSON_SCHEMA, "ok": true, "command": "discover",
                "devices": report.devices, "failures": report.failures,
            })
        )?;
    } else {
        write_devices(&report, stdout)?;
        write_failures(&report, stderr)?;
        if report.devices.is_empty() {
            writeln!(stdout, "No verified Linkr devices found.")?;
        }
    }
    Ok(0)
}

pub fn write_resolution_error(
    failure: &ResolutionError,
    command: &str,
    json_output: bool,
    stdout: &mut dyn Write,
    stderr: &mut dyn Write,
) -> Result<u8> {
    if json_output {
        writeln!(
            stdout,
            "{}",
            json!({
                "schema": JSON_SCHEMA, "ok": false, "command": command,
                "error": failure.error,
                "devices": failure.report.devices, "failures": failure.report.failures,
            })
        )?;
    } else {
        writeln!(stderr, "{}: {}", failure.error.code, failure.error.message)?;
        write_devices(&failure.report, stderr)?;
        write_failures(&failure.report, stderr)?;
    }
    Ok(1)
}

fn write_devices(report: &DiscoveryReport, devices: &mut dyn Write) -> Result<()> {
    for device in &report.devices {
        writeln!(
            devices,
            "{} interface={} local_ip={} project={}",
            device.candidate.base_url,
            device.candidate.interface,
            device.candidate.local_ip,
            device.project
        )?;
        for (key, value) in [
            ("mcu", &device.mcu),
            ("usb", &device.usb),
            ("image_version", &device.image_version),
        ] {
            if let Some(value) = value {
                writeln!(devices, "  {key}={value}")?;
            }
        }
    }
    Ok(())
}

fn write_failures(report: &DiscoveryReport, failures: &mut dyn Write) -> Result<()> {
    for failure in &report.failures {
        writeln!(
            failures,
            "{} interface={}: {}: {}",
            failure.candidate.base_url,
            failure.candidate.interface,
            failure.error.code,
            failure.error.message
        )?;
    }
    Ok(())
}
