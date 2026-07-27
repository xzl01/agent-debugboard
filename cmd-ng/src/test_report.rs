// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Test report generation for linkr-test-report.v1.
// Compatible with the WebUI test automation framework.

use serde::Serialize;
use serde_json::json;
use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::Path;

use crate::json_contract::JSON_SCHEMA;
use crate::test_assertions::AssertionResult;
use crate::test_script::{StepType, TestStep};

#[derive(Debug, Clone, Serialize)]
pub struct RunSummary {
    pub total: usize,
    pub passed: usize,
    pub failed: usize,
    pub skipped: usize,
    pub errored: usize,
    pub aborted: bool,
    pub completed: bool,
    pub duration_ms: u64,
}

#[derive(Debug, Clone, Serialize)]
pub struct StepResult {
    pub step_id: String,
    pub step_type: StepType,
    pub status: StepStatus,
    pub started_at_ms: u64,
    pub finished_at_ms: u64,
    pub duration_ms: u64,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub assertion_result: Option<AssertionResult>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub adc_value_ua: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub serial_output: Option<String>,
}

#[derive(Debug, Clone, Copy, Serialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum StepStatus {
    Pass,
    Fail,
    Skip,
    Error,
    Aborted,
}

pub struct ReportBuilder {
    steps: Vec<StepResult>,
    start_time: std::time::Instant,
    aborted: bool,
}

impl ReportBuilder {
    pub fn new() -> Self {
        Self {
            steps: Vec::new(),
            start_time: std::time::Instant::now(),
            aborted: false,
        }
    }

    pub fn add_result(&mut self, result: StepResult) {
        self.steps.push(result);
    }

    pub fn set_aborted(&mut self) {
        self.aborted = true;
    }

    pub fn results(&self) -> &[StepResult] {
        &self.steps
    }

    pub fn summary(&self, total_steps: usize) -> RunSummary {
        let passed = self
            .steps
            .iter()
            .filter(|r| r.status == StepStatus::Pass)
            .count();
        let failed = self
            .steps
            .iter()
            .filter(|r| r.status == StepStatus::Fail)
            .count();
        let skipped = self
            .steps
            .iter()
            .filter(|r| r.status == StepStatus::Skip)
            .count();
        let errored = self
            .steps
            .iter()
            .filter(|r| r.status == StepStatus::Error)
            .count();
        let duration = self.start_time.elapsed();

        RunSummary {
            total: total_steps,
            passed,
            failed,
            skipped,
            errored,
            aborted: self.aborted,
            completed: !self.aborted && (passed + skipped) == total_steps,
            duration_ms: duration.as_millis() as u64,
        }
    }

    pub fn is_successful(&self, total_steps: usize) -> bool {
        let summary = self.summary(total_steps);
        summary.completed && !summary.aborted && summary.failed == 0 && summary.errored == 0
    }
}

pub fn write_json_report(
    writer: &mut dyn Write,
    script_name: &str,
    script_steps: &[TestStep],
    results: &[StepResult],
    summary: &RunSummary,
) -> anyhow::Result<()> {
    let report = json!({
        "schema": "linkr-test-report.v1",
        "script": {
            "name": script_name,
            "steps": script_steps,
        },
        "summary": summary,
        "results": results,
    });
    serde_json::to_writer_pretty(&mut *writer, &report)?;
    writeln!(writer)?;
    Ok(())
}

pub fn write_csv_report(writer: &mut dyn Write, results: &[StepResult]) -> anyhow::Result<()> {
    writeln!(writer, "step_id,status,duration_ms,error,detail,adc_ua")?;
    for r in results {
        let detail = r
            .assertion_result
            .as_ref()
            .map(|a| a.detail.as_str())
            .unwrap_or("");
        let error = r.error.as_deref().unwrap_or("");
        let adc = r
            .adc_value_ua
            .map(|v| format!("{:.0}", v))
            .unwrap_or_default();
        writeln!(
            writer,
            "{},{},{},{},{},{}",
            csv_escape(&r.step_id),
            serde_json::to_string(&r.status)
                .unwrap_or_default()
                .trim_matches('"'),
            r.duration_ms,
            csv_escape(error),
            csv_escape(detail),
            adc
        )?;
    }
    Ok(())
}

pub fn write_report_file(
    output_path: &str,
    script_name: &str,
    script_steps: &[TestStep],
    results: &[StepResult],
    summary: &RunSummary,
) -> anyhow::Result<()> {
    let file = File::create(output_path)?;
    let mut writer = BufWriter::new(file);
    match Path::new(output_path)
        .extension()
        .and_then(|extension| extension.to_str())
        .unwrap_or("json")
        .to_ascii_lowercase()
        .as_str()
    {
        "csv" => write_csv_report(&mut writer, results),
        "ndjson" => write_ndjson_report(&mut writer, results),
        _ => write_json_report(&mut writer, script_name, script_steps, results, summary),
    }
}

fn csv_escape(value: &str) -> String {
    if value.contains([',', '"', '\n', '\r']) {
        format!("\"{}\"", value.replace('"', "\"\""))
    } else {
        value.to_string()
    }
}

