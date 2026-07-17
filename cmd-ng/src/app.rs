// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

use crate::adc;
use crate::cli::Cli;
use crate::client::{
    resolve_base_url, BoardBinaryUpload, BoardClient, BoardRequest, BoardTransport,
    DEFAULT_BASE_URL,
};
use crate::json_contract::{parse_envelope, render_failure, EnvelopeError, JsonError, JSON_SCHEMA};
use crate::tui;
use anyhow::Result;
use clap::{error::ErrorKind, Parser};
use reqwest::Method;
use serde::Serialize;
use serde_json::{json, Value};
use std::ffi::OsString;
use std::fs::File;
use std::io::{self, Write};
use std::path::Path;
use std::time::Duration;

const INTERNAL_POWER_OUTPUT: &str = "5v_ws";

fn version() -> &'static str {
    env!("CARGO_PKG_VERSION")
}

#[derive(Serialize, Debug)]
struct DoctorResult {
    schema: &'static str,
    ok: bool,
    command: &'static str,
    cli_version: &'static str,
    base_url: String,
    probe_ok: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    status: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    status_text: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    error: Option<JsonError>,
}

pub trait TuiRunner {
    fn run_tui(
        &self,
        base_url: &str,
        timeout: Duration,
        stdout: &mut dyn Write,
        stderr: &mut dyn Write,
    ) -> Result<u8>;
}

pub struct DefaultTuiRunner;

impl TuiRunner for DefaultTuiRunner {
    fn run_tui(
        &self,
        base_url: &str,
        timeout: Duration,
        _stdout: &mut dyn Write,
        _stderr: &mut dyn Write,
    ) -> Result<u8> {
        let client = BoardClient::new(base_url, timeout)?;
        tui::run_tui(client, base_url.to_string(), timeout)
    }
}

pub fn run<I, T>(args: I) -> Result<u8>
where
    I: IntoIterator<Item = T>,
    T: Into<OsString> + Clone,
{
    let stdout = io::stdout();
    let stderr = io::stderr();
    let mut stdout = stdout.lock();
    let mut stderr = stderr.lock();
    run_with_io(args, &mut stdout, &mut stderr)
}

fn run_with_io<I, T>(args: I, stdout: &mut dyn Write, stderr: &mut dyn Write) -> Result<u8>
where
    I: IntoIterator<Item = T>,
    T: Into<OsString> + Clone,
{
    let cli = match Cli::try_parse_from(args) {
        Ok(cli) => cli,
        Err(err) => {
            let code = if err.kind() == ErrorKind::DisplayHelp {
                0
            } else {
                2
            };
            if err.kind() == ErrorKind::DisplayHelp {
                write_usage(stderr)?;
            } else {
                write!(stderr, "{err}")?;
            }
            return Ok(code);
        }
    };

    let base_url = base_url_from_cli(&cli);
    let client = BoardClient::new(&base_url, cli.timeout)?;
    let tui = DefaultTuiRunner;
    execute_with_io(cli, &client, &tui, stdout, stderr)
}

fn execute_with_io<TClient, TTui>(
    cli: Cli,
    client: &TClient,
    tui_runner: &TTui,
    stdout: &mut dyn Write,
    stderr: &mut dyn Write,
) -> Result<u8>
where
    TClient: BoardTransport,
    TTui: TuiRunner,
{
    let base_url = base_url_from_cli(&cli);

    if cli.version {
        if cli.json {
            write_json(
                stdout,
                &json!({
                    "schema": JSON_SCHEMA,
                    "ok": true,
                    "command": "version",
                    "version": version(),
                }),
            )?;
        } else {
            writeln!(stdout, "radxa-linkr-debuggerctl {}", version())?;
        }
        return Ok(0);
    }

    if cli.command_args.is_empty() {
        if !cli.raw && !cli.json {
            return tui_runner.run_tui(&base_url, cli.timeout, stdout, stderr);
        }
        return missing_command(cli.json, stdout, stderr);
    }

    if cli.raw {
        return unsupported_raw(cli.json, stdout, stderr);
    }

    execute_command(&cli, client, stdout, stderr)
}

fn execute_command<TClient>(
    cli: &Cli,
    client: &TClient,
    stdout: &mut dyn Write,
    stderr: &mut dyn Write,
) -> Result<u8>
where
    TClient: BoardTransport,
{
    if !cli.raw && cli.command_args.first().map(String::as_str) == Some("doctor") {
        if cli.command_args.len() != 1 {
            if cli.json {
                write_json_error(
                    stdout,
                    "doctor",
                    "usage",
                    "usage: radxa-linkr-debuggerctl doctor",
                )?;
            } else {
                writeln!(stderr, "usage: radxa-linkr-debuggerctl doctor")?;
            }
            return Ok(2);
        }
        return run_doctor(client, cli.json, stdout, stderr);
    }

    if !cli.raw && is_switch_usb_route_command(&cli.command_args) {
        return run_switch_usb(client, &cli.command_args, cli.json, stdout, stderr);
    }

    if !cli.raw && is_switch_vin_route_command(&cli.command_args) {
        return run_switch_vin(client, &cli.command_args, cli.json, stdout, stderr);
    }

    let adc_read_command = is_adc_read_command(&cli.command_args);
    let adc_verbose = adc_read_command
        && (cli.verbose
            || has_arg(&cli.command_args, "-v")
            || has_arg(&cli.command_args, "--verbose"));

    let mut wire_args = cli.command_args.clone();
    if cli.verbose
        && !cli.json
        && !adc_read_command
        && !has_arg(&wire_args, "-v")
        && !has_arg(&wire_args, "--verbose")
    {
        wire_args.push("-v".to_string());
    }
    if (cli.json || adc_read_command) && !has_arg(&wire_args, "--json") {
        wire_args.push("--json".to_string());
    }

    if is_adc_record_command(&cli.command_args) {
        return run_adc_record(
            client.base_url(),
            cli.timeout,
            &cli.command_args,
            stdout,
            stderr,
        );
    }

    if is_ota_upload_command(&cli.command_args) {
        return run_ota_upload(client, &cli.command_args, cli.json, stdout, stderr);
    }

    let request = match request_from_args(&wire_args) {
        Ok(request) => request,
        Err(err) => {
            if cli.json {
                write_json_error(stdout, command_name(&cli.command_args), "usage", &err)?;
            } else {
                writeln!(stderr, "{err}")?;
            }
            return Ok(2);
        }
    };

    if adc_read_command {
        let channel = extract_adc_channel(&wire_args);
        return run_adc(client, channel, cli.json, adc_verbose, stdout, stderr);
    }

    run_standard(
        client,
        command_name(&cli.command_args),
        request,
        cli.json,
        stdout,
        stderr,
    )
}

fn run_standard<TClient>(
    client: &TClient,
    command: &str,
    request: BoardRequest,
    json_output: bool,
    stdout: &mut dyn Write,
    stderr: &mut dyn Write,
) -> Result<u8>
where
    TClient: BoardTransport,
{
    let output = match client.send_text(request) {
        Ok(output) => output,
        Err(err) => {
            if json_output {
                write_json_error(stdout, command, "transport_error", &err.to_string())?;
            } else {
                writeln!(stderr, "{err}")?;
            }
            return Ok(1);
        }
    };

    if json_output || looks_like_json(&output) {
        match parse_envelope(&output, true) {
            Ok(envelope) => {
                let filtered = filter_internal_power_output(&output)?;
                writeln!(stdout, "{}", filtered.as_deref().unwrap_or(output.trim()))?;
                return Ok(if envelope.ok == Some(false) { 1 } else { 0 });
            }
            Err(err) => {
                write_json_error(stdout, command, "invalid_json", &err.to_string())?;
                return Ok(1);
            }
        }
    }

    if !output.trim().is_empty() {
        writeln!(stdout, "{}", output.trim())?;
    }
    Ok(0)
}

