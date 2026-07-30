// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Test runner: executes linkr-test.v1 scripts against the board.

use anyhow::{bail, ensure, Context, Result};
use std::collections::HashMap;
use std::io::Write;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use crate::client::{BoardClient, BoardRequest, BoardTransport};
use crate::test_assertions::{self, AssertionContext, StepAssertion};
use crate::test_report::{self, ReportBuilder, StepResult, StepStatus};
use crate::test_script::{expand_steps, StepType, TestScript, TestStep};

pub struct RunOptions {
    pub output_path: Option<String>,
    pub serial_uart0_path: Option<String>,
    pub serial_uart1_path: Option<String>,
    pub json_output: bool,
    pub verbose: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum TestSerialChannel {
    Uart0,
    Uart1,
}

impl TestSerialChannel {
    fn name(self) -> &'static str {
        match self {
            Self::Uart0 => "uart0",
            Self::Uart1 => "uart1",
        }
    }
}

#[derive(Default)]
struct TestSerialPorts {
    uart0: Option<crate::test_serial::SerialPort>,
    uart1: Option<crate::test_serial::SerialPort>,
}

impl TestSerialPorts {
    #[cfg(test)]
    fn open(script: &TestScript, opts: &RunOptions) -> Result<Self> {
        let steps = expand_steps(&script.steps)?;
        Self::open_steps(&steps, opts)
    }

    fn open_steps(steps: &[TestStep], opts: &RunOptions) -> Result<Self> {
        let (needs_uart0, needs_uart1) = required_serial_channels_for_steps(steps)?;
        let uart1_path = if needs_uart1 {
            Some(
                opts.serial_uart1_path
                    .as_deref()
                    .context("uart1 steps require --serial-uart1 PATH")?,
            )
        } else {
            None
        };
        ensure!(
            opts.serial_uart0_path.as_deref().is_none()
                || opts.serial_uart0_path.as_deref() != uart1_path,
            "uart0 and uart1 must use different serial device paths"
        );

        let uart0 = if needs_uart0 {
            Some(match opts.serial_uart0_path.as_deref() {
                Some(path) => crate::test_serial::SerialPort::open(path, 115200)?,
                None => match uart1_path {
                    Some(path) => crate::test_serial::SerialPort::auto_detect_excluding(&[path])?,
                    None => crate::test_serial::SerialPort::auto_detect()?,
                },
            })
        } else {
            None
        };
        let uart1 = uart1_path
            .map(|path| crate::test_serial::SerialPort::open(path, 115200))
            .transpose()?;

        Ok(Self { uart0, uart1 })
    }

    fn get_mut(
        &mut self,
        channel: TestSerialChannel,
    ) -> Result<&mut crate::test_serial::SerialPort> {
        match channel {
            TestSerialChannel::Uart0 => self.uart0.as_mut(),
            TestSerialChannel::Uart1 => self.uart1.as_mut(),
        }
        .with_context(|| format!("{} serial port is not available", channel.name()))
    }
}

