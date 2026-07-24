// SPDX-License-Identifier: LGPL-3.0-or-later
//
// `test` subcommand entry point.

use anyhow::Result;
use std::fs::File;
use std::io::Write;
use std::time::Duration;

use crate::client::BoardClient;
use crate::json_contract::render_failure;
use crate::test_runner::RunOptions;
use crate::test_script::parse_script;

fn write_error(stdout: &mut dyn Write, code: &str, message: &str) -> Result<()> {
    let json = render_failure("test", code, message);
    writeln!(stdout, "{json}")?;
    Ok(())
}

pub fn run_test(
    args: &[String],
    base_url: &str,
    timeout: Duration,
    json_output: bool,
    verbose: bool,
    stdout: &mut dyn Write,
    stderr: &mut dyn Write,
) -> Result<u8> {
    if args.len() < 3 || args[1] != "run" {
        let usage = "usage: radxa-linkr-debuggerctl test run SCRIPT [--output PATH] [--serial PATH]";
        if json_output {
            write_error(stdout, "usage", usage)?;
        } else {
            writeln!(stderr, "{usage}")?;
        }
        return Ok(2);
    }

    let script_path = &args[2];
    let mut output_path: Option<String> = None;
    let mut serial_path: Option<String> = None;

    // Parse optional flags
    let mut i = 3;
    while i < args.len() {
        match args[i].as_str() {
            "--output" | "-o" => {
                i += 1;
                output_path = args.get(i).cloned();
            }
            "--serial" | "-s" => {
                i += 1;
                serial_path = args.get(i).cloned();
            }
            other => {
                writeln!(stderr, "unknown test option: {other}")?;
                return Ok(2);
            }
        }
        i += 1;
    }

    // Parse script
    let file = File::open(script_path)
        .map_err(|e| anyhow::anyhow!("cannot open script {script_path:?}: {e}"))?;
    let mut script = match parse_script(file) {
        Ok(script) => script,
        Err(e) => {
            if json_output {
                write_error(stdout, "parse_error", &e.to_string())?;
            } else {
                writeln!(stderr, "failed to parse script: {e}")?;
            }
            return Ok(2);
        }
    };

    if script.steps.is_empty() {
        if json_output {
            write_error(stdout, "empty_script", "script has no steps")?;
        } else {
            writeln!(stderr, "script has no steps")?;
        }
        return Ok(2);
    }

    // Create client
    let client = BoardClient::new(base_url, timeout)?;

    let opts = RunOptions {
        base_url: base_url.to_string(),
        timeout,
        serial_path,
        json_output,
        verbose,
    };

    // Run
    let exit_code = crate::test_runner::run_script(&mut script, &client, &opts, stdout, stderr)?;

    Ok(exit_code)
}