fn run_adc<TClient>(
    client: &TClient,
    channel: Option<String>,
    json_output: bool,
    verbose: bool,
    stdout: &mut dyn Write,
    stderr: &mut dyn Write,
) -> Result<u8>
where
    TClient: BoardTransport,
{
    let mut query = Vec::new();
    if let Some(name) = channel {
        query.push(("channel".to_string(), name));
    }
    let output = match client.send_text(BoardRequest {
        method: Method::GET,
        path: "/api/v1/adc/read".to_string(),
        query,
        body: None,
    }) {
        Ok(output) => output,
        Err(err) => {
            if json_output {
                write_json_error(stdout, "adc", "transport_error", &err.to_string())?;
            } else {
                writeln!(stderr, "{err}")?;
            }
            return Ok(1);
        }
    };

    if json_output {
        match adc::write_json(&output, "adc") {
            Ok(body) => {
                writeln!(stdout, "{body}")?;
                return Ok(
                    if parse_envelope(&output, true).ok().and_then(|env| env.ok) == Some(false) {
                        1
                    } else {
                        0
                    },
                );
            }
            Err(err) => {
                write_json_error(stdout, "adc", "invalid_json", &err.to_string())?;
                return Ok(1);
            }
        }
    }

    match adc::write_text(&output, verbose) {
        Ok(text) => {
            if !text.is_empty() {
                writeln!(stdout, "{text}")?;
            }
            Ok(0)
        }
        Err(err) => {
            writeln!(stderr, "{err}")?;
            Ok(1)
        }
    }
}

fn run_doctor<TClient>(
    client: &TClient,
    json_output: bool,
    stdout: &mut dyn Write,
    stderr: &mut dyn Write,
) -> Result<u8>
where
    TClient: BoardTransport,
{
    let mut result = DoctorResult {
        schema: JSON_SCHEMA,
        ok: false,
        command: "doctor",
        cli_version: version(),
        base_url: client.base_url().to_string(),
        probe_ok: false,
        status: None,
        status_text: None,
        error: None,
    };

    match client.send_text(BoardRequest {
        method: Method::GET,
        path: "/api/v1/status".to_string(),
        query: vec![],
        body: None,
    }) {
        Ok(output) => match parse_envelope(&output, true) {
            Ok(envelope) => {
                result.status = serde_json::from_str::<Value>(&output)
                    .ok()
                    .map(|mut status| {
                        remove_internal_power_output(&mut status);
                        status
                    });
                if envelope.ok == Some(false) {
                    result.error = envelope.error;
                } else {
                    result.ok = true;
                    result.probe_ok = true;
                }
            }
            Err(err) => {
                result.status_text = Some(output.trim().to_string());
                result.error = Some(error_from_envelope(err));
            }
        },
        Err(err) => {
            result.error = Some(JsonError {
                code: "status_failed".to_string(),
                message: err.to_string(),
            });
        }
    }

    finish_doctor(stdout, stderr, result, json_output)
}

fn finish_doctor(
    stdout: &mut dyn Write,
    stderr: &mut dyn Write,
    result: DoctorResult,
    json_output: bool,
) -> Result<u8> {
    let exit_code = if result.ok { 0 } else { 1 };
    if json_output {
        write_json(stdout, &result)?;
        return Ok(exit_code);
    }

    writeln!(stdout, "Radxa Linkr Debugger doctor")?;
    writeln!(stdout, "cli_version={}", result.cli_version)?;
    writeln!(stdout, "base_url={}", result.base_url)?;
    writeln!(
        stdout,
        "probe={}",
        if result.probe_ok { "ok" } else { "failed" }
    )?;
    if let Some(status) = &result.status {
        writeln!(stdout, "status={status}")?;
    }
    if let Some(status_text) = &result.status_text {
        writeln!(stdout, "status_text={status_text}")?;
    }
    if let Some(error) = &result.error {
        writeln!(stdout, "error={}: {}", error.code, error.message)?;
        if exit_code != 0 {
            writeln!(stderr, "{}: {}", error.code, error.message)?;
        }
    } else {
        writeln!(stdout, "result=ok")?;
    }
    Ok(exit_code)
}

fn base_url_from_cli(cli: &Cli) -> String {
    if let Some(url) = cli.url.as_deref() {
        return resolve_base_url(url);
    }
    if let Some(addr) = cli.addr.as_deref() {
        return resolve_base_url(addr);
    }
    if let Some(port) = cli.port.as_deref() {
        return resolve_base_url(port);
    }
    DEFAULT_BASE_URL.to_string()
}

fn has_arg(args: &[String], value: &str) -> bool {
    args.iter().any(|arg| arg == value)
}

fn is_adc_read_command(args: &[String]) -> bool {
    let cleaned = strip_passthrough_flags(args);
    cleaned.first().map(String::as_str) == Some("adc")
        && (cleaned.len() == 1 || cleaned.get(1).map(String::as_str) == Some("read"))
}

fn is_adc_record_command(args: &[String]) -> bool {
    let cleaned = strip_passthrough_flags(args);
    cleaned.first().map(String::as_str) == Some("adc")
        && cleaned.get(1).map(String::as_str) == Some("record")
}

fn is_ota_upload_command(args: &[String]) -> bool {
    let cleaned = strip_passthrough_flags(args);
    cleaned.first().map(String::as_str) == Some("ota")
        && cleaned.get(1).map(String::as_str) == Some("upload")
}

fn strip_passthrough_flags(args: &[String]) -> Vec<String> {
    args.iter()
        .filter(|arg| {
            arg.as_str() != "--json" && arg.as_str() != "-v" && arg.as_str() != "--verbose"
        })
        .cloned()
        .collect()
}

fn command_name(args: &[String]) -> &str {
    args.first()
        .map(String::as_str)
        .unwrap_or("radxa-linkr-debuggerctl")
}

fn extract_adc_channel(args: &[String]) -> Option<String> {
    let cleaned = strip_passthrough_flags(args);
    if cleaned.len() >= 3 {
        return cleaned.get(2).cloned();
    }
    None
}

fn run_adc_record(
    base_url: &str,
    timeout: Duration,
    args: &[String],
    stdout: &mut dyn Write,
    stderr: &mut dyn Write,
) -> Result<u8> {
    let cleaned = strip_passthrough_flags(args);
    let usage =
        "usage: radxa-linkr-debuggerctl adc record OUTPUT_PATH [MAX_SAMPLES] [--rate-hz HZ]";
    if cleaned.len() < 3 {
        writeln!(stderr, "{usage}")?;
        return Ok(2);
    }

    let parsed = match parse_adc_record_args(&cleaned) {
        Ok(parsed) => parsed,
        Err(err) => {
            writeln!(stderr, "{err}")?;
            return Ok(2);
        }
    };
    crate::recorder::record_adc_ws_to_file(
        base_url,
        timeout,
        &parsed.output,
        parsed.max_samples,
        parsed.rate_hz,
    )?;
    writeln!(stdout, "recorded adc websocket stream to {}", parsed.output)?;
    Ok(0)
}

fn run_ota_upload<TClient>(
    client: &TClient,
    args: &[String],
    json_output: bool,
    stdout: &mut dyn Write,
    stderr: &mut dyn Write,
) -> Result<u8>
where
    TClient: BoardTransport,
{
    let cleaned = strip_passthrough_flags(args);
    let usage = "usage: radxa-linkr-debuggerctl ota upload PATH";
    if cleaned.len() != 3 {
        if json_output {
            write_json_error(stdout, "ota", "usage", usage)?;
        } else {
            writeln!(stderr, "{usage}")?;
        }
        return Ok(2);
    }

    let upload = match prepare_ota_upload(&cleaned[2]) {
        Ok(upload) => upload,
        Err(err) => {
            if json_output {
                write_json_error(stdout, "ota", "invalid_file", &err)?;
            } else {
                writeln!(stderr, "{err}")?;
            }
            return Ok(2);
        }
    };
    let size = upload.size;
    let sha256 = upload.sha256.clone();

    let output = match client.upload_binary(upload) {
        Ok(output) => output,
        Err(err) => {
            if json_output {
                write_json_error(stdout, "ota", "transport_error", &err.to_string())?;
            } else {
                writeln!(stderr, "{err}")?;
            }
            return Ok(1);
        }
    };

    match parse_envelope(&output, true) {
        Ok(envelope) => {
            if json_output {
                writeln!(stdout, "{}", output.trim())?;
            } else if envelope.ok == Some(false) {
                if let Some(error) = envelope.error {
                    writeln!(stderr, "{}: {}", error.code, error.message)?;
                } else {
                    writeln!(stderr, "ota upload failed")?;
                }
            } else {
                writeln!(stdout, "ota upload ok: {size} bytes sha256={sha256}")?;
            }
            Ok(if envelope.ok == Some(false) { 1 } else { 0 })
        }
        Err(err) => {
            if json_output {
                write_json_error(stdout, "ota", "invalid_json", &err.to_string())?;
            } else {
                writeln!(stderr, "{err}")?;
            }
            Ok(1)
        }
    }
}