pub fn run_script(
    script: &mut TestScript,
    client: &BoardClient,
    opts: &RunOptions,
    stdout: &mut dyn Write,
    stderr: &mut dyn Write,
) -> Result<u8> {
    let mut execution_steps = expand_steps(&script.steps)?;
    let total_steps = execution_steps.len();
    let mut report = ReportBuilder::new();
    let abort = Arc::new(AtomicBool::new(false));

    // Set up Ctrl+C handler
    let abort_clone = abort.clone();
    let _ = ctrlc::set_handler(move || {
        abort_clone.store(true, Ordering::SeqCst);
    });

    // Validate every serial channel before performing any board-side action.
    let mut serial = TestSerialPorts::open_steps(&execution_steps, opts)?;
    let mut condition_outcomes = HashMap::<String, bool>::new();

    for step in &mut execution_steps {
        if abort.load(Ordering::SeqCst) {
            report.set_aborted();
            break;
        }

        let start = Instant::now();
        let started_at_ms = now_ms();

        if opts.verbose && !opts.json_output {
            writeln!(stderr, "  [{}] {} ...", step.id, step.step_type_name())?;
        }

        let condition_id = step.params["__condition_id"].as_str();
        let condition_role = step.params["__condition_role"].as_str();
        let branch_selected = match (condition_id, condition_role) {
            (Some(id), Some("then")) => condition_outcomes.get(id).copied() == Some(true),
            (Some(id), Some("else")) => condition_outcomes.get(id).copied() == Some(false),
            _ => true,
        };
        let result =
            branch_selected.then(|| execute_step(step, client, &mut serial, &abort, opts, stderr));

        let finished_at_ms = now_ms();
        let duration_ms = start.elapsed().as_millis() as u64;

        let mut continue_on_error = step.continue_on_error == Some(true);
        let timing = StepTiming {
            started_at_ms,
            finished_at_ms,
            duration_ms,
        };
        let mut step_result = match result {
            None => make_step_result(
                step,
                condition_id,
                condition_role,
                StepOutcome {
                    status: StepStatus::Skip,
                    timing,
                    error: Some("condition branch not selected".to_string()),
                    assertion_result: None,
                    adc_value_ua: None,
                    serial_output: None,
                    conditional_skip: true,
                },
            ),
            Some(result) => match result {
                Ok(mut ctx) => match assertion_for_step(step) {
                    Ok(Some(assertion)) => {
                        continue_on_error |= assertion.continue_on_error == Some(true);
                        let evaluated = test_assertions::evaluate(&assertion, &ctx);
                        let status = if evaluated.passed {
                            StepStatus::Pass
                        } else {
                            StepStatus::Fail
                        };
                        make_step_result(
                            step,
                            condition_id,
                            condition_role,
                            StepOutcome {
                                status,
                                timing,
                                error: None,
                                assertion_result: Some(evaluated),
                                adc_value_ua: ctx.adc_value_ua.take(),
                                serial_output: ctx.serial_output.take(),
                                conditional_skip: false,
                            },
                        )
                    }
                    Ok(None) => make_step_result(
                        step,
                        condition_id,
                        condition_role,
                        StepOutcome {
                            status: StepStatus::Pass,
                            timing,
                            error: None,
                            assertion_result: None,
                            adc_value_ua: ctx.adc_value_ua.take(),
                            serial_output: ctx.serial_output.take(),
                            conditional_skip: false,
                        },
                    ),
                    Err(error) => make_step_result(
                        step,
                        condition_id,
                        condition_role,
                        StepOutcome {
                            status: StepStatus::Error,
                            timing,
                            error: Some(error.to_string()),
                            assertion_result: None,
                            adc_value_ua: ctx.adc_value_ua.take(),
                            serial_output: ctx.serial_output.take(),
                            conditional_skip: false,
                        },
                    ),
                },
                Err(e) => {
                    let msg = e.to_string();
                    if abort.load(Ordering::SeqCst) || msg == "aborted" {
                        report.set_aborted();
                        make_step_result(
                            step,
                            condition_id,
                            condition_role,
                            StepOutcome {
                                status: StepStatus::Aborted,
                                timing,
                                error: Some(msg),
                                assertion_result: None,
                                adc_value_ua: None,
                                serial_output: None,
                                conditional_skip: false,
                            },
                        )
                    } else if msg.contains("skip") {
                        make_step_result(
                            step,
                            condition_id,
                            condition_role,
                            StepOutcome {
                                status: StepStatus::Skip,
                                timing,
                                error: Some(msg),
                                assertion_result: None,
                                adc_value_ua: None,
                                serial_output: None,
                                conditional_skip: false,
                            },
                        )
                    } else {
                        make_step_result(
                            step,
                            condition_id,
                            condition_role,
                            StepOutcome {
                                status: StepStatus::Error,
                                timing,
                                error: Some(msg),
                                assertion_result: None,
                                adc_value_ua: None,
                                serial_output: None,
                                conditional_skip: false,
                            },
                        )
                    }
                }
            },
        };

        if let (Some(id), Some("check")) = (condition_id, condition_role) {
            match step_result.status {
                StepStatus::Pass => {
                    condition_outcomes.insert(id.to_string(), true);
                    step_result.condition_outcome = Some(true);
                }
                StepStatus::Fail => {
                    condition_outcomes.insert(id.to_string(), false);
                    step_result.status = StepStatus::Pass;
                    step_result.condition_outcome = Some(false);
                }
                _ => {}
            }
        }

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

        let unresolved_condition =
            condition_role == Some("check") && step_result.condition_outcome.is_none();
        let should_stop = unresolved_condition
            || matches!(
                step_result.status,
                StepStatus::Fail | StepStatus::Error | StepStatus::Aborted
            ) && !continue_on_error;

        report.add_result(step_result);

        if should_stop {
            break;
        }
    }

    let summary = report.summary(total_steps);
    let success = report.is_successful(total_steps);

    if let Some(path) = opts.output_path.as_deref() {
        test_report::write_report_file(
            path,
            &script.header.name,
            &script.steps,
            report.results(),
            &summary,
        )?;
    }

    // Print summary
    if opts.json_output {
        test_report::write_json_summary(stdout, &summary, success)?;
    } else {
        test_report::print_summary(stderr, &summary)?;
    }

    Ok(if success { 0 } else { 1 })
}

