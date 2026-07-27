// SPDX-License-Identifier: LGPL-3.0-or-later
//
// NDJSON test script parser for linkr-test.v1 schema.
// Compatible with the WebUI test automation framework.

use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::io::{BufRead, BufReader, Read};

pub const SCHEMA: &str = "linkr-test.v1";

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

    Ok(TestScript { header, steps })
}

/// Apply default params for a step type (matches WebUI defaultStepParams).
pub fn apply_defaults(step: &mut TestStep) {
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
        StepType::GpioSet => serde_json::json!({"pin": "GP13", "value": 1}),
        StepType::GpioAssert => {
            serde_json::json!({"pin": "GP13", "direction": "output", "value": 1})
        }
        StepType::SwitchRoute => serde_json::json!({"switch": "sd", "route": "target"}),
        StepType::Capture => {
            serde_json::json!({"rail": "5v_out", "trigger": "manual", "duration_ms": 5000, "threshold_a": 0.1})
        }
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
}