fn prepare_ota_upload(path: &str) -> Result<BoardBinaryUpload, String> {
    let path_ref = Path::new(path);
    let metadata = std::fs::metadata(path_ref)
        .map_err(|err| format!("cannot access OTA image {path:?}: {err}"))?;
    if !metadata.is_file() {
        return Err(format!("OTA image {path:?} is not a regular file"));
    }
    let size = metadata.len();
    if size == 0 {
        return Err(format!("OTA image {path:?} is empty"));
    }

    let sha256 = sha256_file(path_ref)?;
    let file =
        File::open(path_ref).map_err(|err| format!("cannot open OTA image {path:?}: {err}"))?;
    Ok(BoardBinaryUpload {
        path: "/api/v1/ota/upload".to_string(),
        file,
        size,
        sha256,
    })
}

fn sha256_file(path: &Path) -> Result<String, String> {
    use sha2::{Digest, Sha256};
    use std::io::Read;

    let mut file = File::open(path)
        .map_err(|err| format!("cannot open OTA image {}: {err}", path.display()))?;
    let mut hasher = Sha256::new();
    let mut buffer = [0u8; 64 * 1024];
    loop {
        let n = file
            .read(&mut buffer)
            .map_err(|err| format!("cannot read OTA image {}: {err}", path.display()))?;
        if n == 0 {
            break;
        }
        hasher.update(&buffer[..n]);
    }
    Ok(hex_lower(&hasher.finalize()))
}

fn hex_lower(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        out.push(HEX[(byte >> 4) as usize] as char);
        out.push(HEX[(byte & 0x0f) as usize] as char);
    }
    out
}

#[derive(Debug, PartialEq, Eq)]
struct AdcRecordArgs {
    output: String,
    max_samples: Option<usize>,
    rate_hz: i32,
}

fn parse_adc_record_args(cleaned: &[String]) -> Result<AdcRecordArgs, String> {
    let usage =
        "usage: radxa-linkr-debuggerctl adc record OUTPUT_PATH [MAX_SAMPLES] [--rate-hz HZ]";
    if cleaned.len() < 3 {
        return Err(usage.to_string());
    }

    let output = cleaned[2].clone();
    let mut positional = Vec::new();
    let mut rate_hz = crate::recorder::DEFAULT_ADC_RECORD_RATE_HZ;
    let mut index = 3usize;
    while index < cleaned.len() {
        match cleaned[index].as_str() {
            "--rate-hz" => {
                let Some(value) = cleaned.get(index + 1) else {
                    return Err(usage.to_string());
                };
                rate_hz = parse_adc_record_rate_hz(value)?;
                index += 2;
            }
            other if other.starts_with("--rate-hz=") => {
                let value = other.trim_start_matches("--rate-hz=");
                rate_hz = parse_adc_record_rate_hz(value)?;
                index += 1;
            }
            other if other.starts_with('-') => {
                return Err(format!("unsupported adc record option {other:?}"));
            }
            other => {
                positional.push(other.to_string());
                index += 1;
            }
        }
    }
    if positional.len() > 1 {
        return Err(usage.to_string());
    }

    let max_samples = match positional.first() {
        Some(value) => match value.parse::<usize>() {
            Ok(max_samples) if max_samples > 0 => Some(max_samples),
            _ => return Err("MAX_SAMPLES must be a positive integer".to_string()),
        },
        None => None,
    };

    Ok(AdcRecordArgs {
        output,
        max_samples,
        rate_hz,
    })
}

fn parse_adc_record_rate_hz(value: &str) -> Result<i32, String> {
    let rate_hz: i32 = value
        .parse()
        .map_err(|_| "--rate-hz must be a positive integer up to 1000".to_string())?;
    if !(1..=1000).contains(&rate_hz) {
        return Err("--rate-hz must be a positive integer up to 1000".to_string());
    }
    Ok(rate_hz)
}

fn request_from_args(args: &[String]) -> Result<BoardRequest, String> {
    let cleaned = strip_passthrough_flags(args);
    if cleaned.is_empty() {
        return Err("missing command".to_string());
    }

    match cleaned[0].as_str() {
        "status" => {
            if cleaned.len() != 1 {
                return Err("usage: radxa-linkr-debuggerctl status".to_string());
            }
            Ok(get_request("/api/v1/status"))
        }
        "power" => power_request(&cleaned),
        "switch" => switch_request(&cleaned),
        "adc" => adc_request(&cleaned),
        "ota" => ota_request(&cleaned),
        "gpio" => gpio_request(&cleaned),
        "watchdog" => watchdog_request(&cleaned),
        "bootloader" => {
            if cleaned.len() != 1 {
                return Err("usage: radxa-linkr-debuggerctl bootloader".to_string());
            }
            Ok(post_request("/api/v1/bootloader"))
        }
        other => Err(format!("unsupported command {:?} over HTTP", other)),
    }
}

fn power_request(args: &[String]) -> Result<BoardRequest, String> {
    if args.len() < 2 {
        return Err("usage: radxa-linkr-debuggerctl power list|get|set ...".to_string());
    }
    match args[1].as_str() {
        "list" => {
            if args.len() != 2 {
                return Err("usage: radxa-linkr-debuggerctl power list".to_string());
            }
            Ok(get_request("/api/v1/power"))
        }
        "get" => {
            if args.len() != 3 {
                return Err("usage: radxa-linkr-debuggerctl power get NAME".to_string());
            }
            reject_internal_power_output(&args[2])?;
            Ok(get_request(format!("/api/v1/power/{}", args[2])))
        }
        "set" => {
            if args.len() != 4 {
                return Err("usage: radxa-linkr-debuggerctl power set NAME on|off".to_string());
            }
            reject_internal_power_output(&args[2])?;
            Ok(put_request(
                format!("/api/v1/power/{}", args[2]),
                json!({ "state": args[3] }),
            ))
        }
        other => Err(format!("unsupported power action {:?}", other)),
    }
}

fn reject_internal_power_output(name: &str) -> Result<(), String> {
    if name == INTERNAL_POWER_OUTPUT {
        return Err(format!(
            "power output {INTERNAL_POWER_OUTPUT:?} is internal and unavailable through the CLI"
        ));
    }
    Ok(())
}

fn filter_internal_power_output(output: &str) -> Result<Option<String>> {
    if !output.contains(INTERNAL_POWER_OUTPUT) {
        return Ok(None);
    }

    let mut value: Value = serde_json::from_str(output)?;
    if remove_internal_power_output(&mut value) {
        return Ok(Some(serde_json::to_string(&value)?));
    }
    Ok(None)
}

fn remove_internal_power_output(value: &mut Value) -> bool {
    match value {
        Value::Object(object) => {
            let mut changed = false;
            if let Some(Value::Array(outputs)) = object.get_mut("power_outputs") {
                let original_len = outputs.len();
                outputs.retain(|output| {
                    output.get("name").and_then(Value::as_str) != Some(INTERNAL_POWER_OUTPUT)
                });
                changed |= outputs.len() != original_len;
            }
            for child in object.values_mut() {
                changed |= remove_internal_power_output(child);
            }
            changed
        }
        Value::Array(values) => {
            let mut changed = false;
            for child in values {
                changed |= remove_internal_power_output(child);
            }
            changed
        }
        _ => false,
    }
}

fn adc_request(args: &[String]) -> Result<BoardRequest, String> {
    if args.len() < 2 || args[1] != "read" {
        return Err("usage: radxa-linkr-debuggerctl adc read [NAME]".to_string());
    }
    if args.len() > 3 {
        return Err("usage: radxa-linkr-debuggerctl adc read [NAME]".to_string());
    }
    let mut query = Vec::new();
    if let Some(name) = args.get(2) {
        query.push(("channel".to_string(), name.clone()));
    }
    Ok(get_request_with_query("/api/v1/adc/read", query))
}