fn execute_step(
    step: &crate::test_script::TestStep,
    client: &dyn BoardTransport,
    serial: &mut TestSerialPorts,
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
            let serial = serial.get_mut(serial_channel_for_step(step)?)?;
            let pattern = step.params["pattern"].as_str().unwrap_or("login:");
            let timeout_ms = step.params["timeout_ms"].as_u64().unwrap_or(60000);

            let result = serial.wait_for_pattern(pattern, Duration::from_millis(timeout_ms))?;
            ctx.serial_output = Some(result.output.clone());

            if result.timed_out {
                bail!("timeout waiting for pattern: {pattern}");
            }
        }

        StepType::SerialSend => {
            let serial = serial.get_mut(serial_channel_for_step(step)?)?;
            let text = step.params["text"].as_str().unwrap_or("root\n");
            serial.write(text)?;
        }

        StepType::SerialExpect => {
            let serial = serial.get_mut(serial_channel_for_step(step)?)?;
            let command = step.params["command"].as_str().unwrap_or("uname -a");
            let pattern = step.params["pattern"].as_str().unwrap_or("Linux");
            let timeout_ms = step.params["timeout_ms"].as_u64().unwrap_or(10000);

            let result =
                serial.send_and_expect(command, pattern, Duration::from_millis(timeout_ms))?;
            // The assertion engine below reports a pattern mismatch as a test failure,
            // while transport timeouts remain infrastructure errors.
            let _ = result.pattern_matched;
            ctx.serial_output = Some(result.output.clone());
            ctx.exit_code = Some(result.exit_code);

            if !result.completed {
                bail!("timeout waiting for command completion");
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
            // Read GPIO state from snapshot
            let output = client.send_text(BoardRequest {
                method: reqwest::Method::GET,
                path: "/api/v1/gpio".to_string(),
                query: vec![],
                body: None,
            })?;

            let value: serde_json::Value = serde_json::from_str(&output)?;
            if let Some(gpios) = value["gpios"].as_array() {
                let requested_pin_number =
                    pin.strip_prefix("GP").unwrap_or(pin).parse::<u64>().ok();
                let found = gpios.iter().find(|g| {
                    g["name"].as_str() == Some(pin)
                        || g["note"].as_str() == Some(pin)
                        || g["pin"].as_str() == Some(pin)
                        || requested_pin_number
                            .is_some_and(|value| g["pin"].as_u64() == Some(value))
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

        StepType::Loop | StepType::Condition => {
            bail!("flow-control step was not expanded before execution")
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

fn is_serial_step(step_type: StepType) -> bool {
    matches!(
        step_type,
        StepType::SerialWait | StepType::SerialSend | StepType::SerialExpect
    )
}

fn serial_channel_for_step(step: &crate::test_script::TestStep) -> Result<TestSerialChannel> {
    match step.params["channel"].as_str().unwrap_or("uart0") {
        "uart0" => Ok(TestSerialChannel::Uart0),
        "uart1" => Ok(TestSerialChannel::Uart1),
        channel => bail!(
            "invalid serial channel for step {}: {channel}; expected uart0 or uart1",
            step.id
        ),
    }
}

#[cfg(test)]
fn required_serial_channels(script: &TestScript) -> Result<(bool, bool)> {
    let steps = expand_steps(&script.steps)?;
    required_serial_channels_for_steps(&steps)
}

fn required_serial_channels_for_steps(steps: &[TestStep]) -> Result<(bool, bool)> {
    let mut uart0 = false;
    let mut uart1 = false;
    for step in steps.iter().filter(|step| is_serial_step(step.step_type)) {
        match serial_channel_for_step(step)? {
            TestSerialChannel::Uart0 => uart0 = true,
            TestSerialChannel::Uart1 => uart1 = true,
        }
    }
    Ok((uart0, uart1))
}

#[derive(Clone, Copy)]
struct StepTiming {
    started_at_ms: u64,
    finished_at_ms: u64,
    duration_ms: u64,
}

struct StepOutcome {
    status: StepStatus,
    timing: StepTiming,
    error: Option<String>,
    assertion_result: Option<test_assertions::AssertionResult>,
    adc_value_ua: Option<f64>,
    serial_output: Option<String>,
    conditional_skip: bool,
}

fn make_step_result(
    step: &crate::test_script::TestStep,
    condition_id: Option<&str>,
    condition_role: Option<&str>,
    outcome: StepOutcome,
) -> StepResult {
    StepResult {
        step_id: step.id.clone(),
        step_type: step.step_type,
        unit_id: step.params["__unit_id"].as_str().map(str::to_string),
        unit_name: step.params["__unit_name"].as_str().map(str::to_string),
        condition_id: condition_id
            .or_else(|| step.params["__condition_id"].as_str())
            .map(str::to_string),
        condition_role: condition_role
            .or_else(|| step.params["__condition_role"].as_str())
            .map(str::to_string),
        condition_outcome: None,
        conditional_skip: outcome.conditional_skip,
        status: outcome.status,
        started_at_ms: outcome.timing.started_at_ms,
        finished_at_ms: outcome.timing.finished_at_ms,
        duration_ms: outcome.timing.duration_ms,
        error: outcome.error,
        assertion_result: outcome.assertion_result,
        adc_value_ua: outcome.adc_value_ua,
        serial_output: outcome.serial_output,
    }
}

fn assertion_for_step(step: &crate::test_script::TestStep) -> Result<Option<StepAssertion>> {
    let mut value = step.assert.clone().unwrap_or_else(|| serde_json::json!({}));

    if step.step_type == StepType::GpioAssert {
        let direction = step.params["direction"].as_str().unwrap_or("output");
        ensure!(
            matches!(direction, "input" | "output"),
            "invalid GPIO direction for step {}: {direction}",
            step.id
        );
        let pin_value = step.params["value"].as_i64().unwrap_or(1);
        ensure!(
            matches!(pin_value, 0 | 1),
            "invalid GPIO value for step {}: {pin_value}",
            step.id
        );
        test_assertions::merge_gpio_assertion(&mut value, direction, pin_value as i32);
    }

    if step.step_type == StepType::SerialExpect {
        let object = value
            .as_object_mut()
            .with_context(|| format!("invalid assertion for step {}", step.id))?;
        let pattern = step.params["pattern"].as_str().unwrap_or("Linux").trim();
        if !pattern.is_empty() {
            object
                .entry("regex")
                .or_insert_with(|| serde_json::json!(pattern));
        } else {
            object.remove("regex");
        }
        object
            .entry("exit_code")
            .or_insert_with(|| serde_json::json!(0));
    }

    if value.as_object().is_some_and(serde_json::Map::is_empty) {
        return Ok(None);
    }

    serde_json::from_value(value)
        .with_context(|| format!("invalid assertion for step {}", step.id))
        .map(Some)
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
            StepType::Loop => "loop",
            StepType::Condition => "condition",
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_script::{TestHeader, TestStep};

    fn gpio_assert_step(
        params: serde_json::Value,
        assertion: Option<serde_json::Value>,
    ) -> TestStep {
        TestStep {
            id: "gpio-check".to_string(),
            step_type: StepType::GpioAssert,
            params,
            assert: assertion,
            continue_on_error: None,
        }
    }

    #[test]
    fn gpio_assert_uses_step_params_as_expectations() {
        let step = gpio_assert_step(
            serde_json::json!({"pin": "GP13", "direction": "input", "value": 0}),
            None,
        );
        let assertion = assertion_for_step(&step).unwrap().unwrap();
        assert_eq!(assertion.pin_direction.as_deref(), Some("input"));
        assert_eq!(assertion.pin_value, Some(0));
    }

    #[test]
    fn gpio_assert_rejects_invalid_values() {
        let step = gpio_assert_step(
            serde_json::json!({"pin": "GP13", "direction": "output", "value": 2}),
            None,
        );
        assert!(assertion_for_step(&step).is_err());
    }

    #[test]
    fn serial_expect_uses_pattern_and_zero_exit_code_as_assertions() {
        let step = TestStep {
            id: "serial-check".to_string(),
            step_type: StepType::SerialExpect,
            params: serde_json::json!({
                "channel": "uart0",
                "command": "uname -a",
                "pattern": "Linux"
            }),
            assert: None,
            continue_on_error: None,
        };
        let assertion = assertion_for_step(&step).unwrap().unwrap();
        assert_eq!(assertion.regex.as_deref(), Some("Linux"));
        assert_eq!(assertion.exit_code, Some(0));
    }

    #[test]
    fn serial_expect_skips_empty_pattern_and_keeps_assert_continue_flag() {
        let step = TestStep {
            id: "serial-check".to_string(),
            step_type: StepType::SerialExpect,
            params: serde_json::json!({
                "channel": "uart0",
                "command": "stress-ng",
                "pattern": "   "
            }),
            assert: Some(serde_json::json!({"continue_on_error": true})),
            continue_on_error: None,
        };
        let assertion = assertion_for_step(&step).unwrap().unwrap();
        assert_eq!(assertion.regex, None);
        assert_eq!(assertion.exit_code, Some(0));
        assert_eq!(assertion.continue_on_error, Some(true));
    }

    fn serial_step(channel: &str) -> TestStep {
        TestStep {
            id: format!("wait-{channel}"),
            step_type: StepType::SerialWait,
            params: serde_json::json!({"channel": channel, "pattern": "login:"}),
            assert: None,
            continue_on_error: None,
        }
    }

    fn script_with_steps(steps: Vec<TestStep>) -> TestScript {
        TestScript {
            header: TestHeader {
                schema: "linkr-test.v1".to_string(),
                name: "serial-routing".to_string(),
                version: "1.0".to_string(),
                board: None,
                created: None,
            },
            steps,
        }
    }

    #[test]
    fn serial_channel_preflight_tracks_both_uarts() {
        let script = script_with_steps(vec![serial_step("uart0"), serial_step("uart1")]);
        assert_eq!(required_serial_channels(&script).unwrap(), (true, true));
    }

    #[test]
    fn serial_channel_preflight_rejects_unknown_channel() {
        let script = script_with_steps(vec![serial_step("uart2")]);
        assert!(required_serial_channels(&script)
            .unwrap_err()
            .to_string()
            .contains("expected uart0 or uart1"));
    }

    #[test]
    fn uart1_requires_an_explicit_device_path() {
        let script = script_with_steps(vec![serial_step("uart1")]);
        let options = RunOptions {
            output_path: None,
            serial_uart0_path: None,
            serial_uart1_path: None,
            json_output: false,
            verbose: false,
        };
        assert!(TestSerialPorts::open(&script, &options)
            .err()
            .expect("uart1 without a path must fail")
            .to_string()
            .contains("--serial-uart1"));
    }

    #[test]
    fn dual_uart_rejects_the_same_device_path() {
        let script = script_with_steps(vec![serial_step("uart0"), serial_step("uart1")]);
        let options = RunOptions {
            output_path: None,
            serial_uart0_path: Some("/dev/serial-a".to_string()),
            serial_uart1_path: Some("/dev/serial-a".to_string()),
            json_output: false,
            verbose: false,
        };
        assert!(TestSerialPorts::open(&script, &options)
            .err()
            .expect("duplicate paths must fail before opening the device")
            .to_string()
            .contains("different serial device paths"));
    }
}