pub fn write_ndjson_report(writer: &mut dyn Write, results: &[StepResult]) -> anyhow::Result<()> {
    for r in results {
        let line = serde_json::to_string(&json!({
            "type": "step_result",
            "step_id": r.step_id,
            "step_type": r.step_type,
            "status": r.status,
            "duration_ms": r.duration_ms,
            "error": r.error,
            "assertion_result": r.assertion_result,
        }))?;
        writeln!(writer, "{line}")?;
    }
    Ok(())
}

pub fn print_summary(writer: &mut dyn Write, summary: &RunSummary) -> anyhow::Result<()> {
    writeln!(writer)?;
    writeln!(
        writer,
        "Test result: {} | {} passed, {} failed, {} skipped, {} errored | {}ms",
        if summary.completed && !summary.aborted && summary.failed == 0 && summary.errored == 0 {
            "PASSED"
        } else if summary.aborted {
            "ABORTED"
        } else {
            "FAILED"
        },
        summary.passed,
        summary.failed,
        summary.skipped,
        summary.errored,
        summary.duration_ms
    )?;
    Ok(())
}

pub fn write_json_summary(
    writer: &mut dyn Write,
    summary: &RunSummary,
    success: bool,
) -> anyhow::Result<()> {
    let value = if success {
        json!({
            "schema": JSON_SCHEMA,
            "ok": true,
            "command": "test",
            "summary": summary,
        })
    } else {
        let (code, message) = if summary.aborted {
            ("test_aborted", "test run was aborted")
        } else {
            ("test_failed", "one or more test steps failed")
        };
        json!({
            "schema": JSON_SCHEMA,
            "ok": false,
            "command": "test",
            "summary": summary,
            "error": {
                "code": code,
                "message": message,
            },
        })
    };
    serde_json::to_writer(&mut *writer, &value)?;
    writeln!(writer)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn sample_step() -> TestStep {
        TestStep {
            id: "boot,check".to_string(),
            step_type: StepType::Delay,
            params: serde_json::json!({"ms": 1}),
            assert: None,
            continue_on_error: None,
        }
    }

    fn sample_result() -> StepResult {
        StepResult {
            step_id: "boot,check".to_string(),
            step_type: StepType::Delay,
            status: StepStatus::Pass,
            started_at_ms: 1,
            finished_at_ms: 2,
            duration_ms: 1,
            error: None,
            assertion_result: None,
            adc_value_ua: None,
            serial_output: None,
        }
    }

    fn temp_report_path(extension: &str) -> std::path::PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        std::env::temp_dir().join(format!(
            "radxa-linkr-debugger-report-{}-{nonce}.{extension}",
            std::process::id()
        ))
    }

    #[test]
    fn csv_escape_quotes_special_fields() {
        assert_eq!(csv_escape("plain"), "plain");
        assert_eq!(csv_escape("a,b"), "\"a,b\"");
        assert_eq!(csv_escape("a\"b"), "\"a\"\"b\"");
    }

    #[test]
    fn report_file_format_follows_extension() {
        let step = sample_step();
        let result = sample_result();
        let summary = RunSummary {
            total: 1,
            passed: 1,
            failed: 0,
            skipped: 0,
            errored: 0,
            aborted: false,
            completed: true,
            duration_ms: 1,
        };

        for (extension, expected) in [
            ("json", "\"schema\": \"linkr-test-report.v1\""),
            ("csv", "\"boot,check\",pass,1"),
            ("ndjson", "\"type\":\"step_result\""),
        ] {
            let path = temp_report_path(extension);
            write_report_file(
                path.to_str().unwrap(),
                "smoke",
                std::slice::from_ref(&step),
                std::slice::from_ref(&result),
                &summary,
            )
            .unwrap();
            let contents = fs::read_to_string(&path).unwrap();
            fs::remove_file(path).unwrap();
            assert!(contents.contains(expected), "unexpected {extension} report");
        }
    }

    #[test]
    fn json_summary_uses_cli_envelope_for_pass_and_fail() {
        let passed = RunSummary {
            total: 1,
            passed: 1,
            failed: 0,
            skipped: 0,
            errored: 0,
            aborted: false,
            completed: true,
            duration_ms: 1,
        };
        let mut output = Vec::new();
        write_json_summary(&mut output, &passed, true).unwrap();
        let value: serde_json::Value = serde_json::from_slice(&output).unwrap();
        assert_eq!(value["schema"], JSON_SCHEMA);
        assert_eq!(value["ok"], true);
        assert_eq!(value["command"], "test");
        assert_eq!(value["summary"]["passed"], 1);

        output.clear();
        let failed = RunSummary {
            total: 1,
            passed: 0,
            failed: 1,
            skipped: 0,
            errored: 0,
            aborted: false,
            completed: false,
            duration_ms: 1,
        };
        write_json_summary(&mut output, &failed, false).unwrap();
        let value: serde_json::Value = serde_json::from_slice(&output).unwrap();
        assert_eq!(value["ok"], false);
        assert_eq!(value["error"]["code"], "test_failed");
    }
}