fn switch_request(args: &[String]) -> Result<BoardRequest, String> {
    if args.len() < 2 {
        return Err("usage: radxa-linkr-debuggerctl switch list|get|route ...".to_string());
    }
    match args[1].as_str() {
        "list" => {
            if args.len() != 2 {
                return Err("usage: radxa-linkr-debuggerctl switch list".to_string());
            }
            Ok(get_request("/api/v1/switch"))
        }
        "get" => {
            if args.len() != 3 {
                return Err("usage: radxa-linkr-debuggerctl switch get sd|usb|vin".to_string());
            }
            Ok(get_request(format!("/api/v1/switch/{}", args[2])))
        }
        "route" => {
            if args.len() != 4 {
                return Err(
                    "usage: radxa-linkr-debuggerctl switch route sd|usb|vin ROUTE".to_string(),
                );
            }
            Ok(put_request(
                format!("/api/v1/switch/{}", args[2]),
                json!({ "route": args[3] }),
            ))
        }
        other => Err(format!("unsupported switch action {:?}", other)),
    }
}

fn is_switch_usb_route_command(args: &[String]) -> bool {
    let cleaned = strip_passthrough_flags(args);
    cleaned.len() >= 4 && cleaned[0] == "switch" && cleaned[1] == "route" && cleaned[2] == "usb"
}

fn is_switch_vin_route_command(args: &[String]) -> bool {
    let cleaned = strip_passthrough_flags(args);
    cleaned.len() >= 4 && cleaned[0] == "switch" && cleaned[1] == "route" && cleaned[2] == "vin"
}

fn run_switch_usb<TClient>(
    client: &TClient,
    args: &[String],
    json_output: bool,
    stdout: &mut dyn Write,
    stderr: &mut dyn Write,
) -> Result<u8>
where
    TClient: BoardTransport,
{
    let has_confirm = args.iter().any(|a| a == "--confirm" || a == "-y");
    if !has_confirm {
        if json_output {
            write_json_error(
                stdout,
                "switch",
                "confirm_required",
                "switch route usb requires --confirm. USB route switching is a dangerous operation that may affect target board connectivity.",
            )?;
        } else {
            writeln!(
                stderr,
                "switch route usb: dangerous operation requires --confirm.\nusage: radxa-linkr-debuggerctl switch route usb pc|target --confirm"
            )?;
        }
        return Ok(2);
    }

    let switch_args: Vec<String> = args
        .iter()
        .filter(|a| *a != "--confirm" && *a != "-y")
        .cloned()
        .collect();

    let target_route = switch_args.get(3).map(String::as_str).unwrap_or("");
    if switch_args.len() != 4 || !matches!(target_route, "pc" | "target") {
        let message = "usage: radxa-linkr-debuggerctl switch route usb pc|target --confirm";
        if json_output {
            write_json_error(stdout, "switch", "usage", message)?;
        } else {
            writeln!(stderr, "{message}")?;
        }
        return Ok(2);
    }
    match request_from_args(&switch_args) {
        Ok(request) => run_standard(
            client,
            command_name(&switch_args),
            request,
            json_output,
            stdout,
            stderr,
        ),
        Err(err) => {
            if json_output {
                write_json_error(stdout, "switch", "usage", &err)?;
            } else {
                writeln!(stderr, "{err}")?;
            }
            Ok(2)
        }
    }
}

fn run_switch_vin<TClient>(
    client: &TClient,
    args: &[String],
    json_output: bool,
    stdout: &mut dyn Write,
    stderr: &mut dyn Write,
) -> Result<u8>
where
    TClient: BoardTransport,
{
    let has_confirm = args.iter().any(|arg| arg == "--confirm" || arg == "-y");
    if !has_confirm {
        if json_output {
            write_json_error(
                stdout,
                "switch",
                "confirm_required",
                "switch route vin requires --confirm because it changes the CH347 I/O voltage.",
            )?;
        } else {
            writeln!(
                stderr,
                "switch route vin: voltage change requires --confirm.\nusage: radxa-linkr-debuggerctl switch route vin 1.8v|3.3v --confirm"
            )?;
        }
        return Ok(2);
    }

    let switch_args: Vec<String> = args
        .iter()
        .filter(|arg| *arg != "--confirm" && *arg != "-y")
        .cloned()
        .collect();
    let route = switch_args.get(3).map(String::as_str).unwrap_or("");
    if switch_args.len() != 4 || !matches!(route, "1.8v" | "3.3v") {
        let message = "usage: radxa-linkr-debuggerctl switch route vin 1.8v|3.3v --confirm";
        if json_output {
            write_json_error(stdout, "switch", "usage", message)?;
        } else {
            writeln!(stderr, "{message}")?;
        }
        return Ok(2);
    }

    match switch_request(&switch_args) {
        Ok(request) => run_standard(client, "switch", request, json_output, stdout, stderr),
        Err(err) => {
            if json_output {
                write_json_error(stdout, "switch", "usage", &err)?;
            } else {
                writeln!(stderr, "{err}")?;
            }
            Ok(2)
        }
    }
}

fn gpio_request(args: &[String]) -> Result<BoardRequest, String> {
    if args.len() < 2 {
        return Err("usage: radxa-linkr-debuggerctl gpio list|set|input ...".to_string());
    }
    match args[1].as_str() {
        "list" => {
            if args.len() != 2 {
                return Err("usage: radxa-linkr-debuggerctl gpio list".to_string());
            }
            Ok(get_request("/api/v1/gpio"))
        }
        "input" => {
            if args.len() != 3 {
                return Err("usage: radxa-linkr-debuggerctl gpio input NAME".to_string());
            }
            Ok(put_request(
                format!("/api/v1/gpio/{}", args[2]),
                json!({ "direction": "input" }),
            ))
        }
        "set" => {
            if args.len() != 4 {
                return Err("usage: radxa-linkr-debuggerctl gpio set NAME 0|1".to_string());
            }
            let value = args[3].parse::<i32>().unwrap_or(0);
            Ok(put_request(
                format!("/api/v1/gpio/{}", args[2]),
                json!({ "direction": "output", "value": value }),
            ))
        }
        other => Err(format!("unsupported gpio action {:?}", other)),
    }
}

fn watchdog_request(args: &[String]) -> Result<BoardRequest, String> {
    if args.len() != 2 {
        return Err("usage: radxa-linkr-debuggerctl watchdog status".to_string());
    }
    match args[1].as_str() {
        "status" => Ok(get_request("/api/v1/watchdog")),
        other => Err(format!(
            "unsupported watchdog action {:?} (watchdog is supervised by firmware)",
            other
        )),
    }
}

fn ota_request(args: &[String]) -> Result<BoardRequest, String> {
    if args.len() < 2 {
        return Err(
            "usage: radxa-linkr-debuggerctl ota status|upload|test|confirm ...".to_string(),
        );
    }
    match args[1].as_str() {
        "status" => {
            if args.len() != 2 {
                return Err("usage: radxa-linkr-debuggerctl ota status".to_string());
            }
            Ok(get_request("/api/v1/ota"))
        }
        "upload" => Err("usage: radxa-linkr-debuggerctl ota upload PATH".to_string()),
        "test" => {
            if args.len() != 2 {
                return Err("usage: radxa-linkr-debuggerctl ota test".to_string());
            }
            Ok(post_request("/api/v1/ota/test"))
        }
        "confirm" => {
            if args.len() != 2 {
                return Err("usage: radxa-linkr-debuggerctl ota confirm".to_string());
            }
            Ok(post_request("/api/v1/ota/confirm"))
        }
        other => Err(format!("unsupported ota action {:?}", other)),
    }
}

fn get_request(path: impl Into<String>) -> BoardRequest {
    BoardRequest {
        method: Method::GET,
        path: path.into(),
        query: vec![],
        body: None,
    }
}

fn get_request_with_query(path: impl Into<String>, query: Vec<(String, String)>) -> BoardRequest {
    BoardRequest {
        method: Method::GET,
        path: path.into(),
        query,
        body: None,
    }
}

fn put_request(path: impl Into<String>, body: Value) -> BoardRequest {
    BoardRequest {
        method: Method::PUT,
        path: path.into(),
        query: vec![],
        body: Some(body),
    }
}

fn post_request(path: impl Into<String>) -> BoardRequest {
    BoardRequest {
        method: Method::POST,
        path: path.into(),
        query: vec![],
        body: None,
    }
}

fn looks_like_json(output: &str) -> bool {
    let trimmed = output.trim();
    trimmed.starts_with('{') || trimmed.starts_with('[')
}

