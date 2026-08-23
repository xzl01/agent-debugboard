// SPDX-License-Identifier: LGPL-3.0-or-later
//
// NDJSON test script parser for linkr-test.v1 schema.
// Compatible with the WebUI test automation framework.

use anyhow::{bail, ensure, Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::HashSet;
use std::io::{BufRead, BufReader, Read};

pub const SCHEMA: &str = "linkr-test.v1";
pub const MIN_LOOP_COUNT: u64 = 1;
pub const MAX_LOOP_COUNT: u64 = 1000;
pub const MAX_EXECUTION_STEPS: usize = 10_000;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TestHeader {
    pub schema: String,
    #[serde(default = "default_name")]
    pub name: String,
    #[serde(default = "default_version")]
    pub version: String,
    #[serde(default)]
    pub board: Option<String>,
    #[serde(default)]
    pub created: Option<String>,
}

fn default_name() -> String {
    "Untitled".to_string()
}

fn default_version() -> String {
    "1.0".to_string()
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TestStep {
    pub id: String,
    #[serde(rename = "type")]
    pub step_type: StepType,
    #[serde(default)]
    pub params: Value,
    #[serde(default)]
    pub assert: Option<Value>,
    #[serde(default)]
    pub continue_on_error: Option<bool>,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum StepType {
    PowerOn,
    PowerOff,
    Delay,
    SerialWait,
    SerialSend,
    SerialExpect,
    AdcRead,
    GpioSet,
    GpioAssert,
    SwitchRoute,
    Capture,
    Loop,
    Condition,
}

#[derive(Debug, Clone)]
pub struct TestScript {
    pub header: TestHeader,
    pub steps: Vec<TestStep>,
}

pub fn parse_script(reader: impl Read) -> Result<TestScript> {
    let buf_reader = BufReader::new(reader);
    let mut lines = buf_reader.lines();
    let header_line = match lines.next() {
        Some(Ok(line)) => line,
        Some(Err(e)) => bail!("failed to read script: {e}"),
        None => bail!("empty script"),
    };

    let header: TestHeader =
        serde_json::from_str(&header_line).context("failed to parse script header")?;
    if header.schema != SCHEMA {
        bail!("unknown schema {:?}, expected {:?}", header.schema, SCHEMA);
    }

    let mut steps = Vec::new();
    for (i, line_result) in lines.enumerate() {
        let line = line_result.context("failed to read script line")?;
        let trimmed = line.trim();
        if trimmed.is_empty() {
            continue;
        }

        let step: TestStep = serde_json::from_str(trimmed)
            .with_context(|| format!("failed to parse step on line {}", i + 2))?;

        if step.id.is_empty() {
            bail!("step on line {} has empty id", i + 2);
        }
        if step.params != Value::Null && !step.params.is_object() {
            bail!("step {} params must be an object", step.id);
        }

        steps.push(step);
    }

    // Validate loop blocks before any board or serial side effects are possible.
    let _ = expand_steps(&steps)?;

    Ok(TestScript { header, steps })
}

/// Expand flow blocks into executable steps. Named Units may be nested one
/// level inside loops and condition branches; anonymous loops and conditions
/// remain top-level to keep the workflow graph bounded and predictable.
pub fn expand_steps(steps: &[TestStep]) -> Result<Vec<TestStep>> {
    let mut expanded = Vec::new();
    let mut item_ids = HashSet::new();
    let mut execution_ids = HashSet::new();

    for step in steps {
        ensure!(!step.id.is_empty(), "test step has an empty id");
        ensure!(
            item_ids.insert(step.id.clone()),
            "duplicate script item id: {}",
            step.id
        );

        if step.step_type == StepType::Condition {
            let check = step
                .params
                .get("check")
                .with_context(|| format!("condition {} check must be a test step", step.id))?;
            let then_steps = step
                .params
                .get("then_steps")
                .and_then(Value::as_array)
                .with_context(|| format!("condition {} then_steps must be an array", step.id))?;
            let else_steps = step
                .params
                .get("else_steps")
                .and_then(Value::as_array)
                .with_context(|| format!("condition {} else_steps must be an array", step.id))?;
            let mut entries = Vec::with_capacity(1 + then_steps.len() + else_steps.len());
            entries.push(("check", check));
            entries.extend(then_steps.iter().map(|child| ("then", child)));
            entries.extend(else_steps.iter().map(|child| ("else", child)));
            let mut child_ids = HashSet::new();

            for (role, child) in entries {
                let executable: TestStep =
                    serde_json::from_value(child.clone()).with_context(|| {
                        format!(
                            "condition {} {role} entry is not a valid test step",
                            step.id
                        )
                    })?;
                ensure!(
                    !executable.id.is_empty(),
                    "condition {} {role} step has an empty id",
                    step.id
                );
                ensure!(
                    child_ids.insert(executable.id.clone()),
                    "condition {} has duplicate child step id: {}",
                    step.id,
                    executable.id
                );
                if role == "check" {
                    ensure!(
                        matches!(
                            executable.step_type,
                            StepType::SerialExpect | StepType::AdcRead | StepType::GpioAssert
                        ),
                        "condition {} has unsupported check type",
                        step.id
                    );
                    if executable.step_type == StepType::AdcRead {
                        ensure!(
                            condition_check_has_assertion(&executable),
                            "condition {} adc_read check requires an assert",
                            step.id
                        );
                    }
                    if executable.step_type == StepType::SerialExpect {
                        ensure!(
                            serial_pattern_is_non_empty(&executable),
                            "condition {} serial_expect check pattern must be non-empty",
                            step.id
                        );
                    }
                }
                ensure!(
                    executable.step_type != StepType::Condition,
                    "nested conditions are not supported (condition {})",
                    step.id
                );

                if executable.step_type == StepType::Loop {
                    ensure!(
                        role != "check",
                        "condition {} has unsupported check type",
                        step.id
                    );
                    let (unit_name, unit_steps) = parse_unit(&executable, "condition branch")?;
                    for mut unit_step in unit_steps {
                        let source_id = unit_step.id.clone();
                        unit_step.id =
                            format!("{}@{}:1@{}:{role}", unit_step.id, executable.id, step.id);
                        set_execution_metadata(
                            &mut unit_step,
                            &source_id,
                            Some((&executable.id, &unit_name)),
                            Some((&step.id, role)),
                        );
                        push_execution(&mut expanded, &mut execution_ids, unit_step)?;
                    }
                    continue;
                }

                let mut executable = executable;
                ensure!(
                    executable.params == Value::Null || executable.params.is_object(),
                    "condition {} {role} step params must be an object",
                    step.id
                );
                apply_defaults(&mut executable);
                ensure_serial_wait_pattern(&executable)?;
                let source_id = executable.id.clone();
                executable.id = format!("{}@{}:{role}", executable.id, step.id);
                set_execution_metadata(&mut executable, &source_id, None, Some((&step.id, role)));
                push_execution(&mut expanded, &mut execution_ids, executable)?;
            }
            continue;
        }

        if step.step_type != StepType::Loop {
            let mut executable = step.clone();
            apply_defaults(&mut executable);
            ensure_serial_wait_pattern(&executable)?;
            push_execution(&mut expanded, &mut execution_ids, executable)?;
            continue;
        }

        if step.params.get("unit").is_some_and(|unit| !unit.is_null()) {
            let (unit_name, unit_steps) = parse_unit(step, "top-level Unit")?;
            for mut unit_step in unit_steps {
                let source_id = unit_step.id.clone();
                unit_step.id = format!("{}@{}:1", unit_step.id, step.id);
                set_execution_metadata(
                    &mut unit_step,
                    &source_id,
                    Some((&step.id, &unit_name)),
                    None,
                );
                push_execution(&mut expanded, &mut execution_ids, unit_step)?;
            }
            continue;
        }

        let count = loop_count(step)?;
        let children = step.params["steps"]
            .as_array()
            .with_context(|| format!("loop {} steps must be a non-empty array", step.id))?;
        ensure!(
            !children.is_empty(),
            "loop {} steps must be a non-empty array",
            step.id
        );
        let mut child_ids = HashSet::new();
        let mut child_templates = Vec::with_capacity(children.len());
        for (child_index, child) in children.iter().enumerate() {
            let executable: TestStep =
                serde_json::from_value(child.clone()).with_context(|| {
                    format!(
                        "loop {} step {} is not a valid test step",
                        step.id,
                        child_index + 1
                    )
                })?;
            ensure!(
                !executable.id.is_empty(),
                "loop {} step {} has an empty id",
                step.id,
                child_index + 1
            );
            ensure!(
                child_ids.insert(executable.id.clone()),
                "loop {} has duplicate child step id: {}",
                step.id,
                executable.id
            );
            ensure!(
                executable.step_type != StepType::Condition,
                "nested conditions are not supported (loop {})",
                step.id
            );
            child_templates.push(executable);
        }

        for iteration in 1..=count {
            for child in &child_templates {
                if child.step_type == StepType::Loop {
                    let (unit_name, unit_steps) = parse_unit(child, "loop body")?;
                    for mut unit_step in unit_steps {
                        let source_id = unit_step.id.clone();
                        unit_step.id =
                            format!("{}@{}:1@{}:{iteration}", unit_step.id, child.id, step.id);
                        set_execution_metadata(
                            &mut unit_step,
                            &source_id,
                            Some((&child.id, &unit_name)),
                            None,
                        );
                        push_execution(&mut expanded, &mut execution_ids, unit_step)?;
                    }
                    continue;
                }
                let mut executable = child.clone();
                ensure!(
                    executable.params == Value::Null || executable.params.is_object(),
                    "loop {} step {} params must be an object",
                    step.id,
                    executable.id
                );
                apply_defaults(&mut executable);
                ensure_serial_wait_pattern(&executable)?;
                let source_id = executable.id.clone();
                executable.id = format!("{}@{}:{iteration}", executable.id, step.id);
                set_execution_metadata(&mut executable, &source_id, None, None);
                push_execution(&mut expanded, &mut execution_ids, executable)?;
            }
        }
    }

    Ok(expanded)
}

fn condition_check_has_assertion(step: &TestStep) -> bool {
    let Some(assert) = step.assert.as_ref() else {
        return false;
    };
    let Some(object) = assert.as_object() else {
        return false;
    };
    object
        .iter()
        .any(|(key, value)| key != "continue_on_error" && !value.is_null())
}

fn serial_pattern_is_non_empty(step: &TestStep) -> bool {
    step.params
        .get("pattern")
        .and_then(Value::as_str)
        .map(str::trim)
        .is_some_and(|pattern| !pattern.is_empty())
}

fn ensure_serial_wait_pattern(step: &TestStep) -> Result<()> {
    if step.step_type == StepType::SerialWait {
        ensure!(
            serial_pattern_is_non_empty(step),
            "step {} serial_wait pattern must be non-empty",
            step.id
        );
    }
    Ok(())
}

fn loop_count(step: &TestStep) -> Result<u64> {
    let count = step.params["count"].as_u64().with_context(|| {
        format!(
            "loop {} count must be an integer between {MIN_LOOP_COUNT} and {MAX_LOOP_COUNT}",
            step.id
        )
    })?;
    ensure!(
        (MIN_LOOP_COUNT..=MAX_LOOP_COUNT).contains(&count),
        "loop {} count must be an integer between {MIN_LOOP_COUNT} and {MAX_LOOP_COUNT}",
        step.id
    );
    Ok(count)
}

fn parse_unit(step: &TestStep, location: &str) -> Result<(String, Vec<TestStep>)> {
    ensure!(
        step.step_type == StepType::Loop,
        "{location} must contain a Unit"
    );
    let count = loop_count(step)?;
    let name = step
        .params
        .get("unit")
        .and_then(|unit| unit.get("name"))
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|name| !name.is_empty() && name.chars().count() <= 80)
        .with_context(|| format!("{location}: only named Units may be nested"))?;
    ensure!(count == 1, "unit {} count must be exactly 1", step.id);
    let children = step.params["steps"]
        .as_array()
        .with_context(|| format!("unit {} steps must be a non-empty array", step.id))?;
    ensure!(
        !children.is_empty(),
        "unit {} steps must be a non-empty array",
        step.id
    );
    let mut child_ids = HashSet::new();
    let mut parsed = Vec::with_capacity(children.len());
    for (index, child) in children.iter().enumerate() {
        let mut executable: TestStep =
            serde_json::from_value(child.clone()).with_context(|| {
                format!(
                    "unit {} step {} is not a valid test step",
                    step.id,
                    index + 1
                )
            })?;
        ensure!(
            !executable.id.is_empty(),
            "unit {} step has an empty id",
            step.id
        );
        ensure!(
            child_ids.insert(executable.id.clone()),
            "unit {} has duplicate child step id: {}",
            step.id,
            executable.id
        );
        ensure!(
            !matches!(executable.step_type, StepType::Loop | StepType::Condition),
            "unit {} cannot contain nested flow blocks",
            step.id
        );
        ensure!(
            executable.params == Value::Null || executable.params.is_object(),
            "unit {} step {} params must be an object",
            step.id,
            executable.id
        );
        apply_defaults(&mut executable);
        ensure_serial_wait_pattern(&executable)?;
        parsed.push(executable);
    }
    Ok((name.to_string(), parsed))
}

fn set_execution_metadata(
    step: &mut TestStep,
    source_id: &str,
    unit: Option<(&str, &str)>,
    condition: Option<(&str, &str)>,
) {
    let Some(params) = step.params.as_object_mut() else {
        return;
    };
    params.insert(
        "__source_step_id".to_string(),
        Value::String(source_id.to_string()),
    );
    if let Some((unit_id, unit_name)) = unit {
        params.insert("__unit_id".to_string(), Value::String(unit_id.to_string()));
        params.insert(
            "__unit_name".to_string(),
            Value::String(unit_name.to_string()),
        );
    }
    if let Some((condition_id, role)) = condition {
        params.insert(
            "__condition_id".to_string(),
            Value::String(condition_id.to_string()),
        );
        params.insert(
            "__condition_role".to_string(),
            Value::String(role.to_string()),
        );
    }
}

fn push_execution(
    expanded: &mut Vec<TestStep>,
    execution_ids: &mut HashSet<String>,
    executable: TestStep,
) -> Result<()> {
    ensure!(
        expanded.len() < MAX_EXECUTION_STEPS,
        "expanded test exceeds {MAX_EXECUTION_STEPS} executable steps"
    );
    ensure!(
        execution_ids.insert(executable.id.clone()),
        "duplicate execution step id: {}",
        executable.id
    );
    expanded.push(executable);
    Ok(())
}

/// Apply default params for a step type (matches WebUI defaultStepParams).
pub fn apply_defaults(step: &mut TestStep) {
    if step.params == Value::Null {
        step.params = serde_json::json!({});
    }
    let defaults = match step.step_type {
        StepType::PowerOn => serde_json::json!({"rail": "5v_out"}),
        StepType::PowerOff => serde_json::json!({"rail": "5v_out"}),
        StepType::Delay => serde_json::json!({"ms": 1000}),
        StepType::SerialWait => {
            serde_json::json!({"channel": "uart0", "pattern": "login:", "timeout_ms": 60000})
        }
        StepType::SerialSend => serde_json::json!({"channel": "uart0", "text": "root\n"}),
        StepType::SerialExpect => {
            serde_json::json!({"channel": "uart0", "command": "uname -a", "pattern": "Linux", "timeout_ms": 10000})
        }
        StepType::AdcRead => serde_json::json!({"channel": "5v_out"}),
        StepType::GpioSet => serde_json::json!({"pin": "GP13", "direction": "output", "value": 1}),
        StepType::GpioAssert => {
            serde_json::json!({"pin": "GP13", "direction": "output", "value": 1})
        }
        StepType::SwitchRoute => serde_json::json!({"switch": "sd", "route": "target"}),
        StepType::Capture => {
            serde_json::json!({"rail": "5v_out", "trigger": "manual", "duration_ms": 5000, "threshold_a": 0.1})
        }
        StepType::Loop => serde_json::json!({"count": 2, "steps": []}),
        StepType::Condition => serde_json::json!({
            "check": {"id": "check", "type": "serial_expect", "params": {}},
            "then_steps": [],
            "else_steps": []
        }),
    };

    if let (Some(defaults_obj), Some(params_obj)) =
        (defaults.as_object(), step.params.as_object_mut())
    {
        for (key, value) in defaults_obj {
            params_obj.entry(key).or_insert_with(|| value.clone());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_valid_script() {
        let input = r#"{"schema":"linkr-test.v1","name":"test","version":"1.0"}
{"id":"s1","type":"power_on","params":{"rail":"5v_out"}}
{"id":"s2","type":"delay","params":{"ms":100}}"#;
        let script = parse_script(input.as_bytes()).unwrap();
        assert_eq!(script.header.name, "test");
        assert_eq!(script.steps.len(), 2);
        assert_eq!(script.steps[0].step_type, StepType::PowerOn);
        assert_eq!(script.steps[1].step_type, StepType::Delay);
    }

    #[test]
    fn reject_empty_script() {
        let result = parse_script("".as_bytes());
        assert!(result.is_err());
    }

    #[test]
    fn reject_wrong_schema() {
        let input = r#"{"schema":"wrong","name":"test","version":"1.0"}"#;
        let result = parse_script(input.as_bytes());
        assert!(result.is_err());
    }

    #[test]
    fn default_params_applied() {
        let mut step = TestStep {
            id: "s1".to_string(),
            step_type: StepType::PowerOn,
            params: serde_json::json!({}),
            assert: None,
            continue_on_error: None,
        };
        apply_defaults(&mut step);
        assert_eq!(step.params["rail"], "5v_out");
    }

    #[test]
    fn explicit_params_override_defaults() {
        let mut step = TestStep {
            id: "s1".to_string(),
            step_type: StepType::PowerOn,
            params: serde_json::json!({"rail": "12v_out"}),
            assert: None,
            continue_on_error: None,
        };
        apply_defaults(&mut step);
        assert_eq!(step.params["rail"], "12v_out");
    }

    #[test]
    fn loop_blocks_expand_with_unique_iteration_ids() {
        let input = r#"{"schema":"linkr-test.v1","name":"loop","version":"1.0"}
{"id":"loop1","type":"loop","params":{"count":3,"steps":[{"id":"send","type":"serial_send","params":{"channel":"uart1","text":"go\\n"}},{"id":"wait","type":"delay","params":{"ms":10}}]}}"#;
        let script = parse_script(input.as_bytes()).unwrap();
        let expanded = expand_steps(&script.steps).unwrap();
        assert_eq!(expanded.len(), 6);
        assert_eq!(expanded[0].id, "send@loop1:1");
        assert_eq!(expanded[1].id, "wait@loop1:1");
        assert_eq!(expanded[4].id, "send@loop1:3");
        assert_eq!(expanded[0].params["channel"], "uart1");
        assert_eq!(expanded[0].params["__source_step_id"], "send");
        assert_eq!(expanded[1].params["__source_step_id"], "wait");
    }

    #[test]
    fn unit_blocks_execute_once_and_preserve_identity() {
        let input = r#"{"schema":"linkr-test.v1","name":"unit","version":"1.0"}
{"id":"unit1","type":"loop","params":{"count":1,"unit":{"name":"Boot and login"},"steps":[{"id":"wait","type":"delay","params":{"ms":10}}]}}"#;
        let script = parse_script(input.as_bytes()).unwrap();
        let expanded = expand_steps(&script.steps).unwrap();
        assert_eq!(expanded.len(), 1);
        assert_eq!(expanded[0].params["__unit_id"], "unit1");
        assert_eq!(expanded[0].params["__unit_name"], "Boot and login");
    }

    #[test]
    fn condition_blocks_expand_with_branch_metadata() {
        let input = r#"{"schema":"linkr-test.v1","name":"condition","version":"1.0"}
{"id":"condition1","type":"condition","params":{"check":{"id":"check","type":"adc_read","params":{"channel":"5v_out"},"assert":{"current_range":{"min_a":0.1,"max_a":1.0}}},"then_steps":[{"id":"then","type":"delay","params":{"ms":10}}],"else_steps":[{"id":"else","type":"power_off","params":{"rail":"5v_out"}}]}}"#;
        let script = parse_script(input.as_bytes()).unwrap();
        let expanded = expand_steps(&script.steps).unwrap();
        assert_eq!(expanded.len(), 3);
        assert_eq!(expanded[0].id, "check@condition1:check");
        assert_eq!(expanded[1].id, "then@condition1:then");
        assert_eq!(expanded[2].id, "else@condition1:else");
        assert_eq!(expanded[0].params["__condition_id"], "condition1");
        assert_eq!(expanded[1].params["__condition_role"], "then");
        assert_eq!(expanded[2].params["__condition_role"], "else");
    }

    #[test]
    fn units_can_run_inside_loops_with_stable_identity() {
        let input = r#"{"schema":"linkr-test.v1","name":"looped-unit"}
{"id":"outer","type":"loop","params":{"count":2,"steps":[{"id":"stress-unit","type":"loop","params":{"count":1,"unit":{"name":"Stress"},"steps":[{"id":"run","type":"serial_expect","params":{"channel":"uart0","command":"stress-ng","pattern":"","timeout_ms":1000}},{"id":"cooldown","type":"delay","params":{"ms":10}}]}}]}}"#;
        let script = parse_script(input.as_bytes()).unwrap();
        let expanded = expand_steps(&script.steps).unwrap();
        assert_eq!(expanded.len(), 4);
        assert_eq!(expanded[0].id, "run@stress-unit:1@outer:1");
        assert_eq!(expanded[1].id, "cooldown@stress-unit:1@outer:1");
        assert_eq!(expanded[2].id, "run@stress-unit:1@outer:2");
        assert_eq!(expanded[0].params["__unit_id"], "stress-unit");
        assert_eq!(expanded[0].params["__unit_name"], "Stress");
    }

    #[test]
    fn units_can_run_inside_condition_branches() {
        let input = r#"{"schema":"linkr-test.v1","name":"conditional-unit"}
{"id":"gate","type":"condition","params":{"check":{"id":"check","type":"gpio_assert","params":{"pin":"GP13","direction":"input","value":1}},"then_steps":[{"id":"pass-unit","type":"loop","params":{"count":1,"unit":{"name":"Pass path"},"steps":[{"id":"pass-delay","type":"delay","params":{"ms":1}}]}}],"else_steps":[{"id":"fail-unit","type":"loop","params":{"count":1,"unit":{"name":"Fail path"},"steps":[{"id":"fail-delay","type":"delay","params":{"ms":1}}]}}]}}"#;
        let script = parse_script(input.as_bytes()).unwrap();
        let expanded = expand_steps(&script.steps).unwrap();
        assert_eq!(expanded.len(), 3);
        assert_eq!(expanded[1].id, "pass-delay@pass-unit:1@gate:then");
        assert_eq!(expanded[2].id, "fail-delay@fail-unit:1@gate:else");
        assert_eq!(expanded[1].params["__condition_role"], "then");
        assert_eq!(expanded[2].params["__condition_role"], "else");
        assert_eq!(expanded[1].params["__unit_name"], "Pass path");
    }

    #[test]
    fn condition_blocks_reject_unsupported_checks_and_duplicate_children() {
        let unsupported = r#"{"schema":"linkr-test.v1","name":"condition"}
{"id":"condition1","type":"condition","params":{"check":{"id":"check","type":"delay","params":{"ms":1}},"then_steps":[],"else_steps":[]}}"#;
        assert!(parse_script(unsupported.as_bytes())
            .unwrap_err()
            .to_string()
            .contains("unsupported check type"));

        let bare_adc = r#"{"schema":"linkr-test.v1","name":"condition"}
{"id":"condition1","type":"condition","params":{"check":{"id":"check","type":"adc_read","params":{"channel":"5v_out"}},"then_steps":[],"else_steps":[]}}"#;
        assert!(parse_script(bare_adc.as_bytes())
            .unwrap_err()
            .to_string()
            .contains("adc_read check requires an assert"));

        let empty_serial_expect = r#"{"schema":"linkr-test.v1","name":"condition"}
{"id":"condition1","type":"condition","params":{"check":{"id":"check","type":"serial_expect","params":{"channel":"uart0","command":"true","pattern":"","timeout_ms":100}},"then_steps":[],"else_steps":[]}}"#;
        assert!(parse_script(empty_serial_expect.as_bytes())
            .unwrap_err()
            .to_string()
            .contains("serial_expect check pattern must be non-empty"));

        let empty_wait = r#"{"schema":"linkr-test.v1","name":"wait"}
{"id":"wait1","type":"serial_wait","params":{"channel":"uart0","pattern":"   ","timeout_ms":1000}}"#;
        assert!(parse_script(empty_wait.as_bytes())
            .unwrap_err()
            .to_string()
            .contains("serial_wait pattern must be non-empty"));

        let duplicate = r#"{"schema":"linkr-test.v1","name":"condition"}
{"id":"condition1","type":"condition","params":{"check":{"id":"same","type":"gpio_assert","params":{"pin":"GP13","direction":"input","value":0}},"then_steps":[{"id":"same","type":"delay","params":{"ms":1}}],"else_steps":[]}}"#;
        assert!(parse_script(duplicate.as_bytes())
            .unwrap_err()
            .to_string()
            .contains("duplicate child step id"));
    }

    #[test]
    fn repeated_unit_blocks_are_rejected() {
        let input = r#"{"schema":"linkr-test.v1","name":"unit","version":"1.0"}
{"id":"unit1","type":"loop","params":{"count":2,"unit":{"name":"Power cycle"},"steps":[{"id":"off","type":"power_off","params":{"rail":"5v_out"}}]}}"#;
        assert!(parse_script(input.as_bytes())
            .unwrap_err()
            .to_string()
            .contains("count must be exactly 1"));
    }

    #[test]
    fn invalid_and_nested_loops_are_rejected() {
        let empty = r#"{"schema":"linkr-test.v1","name":"empty"}
{"id":"loop1","type":"loop","params":{"count":2,"steps":[]}}"#;
        assert!(parse_script(empty.as_bytes()).is_err());

        let nested = r#"{"schema":"linkr-test.v1","name":"nested"}
{"id":"loop1","type":"loop","params":{"count":2,"steps":[{"id":"loop2","type":"loop","params":{"count":2,"steps":[]}}]}}"#;
        assert!(parse_script(nested.as_bytes())
            .unwrap_err()
            .to_string()
            .contains("only named Units may be nested"));
    }

    #[test]
    fn duplicate_script_child_and_execution_ids_are_rejected() {
        let duplicate_items = r#"{"schema":"linkr-test.v1","name":"duplicates"}
{"id":"same","type":"delay","params":{"ms":1}}
{"id":"same","type":"delay","params":{"ms":1}}"#;
        assert!(parse_script(duplicate_items.as_bytes())
            .unwrap_err()
            .to_string()
            .contains("duplicate script item id"));

        let duplicate_children = r#"{"schema":"linkr-test.v1","name":"duplicates"}
{"id":"loop1","type":"loop","params":{"count":2,"steps":[{"id":"same","type":"delay","params":{"ms":1}},{"id":"same","type":"delay","params":{"ms":1}}]}}"#;
        assert!(parse_script(duplicate_children.as_bytes())
            .unwrap_err()
            .to_string()
            .contains("duplicate child step id"));

        let duplicate_execution = r#"{"schema":"linkr-test.v1","name":"duplicates"}
{"id":"child@loop1:1","type":"delay","params":{"ms":1}}
{"id":"loop1","type":"loop","params":{"count":1,"steps":[{"id":"child","type":"delay","params":{"ms":1}}]}}"#;
        assert!(parse_script(duplicate_execution.as_bytes())
            .unwrap_err()
            .to_string()
            .contains("duplicate execution step id"));
    }

    #[test]
    fn loop_expansion_limit_is_enforced() {
        let children = (0..11)
            .map(|index| {
                serde_json::json!({
                    "id": format!("s{index}"),
                    "type": "delay",
                    "params": {"ms": 1}
                })
            })
            .collect::<Vec<_>>();
        let steps = vec![TestStep {
            id: "loop1".to_string(),
            step_type: StepType::Loop,
            params: serde_json::json!({"count": 1000, "steps": children}),
            assert: None,
            continue_on_error: None,
        }];

        assert!(expand_steps(&steps)
            .unwrap_err()
            .to_string()
            .contains("10000 executable steps"));
    }
}
