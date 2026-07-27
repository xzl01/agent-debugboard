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
        let usage = "usage: radxa-linkr-debuggerctl test run SCRIPT [--output PATH] [--serial PATH] [--serial-uart0 PATH] [--serial-uart1 PATH]";
        if json_output {
            write_error(stdout, "usage", usage)?;
        } else {
            writeln!(stderr, "{usage}")?;
        }
        return Ok(2);
    }

    let script_path = &args[2];
    let mut output_path: Option<String> = None;
    let mut serial_uart0_path: Option<String> = None;
    let mut serial_uart1_path: Option<String> = None;

    // Parse optional flags
    let mut i = 3;
    while i < args.len() {
        match args[i].as_str() {
            "--output" | "-o" => {
                i += 1;
                let Some(path) = args.get(i) else {
                    if json_output {
                        write_error(stdout, "usage", "missing value for --output")?;
                    } else {
                        writeln!(stderr, "missing value for --output")?;
                    }
                    return Ok(2);
                };
                output_path = Some(path.clone());
            }
            "--serial" | "-s" | "--serial-uart0" => {
                i += 1;
                let Some(path) = args.get(i) else {
                    if json_output {
                        write_error(stdout, "usage", "missing value for UART0 serial option")?;
                    } else {
                        writeln!(stderr, "missing value for UART0 serial option")?;
                    }
                    return Ok(2);
                };
                serial_uart0_path = Some(path.clone());
            }
            "--serial-uart1" => {
                i += 1;
                let Some(path) = args.get(i) else {
                    if json_output {
                        write_error(stdout, "usage", "missing value for --serial-uart1")?;
                    } else {
                        writeln!(stderr, "missing value for --serial-uart1")?;
                    }
                    return Ok(2);
                };
                serial_uart1_path = Some(path.clone());
            }
            other => {
                let message = format!("unknown test option: {other}");
                if json_output {
                    write_error(stdout, "usage", &message)?;
                } else {
                    writeln!(stderr, "{message}")?;
                }
                return Ok(2);
            }
        }
        i += 1;
    }

    // Parse script
    let file = match File::open(script_path) {
        Ok(file) => file,
        Err(error) => {
            let message = format!("cannot open script {script_path:?}: {error}");
            if json_output {
                write_error(stdout, "script_open_failed", &message)?;
            } else {
                writeln!(stderr, "{message}")?;
            }
            return Ok(2);
        }
    };
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
        output_path,
        serial_uart0_path,
        serial_uart1_path,
        json_output,
        verbose,
    };

    // Run
    match crate::test_runner::run_script(&mut script, &client, &opts, stdout, stderr) {
        Ok(exit_code) => Ok(exit_code),
        Err(error) => {
            if json_output {
                write_error(stdout, "run_error", &error.to_string())?;
            } else {
                writeln!(stderr, "failed to run test: {error}")?;
            }
            Ok(1)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn run_json(args: &[&str]) -> (u8, serde_json::Value, String) {
        let args = args
            .iter()
            .map(|value| value.to_string())
            .collect::<Vec<_>>();
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();
        let exit = run_test(
            &args,
            "http://127.0.0.1:9",
            Duration::from_millis(10),
            true,
            false,
            &mut stdout,
            &mut stderr,
        )
        .unwrap();
        (
            exit,
            serde_json::from_slice(&stdout).unwrap(),
            String::from_utf8(stderr).unwrap(),
        )
    }

    #[test]
    fn json_option_errors_stay_on_stdout_as_envelopes() {
        let (exit, value, stderr) = run_json(&["test", "run", "unused", "--serial-uart1"]);
        assert_eq!(exit, 2);
        assert_eq!(value["schema"], crate::json_contract::JSON_SCHEMA);
        assert_eq!(value["ok"], false);
        assert_eq!(value["command"], "test");
        assert!(stderr.is_empty());
    }

    #[test]
    fn json_script_open_errors_stay_on_stdout_as_envelopes() {
        let (exit, value, stderr) =
            run_json(&["test", "run", "/path/that/does/not/exist/linkr-test.ndjson"]);
        assert_eq!(exit, 2);
        assert_eq!(value["error"]["code"], "script_open_failed");
        assert!(stderr.is_empty());
    }
}