fn missing_command(
    json_output: bool,
    stdout: &mut dyn Write,
    stderr: &mut dyn Write,
) -> Result<u8> {
    if json_output {
        write_json_error(
            stdout,
            "radxa-linkr-debuggerctl",
            "missing_command",
            "missing command, for example: adc read",
        )?;
    } else {
        writeln!(stderr, "missing command, for example: adc read")?;
        write_usage(stderr)?;
    }
    Ok(2)
}

fn unsupported_raw(
    json_output: bool,
    stdout: &mut dyn Write,
    stderr: &mut dyn Write,
) -> Result<u8> {
    if json_output {
        write_json_error(
            stdout,
            "raw",
            "unsupported_raw",
            "raw shell commands are not available over HTTP",
        )?;
    } else {
        writeln!(stderr, "raw shell commands are not available over HTTP")?;
    }
    Ok(2)
}

fn error_from_envelope(err: EnvelopeError) -> JsonError {
    JsonError {
        code: "invalid_json".to_string(),
        message: err.to_string(),
    }
}

fn write_json_error(
    writer: &mut dyn Write,
    command: &str,
    code: &str,
    message: &str,
) -> Result<()> {
    let json = render_failure(command, code, message);
    writeln!(writer, "{json}")?;
    Ok(())
}

fn write_json<T: Serialize>(writer: &mut dyn Write, value: &T) -> Result<()> {
    serde_json::to_writer(&mut *writer, value)?;
    writeln!(writer)?;
    Ok(())
}

