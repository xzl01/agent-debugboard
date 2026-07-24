// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Test runner: executes linkr-test.v1 scripts against the board.

use anyhow::{bail, Context, Result};
use std::io::Write;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use crate::client::{BoardClient, BoardRequest, BoardTransport};
use crate::test_assertions::{self, AssertionContext, AssertionResult, StepAssertion};
use crate::test_report::{self, ReportBuilder, StepResult, StepStatus};
use crate::test_script::{apply_defaults, StepType, TestScript};

pub struct RunOptions {
    pub base_url: String,
    pub timeout: Duration,
    pub serial_path: Option<String>,
    pub json_output: bool,
    pub verbose: bool,
}

pub fn run_script(
    script: &mut TestScript,
    client: &BoardClient,
    opts: &RunOptions,
    stdout: &mut dyn Write,
    stderr: &mut dyn Write,
) -> Result<u8> {
    let total_steps = script.steps.len();
    let mut report = ReportBuilder::new(script.header.name.clone());
    let abort = Arc::new(AtomicBool::new(false));

    // Set up Ctrl+C handler
    let abort_clone = abort.clone();
    let _ = ctrlc::set_handler(move || {
        abort_clone.store(true, Ordering::SeqCst);
    });

    // Open serial port if needed
    let needs_serial = script.steps.iter().any(|s| {
        matches!(
            s.step_type,
            StepType::SerialWait | StepType::SerialSend | StepType::SerialExpect
        )
    });
    let mut serial: Option<crate::test_serial::SerialPort> = if needs_serial {
        match &opts.serial_path {
            Some(path) => Some(crate::test_serial::SerialPort::open(path, 115200)?),
            None => match crate::test_serial::SerialPort::auto_detect() {
                Ok(port) => Some(port),
                Err(e) => {
                    if opts.verbose {
                        writeln!(stderr, "warning: no serial port: {e}")?;
                    }
                    None
                }
            },
        }
    } else {
        None
    };

    for step in &mut script.steps {
        if abort.load(Ordering::SeqCst) {
            report.set_aborted();
            break;
        }

        apply_defaults(step);

        let start = Instant::now();
        let started_at_ms = now_ms();

        if opts.verbose && !opts.json_output {
            writeln!(stderr, "  [{}] {} ...", step.id, step.step_type_name())?;
        }

        let result = execute_step(
            step,
            client,
            serial.as_mut(),
            &abort,
            opts,
            stderr,
        );

        let finished_at_ms = now_ms();
        let duration_ms = start.elapsed().as_millis() as u64;

        let step_result = match result {
            Ok(mut ctx) => {
                // Evaluate assertions
                let assertion = step
                    .assert
                    .as_ref()
                    .and_then(|v| serde_json::from_value::<StepAssertion>(v.clone()).ok());

                let (status, assertion_result) = if let Some(ref assert) = assertion {
                    let result = test_assertions::evaluate(assert, &ctx);
                    if result.passed {
                        (StepStatus::Pass, Some(result))
                    } else {
                        (StepStatus::Fail, Some(result))
                    }
                } else {
                    (StepStatus::Pass, None)
                };

                StepResult {
                    step_id: step.id.clone(),
                    step_type: step.step_type,
                    status,
                    started_at_ms,
                    finished_at_ms,
                    duration_ms,
                    error: None,
                    assertion_result,
                    adc_value_ua: ctx.adc_value_ua.take(),
                    serial_output: ctx.serial_output.take(),
                }
            }
            Err(e) => {
                let msg = e.to_string();
                if msg.contains("skip") {
                    StepResult {
                        step_id: step.id.clone(),
                        step_type: step.step_type,
                        status: StepStatus::Skip,
                        started_at_ms,
                        finished_at_ms,
                        duration_ms,
                        error: Some(msg),
                        assertion_result: None,
                        adc_value_ua: None,
                        serial_output: None,
                    }
                } else {
                    StepResult {
                        step_id: step.id.clone(),
                        step_type: step.step_type,
                        status: StepStatus::Error,
                        started_at_ms,
                        finished_at_ms,
                        duration_ms,
                        error: Some(msg),
                        assertion_result: None,
                        adc_value_ua: None,
                        serial_output: None,
                    }
                }
            }
        };

        // Print step result
        if !opts.json_output {
            let icon = match step_result.status {
                StepStatus::Pass => "✓",
                StepStatus::Fail => "✗",
                StepStatus::Skip => "⊘",
                StepStatus::Error => "!",
                StepStatus::Aborted => "⊘",
            };
            writeln!(
                stderr,
                "  {} {} [{}ms]",
                icon, step_result.step_id, step_result.duration_ms
            )?;
            if let Some(ref err) = step_result.error {
                if step_result.status != StepStatus::Skip || opts.verbose {
                    writeln!(stderr, "    {err}")?;
                }
            }
            if let Some(ref ar) = step_result.assertion_result {
                if !ar.passed {
                    writeln!(stderr, "    assertion: {}", ar.detail)?;
                }
            }
        }

        let should_stop = matches!(
            step_result.status,
            StepStatus::Fail | StepStatus::Error
        ) && step.continue_on_error != Some(true);

        report.add_result(step_result);

        if should_stop {
            break;
        }
    }

    let summary = report.summary(total_steps);
    let success = report.is_successful(total_steps);

    // Print summary
    if opts.json_output {
        let report_json = serde_json::to_string(&summary)?;
        writeln!(stdout, "{report_json}")?;
    } else {
        test_report::print_summary(stderr, &summary)?;
    }

    Ok(if success { 0 } else { 1 })
}

