// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Test report generation for linkr-test-report.v1.
// Compatible with the WebUI test automation framework.

use serde::Serialize;
use serde_json::json;
use std::io::Write;
use std::time::Duration;

use crate::test_assertions::AssertionResult;
use crate::test_script::StepType;

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
    script_name: String,
    steps: Vec<StepResult>,
    start_time: std::time::Instant,
    aborted: bool,
}

impl ReportBuilder {
    pub fn new(script_name: String) -> Self {
        Self {
            script_name,
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

    pub fn summary(&self, total_steps: usize) -> RunSummary {
        let passed = self.steps.iter().filter(|r| r.status == StepStatus::Pass).count();
        let failed = self.steps.iter().filter(|r| r.status == StepStatus::Fail).count();
        let skipped = self.steps.iter().filter(|r| r.status == StepStatus::Skip).count();
        let errored = self.steps.iter().filter(|r| r.status == StepStatus::Error).count();
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
        summary.completed
            && !summary.aborted
            && summary.failed == 0
            && summary.errored == 0
    }
}

pub fn write_json_report(
    writer: &mut dyn Write,
    script_name: &str,
    script_steps: &[serde_json::Value],
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

pub fn write_csv_report(
    writer: &mut dyn Write,
    results: &[StepResult],
) -> anyhow::Result<()> {
    writeln!(writer, "step_id,status,duration_ms,error,detail,adc_ua")?;
    for r in results {
        let detail = r
            .assertion_result
            .as_ref()
            .map(|a| a.detail.as_str())
            .unwrap_or("");
        let error = r.error.as_deref().unwrap_or("");
        let adc = r.adc_value_ua.map(|v| format!("{:.0}", v)).unwrap_or_default();
        writeln!(
            writer,
            "{},{},{},{},{},{}",
            r.step_id,
            serde_json::to_string(&r.status).unwrap_or_default().trim_matches('"'),
            r.duration_ms,
            error,
            detail,
            adc
        )?;
    }
    Ok(())
}

pub fn write_ndjson_report(
    writer: &mut dyn Write,
    results: &[StepResult],
) -> anyhow::Result<()> {
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

pub fn print_summary(
    writer: &mut dyn Write,
    summary: &RunSummary,
) -> anyhow::Result<()> {
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

/// Compute nominal voltage from rail name prefix.
pub fn nominal_voltage(rail: &str) -> f64 {
    if rail.starts_with("20v_") {
        20.0
    } else if rail.starts_with("12v_") {
        12.0
    } else {
        5.0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nominal_voltage_from_rail() {
        assert_eq!(nominal_voltage("5v_out"), 5.0);
        assert_eq!(nominal_voltage("12v_out"), 12.0);
        assert_eq!(nominal_voltage("20v_out"), 20.0);
    }
}