fn write_usage(writer: &mut dyn Write) -> Result<()> {
    writeln!(writer, "usage: radxa-linkr-debuggerctl [--url URL] [--timeout 2s] [--json] [-v] [--version] <command> [args...]\n")?;
    writeln!(writer, "examples:")?;
    writeln!(writer, "  radxa-linkr-debuggerctl status")?;
    writeln!(writer, "  radxa-linkr-debuggerctl --json status")?;
    writeln!(writer, "  radxa-linkr-debuggerctl doctor")?;
    writeln!(writer, "  radxa-linkr-debuggerctl power set 12v_out on")?;
    writeln!(writer, "  radxa-linkr-debuggerctl adc read")?;
    writeln!(writer, "  radxa-linkr-debuggerctl adc read -v 5v_out")?;
    writeln!(
        writer,
        "  radxa-linkr-debuggerctl adc record /tmp/adc.ndjson 1000 --rate-hz 250"
    )?;
    writeln!(writer, "  radxa-linkr-debuggerctl ota status")?;
    writeln!(
        writer,
        "  radxa-linkr-debuggerctl ota upload /tmp/firmware.bin"
    )?;
    writeln!(writer, "  radxa-linkr-debuggerctl watchdog status\n")?;
    writeln!(writer, "      --url <URL>")?;
    writeln!(writer, "      --addr <ADDR>")?;
    writeln!(writer, "      --port <PORT>")?;
    writeln!(writer, "      --timeout <TIMEOUT>")?;
    writeln!(writer, "      --raw")?;
    writeln!(writer, "      --json")?;
    writeln!(writer, "  -v, --verbose")?;
    writeln!(writer, "      --version")?;
    writeln!(writer, "  -h, --help")?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use clap::Parser;
    use serde_json::Value;
    use std::cell::RefCell;
    use std::io::Read;

    #[derive(Default)]
    struct FakeClient {
        requests: RefCell<Vec<BoardRequest>>,
        uploads: RefCell<Vec<FakeUpload>>,
        response: String,
        upload_response: String,
        err: Option<String>,
        upload_err: Option<String>,
        base_url: String,
    }

    #[derive(Debug, PartialEq, Eq)]
    struct FakeUpload {
        path: String,
        size: u64,
        sha256: String,
        body: Vec<u8>,
    }

    impl BoardTransport for FakeClient {
        fn send_text(&self, request: BoardRequest) -> Result<String> {
            self.requests.borrow_mut().push(request);
            if let Some(err) = &self.err {
                return Err(anyhow::anyhow!(err.clone()));
            }
            Ok(self.response.clone())
        }

        fn upload_binary(&self, mut request: BoardBinaryUpload) -> Result<String> {
            let mut body = Vec::new();
            request.file.read_to_end(&mut body)?;
            self.uploads.borrow_mut().push(FakeUpload {
                path: request.path,
                size: request.size,
                sha256: request.sha256,
                body,
            });
            if let Some(err) = &self.upload_err {
                return Err(anyhow::anyhow!(err.clone()));
            }
            Ok(self.upload_response.clone())
        }

        fn base_url(&self) -> &str {
            if self.base_url.is_empty() {
                DEFAULT_BASE_URL
            } else {
                &self.base_url
            }
        }
    }

    struct FakeTuiRunner {
        called: RefCell<bool>,
        seen_base_url: RefCell<String>,
        seen_timeout: RefCell<Duration>,
        code: u8,
    }

    impl FakeTuiRunner {
        fn new(code: u8) -> Self {
            Self {
                called: RefCell::new(false),
                seen_base_url: RefCell::new(String::new()),
                seen_timeout: RefCell::new(Duration::ZERO),
                code,
            }
        }
    }

    impl TuiRunner for FakeTuiRunner {
        fn run_tui(
            &self,
            base_url: &str,
            timeout: Duration,
            _stdout: &mut dyn Write,
            _stderr: &mut dyn Write,
        ) -> Result<u8> {
            *self.called.borrow_mut() = true;
            *self.seen_base_url.borrow_mut() = base_url.to_string();
            *self.seen_timeout.borrow_mut() = timeout;
            Ok(self.code)
        }
    }

    #[test]
    fn prefers_url_flag() {
        let cli = Cli::parse_from(["cmd", "--url", "http://example.com", "status"]);
        assert_eq!(base_url_from_cli(&cli), "http://example.com");
    }

    #[test]
    fn detects_json_output() {
        assert!(looks_like_json("{\"ok\":true}"));
    }

    #[test]
    fn run_without_args_starts_tui() {
        let cli = Cli::parse_from(["cmd"]);
        let client = FakeClient::default();
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 0);
        assert!(*tui.called.borrow());
        assert_eq!(&*tui.seen_base_url.borrow(), DEFAULT_BASE_URL);
        assert_eq!(*tui.seen_timeout.borrow(), Duration::from_secs(2));
    }

    #[test]
    fn run_status_uses_http_client() {
        let cli = Cli::parse_from(["cmd", "status"]);
        let client = FakeClient {
            response: r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"status","project":"radxa-linkr-debugger"}"#.to_string(),
            ..Default::default()
        };
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 0);
        let requests = client.requests.borrow();
        assert_eq!(requests.len(), 1);
        assert_eq!(requests[0].method, Method::GET);
        assert_eq!(requests[0].path, "/api/v1/status");
    }

    #[test]
    fn run_raw_command_is_rejected_over_http() {
        let cli = Cli::parse_from(["cmd", "--raw", "status"]);
        let client = FakeClient::default();
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 2);
        assert!(String::from_utf8(stderr)
            .unwrap()
            .contains("raw shell commands are not available"));
    }

    #[test]
    fn run_adc_read_default_text_uses_raw_current() {
        let cli = Cli::parse_from(["cmd", "adc", "read", "5v_out"]);
        let client = FakeClient {
            response: r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"adc","action":"read","readings":[{"name":"5v_out","signal":"S_C_5V","power_enabled":true,"sensor_channel":"current","unit":"A","sensor_value":{"val1":0,"val2":850000},"current_ua":850000}]}"#.to_string(),
            ..Default::default()
        };
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 0);
        let requests = client.requests.borrow();
        assert_eq!(requests.len(), 1);
        assert_eq!(requests[0].path, "/api/v1/adc/read");
        assert_eq!(
            requests[0].query[0],
            ("channel".to_string(), "5v_out".to_string())
        );
        assert_eq!(
            String::from_utf8(stdout).unwrap().trim(),
            "5v_out=0.850000A"
        );
    }

    #[test]
    fn run_adc_read_json_preserves_raw_current_fields() {
        let cli = Cli::parse_from(["cmd", "--json", "adc", "read", "5v_out"]);
        let client = FakeClient {
            response: r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"adc","action":"read","readings":[{"name":"5v_out","signal":"S_C_5V","power_enabled":true,"sensor_channel":"current","unit":"A","sensor_value":{"val1":0,"val2":850000},"current_ua":850000}]}"#.to_string(),
            ..Default::default()
        };
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 0);
        assert_eq!(
            String::from_utf8(stdout).unwrap().trim(),
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"adc","action":"read","readings":[{"name":"5v_out","signal":"S_C_5V","raw":null,"power_enabled":true,"sensor_channel":"current","unit":"A","sensor_value":{"val1":0,"val2":850000},"current_ua":850000}]}"#
        );
    }

    #[test]
    fn run_adc_read_verbose_flag_after_command_is_honored() {
        let cli = Cli::parse_from(["cmd", "adc", "read", "-v", "5v_out"]);
        let client = FakeClient {
            response: r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"adc","action":"read","readings":[{"name":"5v_out","signal":"S_C_5V","power_enabled":true,"sensor_channel":"current","unit":"A","sensor_value":{"val1":0,"val2":850000},"current_ua":850000}]}"#.to_string(),
            ..Default::default()
        };
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 0);
        assert_eq!(
            String::from_utf8(stdout).unwrap().trim(),
            "5v_out signal=S_C_5V power=on current=0.850000A current_ua=850000 raw=null"
        );
    }

    #[test]
    fn run_adc_read_power_disabled_still_reports_raw_current() {
        let cli = Cli::parse_from(["cmd", "adc", "read", "5v_out"]);
        let client = FakeClient {
            response: r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"adc","action":"read","readings":[{"name":"5v_out","signal":"S_C_5V","power_enabled":false,"raw":24,"mv":19,"sensor_channel":"current","unit":"A","sensor_value":{"val1":0,"val2":850000},"current_ua":850000}]}"#.to_string(),
            ..Default::default()
        };
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 0);
        assert_eq!(
            String::from_utf8(stdout).unwrap().trim(),
            "5v_out=0.850000A"
        );
    }

    #[test]
    fn parse_adc_record_rate_accepts_firmware_supported_range() {
        assert_eq!(parse_adc_record_rate_hz("1").unwrap(), 1);
        assert_eq!(parse_adc_record_rate_hz("1000").unwrap(), 1000);
        assert!(parse_adc_record_rate_hz("0").is_err());
        assert!(parse_adc_record_rate_hz("1001").is_err());
        assert!(parse_adc_record_rate_hz("fast").is_err());
    }

    #[test]
    fn parse_adc_record_args_accepts_optional_rate() {
        let parsed = parse_adc_record_args(&[
            "adc".to_string(),
            "record".to_string(),
            "/tmp/adc.ndjson".to_string(),
            "42".to_string(),
            "--rate-hz".to_string(),
            "250".to_string(),
        ])
        .unwrap();

        assert_eq!(parsed.output, "/tmp/adc.ndjson");
        assert_eq!(parsed.max_samples, Some(42));
        assert_eq!(parsed.rate_hz, 250);
    }

    #[test]
    fn parse_adc_record_args_keeps_default_rate() {
        let parsed = parse_adc_record_args(&[
            "adc".to_string(),
            "record".to_string(),
            "/tmp/adc.ndjson".to_string(),
        ])
        .unwrap();

        assert_eq!(parsed.max_samples, None);
        assert_eq!(parsed.rate_hz, crate::recorder::DEFAULT_ADC_RECORD_RATE_HZ);
    }

    #[test]
    fn run_power_set_maps_to_power_endpoint() {
        let cli = Cli::parse_from(["cmd", "power", "set", "12v_out", "off"]);
        let client = FakeClient {
            response: r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"power","action":"set","power_output":{"name":"12v_out","state":"off"}}"#.to_string(),
            ..Default::default()
        };
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 0);
        let requests = client.requests.borrow();
        assert_eq!(requests.len(), 1);
        assert_eq!(requests[0].method, Method::PUT);
        assert_eq!(requests[0].path, "/api/v1/power/12v_out");
        assert_eq!(requests[0].body.as_ref().unwrap()["state"], "off");
    }

    #[test]
    fn internal_power_output_is_rejected_without_board_access() {
        for args in [
            ["cmd", "power", "get", INTERNAL_POWER_OUTPUT, ""],
            ["cmd", "power", "set", INTERNAL_POWER_OUTPUT, "off"],
        ] {
            let args: Vec<&str> = args.into_iter().filter(|arg| !arg.is_empty()).collect();
            let cli = Cli::parse_from(args);
            let client = FakeClient::default();
            let tui = FakeTuiRunner::new(0);
            let mut stdout = Vec::new();
            let mut stderr = Vec::new();

            let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
            assert_eq!(code, 2);
            assert!(client.requests.borrow().is_empty());
            assert!(String::from_utf8(stderr)
                .unwrap()
                .contains("is internal and unavailable through the CLI"));
        }
    }

    #[test]
    fn status_output_hides_internal_power_output() {
        let cli = Cli::parse_from(["cmd", "--json", "status"]);
        let client = FakeClient {
            response: format!(
                r#"{{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"status","power_outputs":[{{"name":"12v_out","state":"off"}},{{"name":"{}","state":"on"}}]}}"#,
                INTERNAL_POWER_OUTPUT
            ),
            ..Default::default()
        };
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 0);
        let output: Value = serde_json::from_slice(&stdout).unwrap();
        let outputs = output["power_outputs"].as_array().unwrap();
        assert_eq!(outputs.len(), 1);
        assert_eq!(outputs[0]["name"], "12v_out");
    }

    #[test]
    fn nested_doctor_status_hides_internal_power_output() {
        let mut output = serde_json::json!({
            "status": {
                "power_outputs": [
                    {"name": INTERNAL_POWER_OUTPUT},
                    {"name": "5v_out"}
                ]
            }
        });

        assert!(remove_internal_power_output(&mut output));
        assert_eq!(
            output["status"]["power_outputs"].as_array().unwrap().len(),
            1
        );
        assert_eq!(output["status"]["power_outputs"][0]["name"], "5v_out");
    }

    #[test]
    fn run_switch_get_vin_maps_to_switch_endpoint() {
        let cli = Cli::parse_from(["cmd", "switch", "get", "vin"]);
        let client = FakeClient {
            response: r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"switch","action":"get","name":"vin","route":"3.3v"}"#.to_string(),
            ..Default::default()
        };
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 0);
        let requests = client.requests.borrow();
        assert_eq!(requests.len(), 1);
        assert_eq!(requests[0].method, Method::GET);
        assert_eq!(requests[0].path, "/api/v1/switch/vin");
    }

    #[test]
    fn run_switch_vin_requires_confirmation() {
        let cli = Cli::parse_from(["cmd", "switch", "route", "vin", "1.8v"]);
        let client = FakeClient::default();
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 2);
        assert!(client.requests.borrow().is_empty());
        assert!(String::from_utf8(stderr)
            .unwrap()
            .contains("requires --confirm"));
    }

    #[test]
    fn run_switch_vin_sends_only_confirmed_switch_request() {
        let cli = Cli::parse_from(["cmd", "switch", "route", "vin", "1.8v", "--confirm"]);
        let client = FakeClient {
            response: r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"switch","action":"route","name":"vin","route":"1.8v"}"#.to_string(),
            ..Default::default()
        };
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 0);
        let requests = client.requests.borrow();
        assert_eq!(requests.len(), 1);
        assert_eq!(requests[0].method, Method::PUT);
        assert_eq!(requests[0].path, "/api/v1/switch/vin");
        assert_eq!(requests[0].body.as_ref().unwrap()["route"], "1.8v");
    }

    #[test]
    fn run_switch_vin_rejects_invalid_route_before_board_access() {
        let cli = Cli::parse_from(["cmd", "switch", "route", "vin", "1v8", "--confirm"]);
        let client = FakeClient::default();
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 2);
        assert!(client.requests.borrow().is_empty());
    }

    #[test]
    fn run_switch_usb_rejects_invalid_request_before_power_side_effect() {
        for args in [
            vec!["cmd", "switch", "route", "usb", "invalid", "--confirm"],
            vec![
                "cmd",
                "switch",
                "route",
                "usb",
                "target",
                "extra",
                "--confirm",
            ],
        ] {
            let cli = Cli::parse_from(args);
            let client = FakeClient::default();
            let tui = FakeTuiRunner::new(0);
            let mut stdout = Vec::new();
            let mut stderr = Vec::new();

            let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
            assert_eq!(code, 2);
            assert!(client.requests.borrow().is_empty());
        }
    }

    #[test]
    fn run_switch_usb_sends_only_confirmed_switch_request() {
        let cli = Cli::parse_from(["cmd", "switch", "route", "usb", "pc", "--confirm"]);
        let client = FakeClient {
            response: r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"switch","action":"route","name":"usb","route":"pc"}"#.to_string(),
            ..Default::default()
        };
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 0);
        let requests = client.requests.borrow();
        assert_eq!(requests.len(), 1);
        assert_eq!(requests[0].method, Method::PUT);
        assert_eq!(requests[0].path, "/api/v1/switch/usb");
        assert_eq!(requests[0].body.as_ref().unwrap()["route"], "pc");
    }

    #[test]
    fn run_sd_gpio_and_bootloader_mappings() {
        let tests = [
            (
                vec!["cmd", "gpio", "input", "GP13"],
                Method::PUT,
                "/api/v1/gpio/GP13",
            ),
            (
                vec!["cmd", "gpio", "set", "GP13", "1"],
                Method::PUT,
                "/api/v1/gpio/GP13",
            ),
            (
                vec!["cmd", "gpio", "set", "CON_MAS", "1"],
                Method::PUT,
                "/api/v1/gpio/CON_MAS",
            ),
            (
                vec!["cmd", "watchdog", "status"],
                Method::GET,
                "/api/v1/watchdog",
            ),
            (
                vec!["cmd", "bootloader"],
                Method::POST,
                "/api/v1/bootloader",
            ),
        ];

        for (args, method, path) in tests {
            let cli = Cli::parse_from(args.clone());
            let client = FakeClient {
                response: r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"ok"}"#
                    .to_string(),
                ..Default::default()
            };
            let tui = FakeTuiRunner::new(0);
            let mut stdout = Vec::new();
            let mut stderr = Vec::new();

            let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
            assert_eq!(code, 0, "args={args:?}");
            let requests = client.requests.borrow();
            assert_eq!(requests.len(), 1, "args={args:?}");
            assert_eq!(requests[0].method, method, "args={args:?}");
            assert_eq!(requests[0].path, path, "args={args:?}");
        }
    }

    #[test]
    fn run_ota_standard_commands_map_to_frozen_endpoints() {
        let tests = [
            (vec!["cmd", "ota", "status"], Method::GET, "/api/v1/ota"),
            (vec!["cmd", "ota", "test"], Method::POST, "/api/v1/ota/test"),
            (
                vec!["cmd", "ota", "confirm"],
                Method::POST,
                "/api/v1/ota/confirm",
            ),
        ];

        for (args, method, path) in tests {
            let cli = Cli::parse_from(args.clone());
            let client = FakeClient {
                response: r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"ota"}"#
                    .to_string(),
                ..Default::default()
            };
            let tui = FakeTuiRunner::new(0);
            let mut stdout = Vec::new();
            let mut stderr = Vec::new();

            let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
            assert_eq!(code, 0, "args={args:?}");
            let requests = client.requests.borrow();
            assert_eq!(requests.len(), 1, "args={args:?}");
            assert_eq!(requests[0].method, method, "args={args:?}");
            assert_eq!(requests[0].path, path, "args={args:?}");
            assert!(client.uploads.borrow().is_empty(), "args={args:?}");
        }
    }

    #[test]
    fn run_ota_upload_streams_file_with_size_and_sha256() {
        let path = temp_file_path("ota-upload");
        std::fs::write(&path, b"abc123").unwrap();
        let cli = Cli::parse_from(["cmd", "ota", "upload", path.to_str().unwrap()]);
        let client = FakeClient {
            upload_response:
                r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"ota","action":"upload"}"#
                    .to_string(),
            ..Default::default()
        };
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        let _ = std::fs::remove_file(&path);

        assert_eq!(code, 0);
        assert!(client.requests.borrow().is_empty());
        let uploads = client.uploads.borrow();
        assert_eq!(uploads.len(), 1);
        assert_eq!(uploads[0].path, "/api/v1/ota/upload");
        assert_eq!(uploads[0].size, 6);
        assert_eq!(uploads[0].body, b"abc123");
        assert_eq!(
            uploads[0].sha256,
            "6ca13d52ca70c883e0f0bb101e425a89e8624de51db2d2392593af6a84118090"
        );
        assert_eq!(
            String::from_utf8(stdout).unwrap().trim(),
            "ota upload ok: 6 bytes sha256=6ca13d52ca70c883e0f0bb101e425a89e8624de51db2d2392593af6a84118090"
        );
    }

    #[test]
    fn run_json_ota_upload_returns_board_response() {
        let path = temp_file_path("ota-upload-json");
        std::fs::write(&path, b"abc123").unwrap();
        let cli = Cli::parse_from(["cmd", "--json", "ota", "upload", path.to_str().unwrap()]);
        let response =
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"ota","action":"upload"}"#;
        let client = FakeClient {
            upload_response: response.to_string(),
            ..Default::default()
        };
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        let _ = std::fs::remove_file(&path);

        assert_eq!(code, 0);
        assert_eq!(String::from_utf8(stdout).unwrap().trim(), response);
    }

    #[test]
    fn run_json_ota_upload_preserves_board_error_envelope() {
        let path = temp_file_path("ota-json-board-error");
        std::fs::write(&path, b"abc123").unwrap();
        let cli = Cli::parse_from(["cmd", "--json", "ota", "upload", path.to_str().unwrap()]);
        let response = r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"ota","error":{"code":"image_too_large","message":"OTA upload failed validation"}}"#;
        let client = FakeClient {
            upload_response: response.to_string(),
            ..Default::default()
        };
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        let _ = std::fs::remove_file(&path);

        assert_eq!(code, 1);
        assert_eq!(String::from_utf8(stdout).unwrap().trim(), response);
        assert!(stderr.is_empty());
    }

    #[test]
    fn run_ota_upload_rejects_missing_and_empty_files_before_board_access() {
        let missing = temp_file_path("ota-missing");
        let empty = temp_file_path("ota-empty");
        std::fs::write(&empty, b"").unwrap();

        for path in [&missing, &empty] {
            let cli = Cli::parse_from(["cmd", "ota", "upload", path.to_str().unwrap()]);
            let client = FakeClient::default();
            let tui = FakeTuiRunner::new(0);
            let mut stdout = Vec::new();
            let mut stderr = Vec::new();

            let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
            assert_eq!(code, 2);
            assert!(client.requests.borrow().is_empty());
            assert!(client.uploads.borrow().is_empty());
            assert!(!String::from_utf8(stderr).unwrap().trim().is_empty());
        }
        let _ = std::fs::remove_file(&empty);
    }

    #[test]
    fn run_json_ota_upload_reports_structured_local_and_transport_errors() {
        let missing = temp_file_path("ota-json-missing");
        let cli = Cli::parse_from(["cmd", "--json", "ota", "upload", missing.to_str().unwrap()]);
        let client = FakeClient::default();
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 2);
        let got: Value = serde_json::from_slice(&stdout).unwrap();
        assert_eq!(got["command"], "ota");
        assert_eq!(got["error"]["code"], "invalid_file");

        let path = temp_file_path("ota-json-transport");
        std::fs::write(&path, b"abc123").unwrap();
        let cli = Cli::parse_from(["cmd", "--json", "ota", "upload", path.to_str().unwrap()]);
        let client = FakeClient {
            upload_err: Some("upload failed".to_string()),
            ..Default::default()
        };
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        let _ = std::fs::remove_file(&path);
        assert_eq!(code, 1);
        let got: Value = serde_json::from_slice(&stdout).unwrap();
        assert_eq!(got["command"], "ota");
        assert_eq!(got["error"]["code"], "transport_error");
        assert!(stderr.is_empty());
    }

    #[test]
    fn run_watchdog_feed_is_rejected_locally() {
        let cli = Cli::parse_from(["cmd", "watchdog", "feed"]);
        let client = FakeClient {
            response: r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"ok"}"#
                .to_string(),
            ..Default::default()
        };
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 2);
        assert_eq!(client.requests.borrow().len(), 0);
        assert!(String::from_utf8(stderr)
            .unwrap()
            .contains("unsupported watchdog action"));
    }

    #[test]
    fn run_json_watchdog_feed_is_rejected_locally() {
        let cli = Cli::parse_from(["cmd", "--json", "watchdog", "feed"]);
        let client = FakeClient {
            response: r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"ok"}"#
                .to_string(),
            ..Default::default()
        };
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 2);
        assert_eq!(client.requests.borrow().len(), 0);
        let got: Value = serde_json::from_slice(&stdout).unwrap();
        assert_eq!(got["ok"], false);
        assert_eq!(got["command"], "watchdog");
        assert_eq!(got["error"]["code"], "usage");
    }

    #[test]
    fn doctor_with_extra_args_returns_usage() {
        let cli = Cli::parse_from(["cmd", "doctor", "extra"]);
        let client = FakeClient::default();
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 2);
        assert!(String::from_utf8(stderr)
            .unwrap()
            .contains("usage: radxa-linkr-debuggerctl doctor"));
    }

    #[test]
    fn run_json_command_returns_failure_on_board_error() {
        let cli = Cli::parse_from(["cmd", "--json", "power", "get", "missing"]);
        let client = FakeClient {
            response: r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"power","error":{"code":"unknown_power_output","message":"unknown power output"}}"#.to_string(),
            ..Default::default()
        };
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 1);
        assert_eq!(String::from_utf8(stdout).unwrap().trim(), client.response);
    }

    #[test]
    fn run_json_rejects_old_text_firmware_output() {
        let cli = Cli::parse_from(["cmd", "--json", "status"]);
        let client = FakeClient {
            response: "project=radxa-linkr-debugger".to_string(),
            ..Default::default()
        };
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 1);
        let got: Value = serde_json::from_slice(&stdout).unwrap();
        assert_eq!(got["ok"], false);
        assert_eq!(got["command"], "status");
        assert_eq!(got["error"]["code"], "invalid_json");
    }

    #[test]
    fn run_reports_transport_error() {
        let cli = Cli::parse_from(["cmd", "status"]);
        let client = FakeClient {
            err: Some("dial failed".to_string()),
            ..Default::default()
        };
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 1);
        assert!(String::from_utf8(stderr).unwrap().contains("dial failed"));
    }

    #[test]
    fn run_help_returns_success() {
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = run_with_io(["cmd", "--help"], &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 0);
        let _ = String::from_utf8(stdout).unwrap();
        let _ = String::from_utf8(stderr).unwrap();
    }

    #[test]
    fn run_version_returns_success_without_board_access() {
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = run_with_io(["cmd", "--version"], &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 0);
        assert_eq!(
            String::from_utf8(stdout).unwrap().trim(),
            format!("radxa-linkr-debuggerctl {}", version())
        );
    }

    #[test]
    fn version_matches_the_cargo_package_version() {
        assert_eq!(version(), env!("CARGO_PKG_VERSION"));
        assert_ne!(version(), "dev");
    }

    #[test]
    fn run_json_version_returns_success_without_board_access() {
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = run_with_io(["cmd", "--json", "--version"], &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 0);
        let got: Value = serde_json::from_slice(&stdout).unwrap();
        assert_eq!(got["schema"], JSON_SCHEMA);
        assert_eq!(got["command"], "version");
        assert_eq!(got["version"], version());
    }

    #[test]
    fn doctor_json_reports_http_status() {
        let cli = Cli::parse_from(["cmd", "--json", "--url", "http://172.29.203.1", "doctor"]);
        let client = FakeClient {
            response: r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"status","project":"radxa-linkr-debugger"}"#.to_string(),
            base_url: "http://172.29.203.1".to_string(),
            ..Default::default()
        };
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 0);
        let got: Value = serde_json::from_slice(&stdout).unwrap();
        assert_eq!(got["ok"], true);
        assert_eq!(got["base_url"], "http://172.29.203.1");
        assert_eq!(got["probe_ok"], true);
    }

    #[test]
    fn doctor_json_reports_http_failure() {
        let cli = Cli::parse_from(["cmd", "--json", "doctor"]);
        let client = FakeClient {
            err: Some("connection refused".to_string()),
            ..Default::default()
        };
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 1);
        let got: Value = serde_json::from_slice(&stdout).unwrap();
        assert_eq!(got["ok"], false);
        assert_eq!(got["error"]["code"], "status_failed");
    }

    #[test]
    fn status_json_includes_board_monitoring_shape() {
        let cli = Cli::parse_from(["cmd", "--json", "status"]);
        let client = FakeClient {
            response: r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"status","project":"radxa-linkr-debugger","board_monitoring":{"temperature":{"available":false,"reason":"no_zephyr_temperature_device"},"heap":{"available":true,"reason":"","source":"system_heap","free_bytes":6144,"allocated_bytes":2048,"max_allocated_bytes":3072,"total_bytes":8192},"memory":{"available":true,"reason":"","source":"zephyr","coverage":"heap_and_stacks","pressure_pct_x100":4250,"limiting_component":"thread_stack","limiting_name":"main","system_heap_pressure_pct_x100":3000,"current_pressure":{"available":true,"reason":"","coverage":"heap_and_stacks","pressure_pct_x100":3000,"limiting_component":"system_heap","limiting_name":"","tie_count":1},"peak_pressure":{"available":true,"reason":"","coverage":"heap_and_stacks","pressure_pct_x100":4250,"limiting_component":"thread_stack","limiting_name":"main","tie_count":1,"since":"boot"},"physical":{"total_bytes":270336,"image_reserved_bytes":98304,"reserved_pct_x100":3721},"stacks":{"thread_count":7,"measured_count":6,"error_count":1,"total_bytes":12288,"used_high_water_bytes":4096,"max_pressure_pct_x100":4250,"max_pressure_thread":"main"}},"runtime":{"available":true,"reason":"","uptime_ms":12345,"uptime_seconds":12},"cpu":{"available":false,"reason":"thread_runtime_stats_disabled"}}}"#.to_string(),
            ..Default::default()
        };
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 0);
        let got: Value = serde_json::from_slice(&stdout).unwrap();
        assert_eq!(got["board_monitoring"]["temperature"]["available"], false);
        assert_eq!(
            got["board_monitoring"]["temperature"]["reason"],
            "no_zephyr_temperature_device"
        );
        assert_eq!(got["board_monitoring"]["heap"]["source"], "system_heap");
        assert_eq!(got["board_monitoring"]["memory"]["pressure_pct_x100"], 4250);
        assert_eq!(
            got["board_monitoring"]["memory"]["current_pressure"]["pressure_pct_x100"],
            3000
        );
        assert_eq!(
            got["board_monitoring"]["memory"]["peak_pressure"]["limiting_component"],
            "thread_stack"
        );
        assert_eq!(
            got["board_monitoring"]["memory"]["peak_pressure"]["since"],
            "boot"
        );
        assert_eq!(got["board_monitoring"]["memory"]["source"], "zephyr");
        assert_eq!(
            got["board_monitoring"]["memory"]["limiting_component"],
            "thread_stack"
        );
        assert_eq!(
            got["board_monitoring"]["memory"]["physical"]["total_bytes"],
            270336
        );
        assert_eq!(
            got["board_monitoring"]["memory"]["stacks"]["max_pressure_thread"],
            "main"
        );
        assert_eq!(got["board_monitoring"]["runtime"]["uptime_seconds"], 12);
    }

    #[test]
    fn status_json_preserves_old_board_monitoring_shape_without_memory() {
        let cli = Cli::parse_from(["cmd", "--json", "status"]);
        let client = FakeClient {
            response: r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"status","project":"radxa-linkr-debugger","board_monitoring":{"heap":{"available":true,"reason":"","source":"system_heap","allocated_bytes":2048,"total_bytes":8192}}}"#.to_string(),
            ..Default::default()
        };
        let tui = FakeTuiRunner::new(0);
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        let code = execute_with_io(cli, &client, &tui, &mut stdout, &mut stderr).unwrap();
        assert_eq!(code, 0);
        let got: Value = serde_json::from_slice(&stdout).unwrap();
        assert_eq!(got["board_monitoring"]["heap"]["allocated_bytes"], 2048);
        assert!(got["board_monitoring"].get("memory").is_none());
    }

    fn temp_file_path(name: &str) -> std::path::PathBuf {
        let mut path = std::env::temp_dir();
        path.push(format!(
            "radxa-linkr-debugger-app-test-{name}-{}",
            std::process::id(),
        ));
        path
    }
}