fn execute_step(
    step: &crate::test_script::TestStep,
    client: &dyn BoardTransport,
    serial: Option<&mut crate::test_serial::SerialPort>,
    abort: &Arc<AtomicBool>,
    _opts: &RunOptions,
    _stderr: &mut dyn Write,
) -> Result<AssertionContext> {
    let mut ctx = AssertionContext::default();

    match step.step_type {
        StepType::PowerOn => {
            let rail = step.params["rail"].as_str().unwrap_or("5v_out");
            client.send_text(BoardRequest {
                method: reqwest::Method::PUT,
                path: format!("/api/v1/power/{rail}"),
                query: vec![],
                body: Some(serde_json::json!({"state": "on"})),
            })?;
            sleep_interruptible(Duration::from_millis(500), abort)?;
        }

        StepType::PowerOff => {
            let rail = step.params["rail"].as_str().unwrap_or("5v_out");
            client.send_text(BoardRequest {
                method: reqwest::Method::PUT,
                path: format!("/api/v1/power/{rail}"),
                query: vec![],
                body: Some(serde_json::json!({"state": "off"})),
            })?;
        }

        StepType::Delay => {
            let ms = step.params["ms"].as_u64().unwrap_or(1000);
            sleep_interruptible(Duration::from_millis(ms), abort)?;
        }

        StepType::SerialWait => {
            let serial = serial.context("serial port not available")?;
            let pattern = step.params["pattern"].as_str().unwrap_or("login:");
            let timeout_ms = step.params["timeout_ms"].as_u64().unwrap_or(60000);

            let result = serial.wait_for_pattern(pattern, Duration::from_millis(timeout_ms))?;
            ctx.serial_output = Some(result.output.clone());

            if result.timed_out {
                bail!("timeout waiting for pattern: {pattern}");
            }
        }

        StepType::SerialSend => {
            let serial = serial.context("serial port not available")?;
            let text = step.params["text"].as_str().unwrap_or("root\n");
            serial.write(text)?;
        }

        StepType::SerialExpect => {
            let serial = serial.context("serial port not available")?;
            let command = step.params["command"].as_str().unwrap_or("uname -a");
            let pattern = step.params["pattern"].as_str().unwrap_or("Linux");
            let timeout_ms = step.params["timeout_ms"].as_u64().unwrap_or(10000);

            let result = serial.send_and_expect(command, pattern, Duration::from_millis(timeout_ms))?;
            ctx.serial_output = Some(result.output.clone());
            ctx.exit_code = Some(result.exit_code);

            if !result.completed {
                bail!("timeout waiting for command completion");
            }
            if !result.pattern_matched {
                bail!("command output did not match: {pattern}");
            }
        }

        StepType::AdcRead => {
            let channel = step.params["channel"].as_str().unwrap_or("5v_out");
            let output = client.send_text(BoardRequest {
                method: reqwest::Method::GET,
                path: "/api/v1/adc/read".to_string(),
                query: vec![("channel".to_string(), channel.to_string())],
                body: None,
            })?;

            let value: serde_json::Value = serde_json::from_str(&output)?;
            if let Some(readings) = value["readings"].as_array() {
                if let Some(first) = readings.first() {
                    if let Some(ua) = first["current_ua"].as_f64() {
                        ctx.adc_value_ua = Some(ua);
                    }
                }
            }
        }

        StepType::GpioSet => {
            let pin = step.params["pin"].as_str().unwrap_or("GP13");
            let value = step.params["value"].as_i64().unwrap_or(1);
            client.send_text(BoardRequest {
                method: reqwest::Method::PUT,
                path: format!("/api/v1/gpio/{pin}"),
                query: vec![],
                body: Some(serde_json::json!({"direction": "output", "value": value})),
            })?;
        }

        StepType::GpioAssert => {
            let pin = step.params["pin"].as_str().unwrap_or("GP13");
            let _expected_dir = step.params["direction"].as_str().unwrap_or("output");
            let _expected_val = step.params["value"].as_i64().unwrap_or(1);

            // Read GPIO state from snapshot
            let output = client.send_text(BoardRequest {
                method: reqwest::Method::GET,
                path: "/api/v1/gpio".to_string(),
                query: vec![],
                body: None,
            })?;

            let value: serde_json::Value = serde_json::from_str(&output)?;
            if let Some(gpios) = value["gpios"].as_array() {
                let found = gpios.iter().find(|g| {
                    g["name"].as_str() == Some(pin)
                        || g["note"].as_str() == Some(pin)
                        || g["pin"].to_string() == pin
                });
                if let Some(gpio) = found {
                    ctx.pin_direction = gpio["direction"].as_str().map(String::from);
                    ctx.pin_value = gpio["value"].as_i64().map(|v| v as i32);
                } else {
                    bail!("GPIO {pin} not found");
                }
            }

            sleep_interruptible(Duration::from_millis(100), abort)?;
        }

        StepType::SwitchRoute => {
            let switch = step.params["switch"].as_str().unwrap_or("sd");
            let route = step.params["route"].as_str().unwrap_or("target");
            client.send_text(BoardRequest {
                method: reqwest::Method::PUT,
                path: format!("/api/v1/switch/{switch}"),
                query: vec![],
                body: Some(serde_json::json!({"route": route})),
            })?;
        }

        StepType::Capture => {
            // Capture is complex - for the initial implementation, arm and trigger via WS
            // For now, use a simplified HTTP-based approach
            bail!("capture step not yet implemented in CLI; use WebUI for power capture");
        }
    }

    Ok(ctx)
}

fn sleep_interruptible(duration: Duration, abort: &Arc<AtomicBool>) -> Result<()> {
    let deadline = Instant::now() + duration;
    while Instant::now() < deadline {
        if abort.load(Ordering::SeqCst) {
            bail!("aborted");
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    Ok(())
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}

trait StepTypeName {
    fn step_type_name(&self) -> &str;
}

impl StepTypeName for crate::test_script::TestStep {
    fn step_type_name(&self) -> &str {
        match self.step_type {
            StepType::PowerOn => "power_on",
            StepType::PowerOff => "power_off",
            StepType::Delay => "delay",
            StepType::SerialWait => "serial_wait",
            StepType::SerialSend => "serial_send",
            StepType::SerialExpect => "serial_expect",
            StepType::AdcRead => "adc_read",
            StepType::GpioSet => "gpio_set",
            StepType::GpioAssert => "gpio_assert",
            StepType::SwitchRoute => "switch_route",
            StepType::Capture => "capture",
        }
    }
}
