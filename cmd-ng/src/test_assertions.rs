// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Assertion evaluation engine for linkr-test.v1.
// Compatible with the WebUI test automation framework.

use serde::{Deserialize, Serialize};
use serde_json::Value;

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct StepAssertion {
    #[serde(default)]
    pub continue_on_error: Option<bool>,
    #[serde(default)]
    pub current_range: Option<CurrentRange>,
    #[serde(default)]
    pub contains: Option<String>,
    #[serde(default)]
    pub regex: Option<String>,
    #[serde(default)]
    pub exit_code: Option<i32>,
    #[serde(default)]
    pub pin_direction: Option<String>,
    #[serde(default)]
    pub pin_value: Option<i32>,
    #[serde(default)]
    pub peak_current_max_a: Option<f64>,
    #[serde(default)]
    pub energy_max_j: Option<f64>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CurrentRange {
    pub min_a: f64,
    pub max_a: f64,
}

#[derive(Debug, Clone, Default)]
pub struct AssertionContext {
    pub adc_value_ua: Option<f64>,
    pub serial_output: Option<String>,
    pub exit_code: Option<i32>,
    pub pin_direction: Option<String>,
    pub pin_value: Option<i32>,
    pub peak_current_ua: Option<f64>,
    pub energy_uj: Option<f64>,
}

#[derive(Debug, Clone, Serialize)]
pub struct AssertionResult {
    pub passed: bool,
    pub detail: String,
}

pub fn evaluate(assert: &StepAssertion, ctx: &AssertionContext) -> AssertionResult {
    let mut failures = Vec::new();
    let mut passes = Vec::new();

    if let Some(ref range) = assert.current_range {
        match ctx.adc_value_ua {
            Some(ua) => {
                let min_ua = range.min_a * 1e6;
                let max_ua = range.max_a * 1e6;
                if ua >= min_ua && ua <= max_ua {
                    passes.push(format!("current {:.6}A in range", ua / 1e6));
                } else {
                    failures.push(format!(
                        "current {:.6}A outside range [{}, {}]A",
                        ua / 1e6, range.min_a, range.max_a
                    ));
                }
            }
            None => failures.push("adcValueUa is unavailable".to_string()),
        }
    }

    if let Some(ref expected) = assert.contains {
        match &ctx.serial_output {
            Some(output) => {
                let stripped = strip_ansi(output);
                if stripped.contains(expected) {
                    passes.push(format!("output contains {:?}", expected));
                } else {
                    failures.push(format!("output does not contain {:?}", expected));
                }
            }
            None => failures.push("serialOutput is unavailable".to_string()),
        }
    }

    if let Some(ref pattern) = assert.regex {
        match &ctx.serial_output {
            Some(output) => {
                let stripped = strip_ansi(output);
                match regex::Regex::new(pattern) {
                    Ok(re) => {
                        if re.is_match(&stripped) {
                            passes.push(format!("output matches /{}/", pattern));
                        } else {
                            failures.push(format!("output does not match /{}/", pattern));
                        }
                    }
                    Err(e) => failures.push(format!("invalid regex: {e}")),
                }
            }
            None => failures.push("serialOutput is unavailable".to_string()),
        }
    }

    if let Some(expected_code) = assert.exit_code {
        match ctx.exit_code {
            Some(code) => {
                if code == expected_code {
                    passes.push(format!("exit code {code}"));
                } else {
                    failures.push(format!("exit code {code}, expected {expected_code}"));
                }
            }
            None => failures.push("exitCode is unavailable".to_string()),
        }
    }

    if let Some(ref expected_dir) = assert.pin_direction {
        match &ctx.pin_direction {
            Some(dir) => {
                if dir == expected_dir {
                    passes.push(format!("direction {:?}", dir));
                } else {
                    failures.push(format!("direction {:?}, expected {:?}", dir, expected_dir));
                }
            }
            None => failures.push("pinDirection is unavailable".to_string()),
        }
    }

    if let Some(expected_val) = assert.pin_value {
        match ctx.pin_value {
            Some(val) => {
                if val == expected_val {
                    passes.push(format!("value {val}"));
                } else {
                    failures.push(format!("value {val}, expected {expected_val}"));
                }
            }
            None => failures.push("pinValue is unavailable".to_string()),
        }
    }

    if let Some(max_a) = assert.peak_current_max_a {
        match ctx.peak_current_ua {
            Some(ua) => {
                let max_ua = max_a * 1e6;
                if ua <= max_ua {
                    passes.push(format!("peak {:.6}A within limit", ua / 1e6));
                } else {
                    failures.push(format!("peak {:.6}A exceeds max {}A", ua / 1e6, max_a));
                }
            }
            None => failures.push("peakCurrentUa is unavailable".to_string()),
        }
    }

    if let Some(max_j) = assert.energy_max_j {
        match ctx.energy_uj {
            Some(uj) => {
                let max_uj = max_j * 1e6;
                if uj <= max_uj {
                    passes.push(format!("energy {:.6}J within limit", uj / 1e6));
                } else {
                    failures.push(format!("energy {:.6}J exceeds max {}J", uj / 1e6, max_j));
                }
            }
            None => failures.push("energyUj is unavailable".to_string()),
        }
    }

    if failures.is_empty() {
        AssertionResult {
            passed: true,
            detail: if passes.is_empty() {
                "no assertions".to_string()
            } else {
                passes.join("; ")
            },
        }
    } else {
        AssertionResult {
            passed: false,
            detail: failures.join("; "),
        }
    }
}

fn strip_ansi(s: &str) -> String {
    let mut result = String::with_capacity(s.len());
    let mut chars = s.chars().peekable();
    while let Some(c) = chars.next() {
        if c == '\x1b' {
            // Skip ANSI escape sequence
            if chars.peek() == Some(&'[') {
                chars.next();
                while let Some(&next) = chars.peek() {
                    if next.is_ascii_alphabetic() || next == 'm' {
                        chars.next();
                        break;
                    }
                    chars.next();
                }
            }
        } else {
            result.push(c);
        }
    }
    result
}

/// Merge implicit gpio_assert assertions with explicit ones.
pub fn merge_gpio_assertion(assert: &mut Value, _pin: &str, direction: &str, value: i32) {
    if let Some(obj) = assert.as_object_mut() {
        obj.entry("pin_direction")
            .or_insert_with(|| Value::String(direction.to_string()));
        obj.entry("pin_value")
            .or_insert_with(|| Value::Number(value.into()));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn current_range_pass() {
        let assert = StepAssertion {
            current_range: Some(CurrentRange {
                min_a: 0.0,
                max_a: 3.0,
            }),
            ..Default::default()
        };
        let ctx = AssertionContext {
            adc_value_ua: Some(1_500_000.0),
            ..Default::default()
        };
        let result = evaluate(&assert, &ctx);
        assert!(result.passed);
    }

    #[test]
    fn current_range_fail() {
        let assert = StepAssertion {
            current_range: Some(CurrentRange {
                min_a: 0.0,
                max_a: 1.0,
            }),
            ..Default::default()
        };
        let ctx = AssertionContext {
            adc_value_ua: Some(2_000_000.0),
            ..Default::default()
        };
        let result = evaluate(&assert, &ctx);
        assert!(!result.passed);
    }

    #[test]
    fn contains_pass() {
        let assert = StepAssertion {
            contains: Some("Linux".to_string()),
            ..Default::default()
        };
        let ctx = AssertionContext {
            serial_output: Some("Linux version 6.1.0".to_string()),
            ..Default::default()
        };
        assert!(evaluate(&assert, &ctx).passed);
    }

    #[test]
    fn exit_code_fail() {
        let assert = StepAssertion {
            exit_code: Some(0),
            ..Default::default()
        };
        let ctx = AssertionContext {
            exit_code: Some(1),
            ..Default::default()
        };
        assert!(!evaluate(&assert, &ctx).passed);
    }

    #[test]
    fn strip_ansi_removes_escape_codes() {
        assert_eq!(strip_ansi("\x1b[31mHello\x1b[0m"), "Hello");
    }
}
