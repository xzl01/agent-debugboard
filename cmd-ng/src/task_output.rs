use crate::json_contract::JSON_SCHEMA;
use crate::task_error::TaskError;
use crate::task_execution::TaskCleanupOutcome;
use anyhow::Result;
use serde::Serialize;
use std::io::Write;

pub(crate) struct TaskCommandIo<'a> {
    pub(crate) json_output: bool,
    pub(crate) stdout: &'a mut dyn Write,
    pub(crate) stderr: &'a mut dyn Write,
}

impl<'a> TaskCommandIo<'a> {
    pub(crate) fn new(
        json_output: bool,
        stdout: &'a mut dyn Write,
        stderr: &'a mut dyn Write,
    ) -> Self {
        Self {
            json_output,
            stdout,
            stderr,
        }
    }

    pub(crate) fn failure(&mut self, action: &str, error: &TaskError, exit_code: u8) -> Result<u8> {
        self.execution_failure(action, error, None, exit_code)
    }

    pub(crate) fn execution_failure(
        &mut self,
        action: &str,
        error: &TaskError,
        cleanup: Option<&TaskCleanupOutcome>,
        exit_code: u8,
    ) -> Result<u8> {
        if self.json_output {
            let record = error.record();
            let failure = TaskFailure {
                schema: JSON_SCHEMA,
                ok: false,
                command: "task",
                action,
                error: TaskFailureDetail {
                    code: error.code(),
                    message: error.to_string(),
                    record_index: record.map(|(index, _)| index),
                    path: record.map(|(_, path)| path),
                    requests_completed: error.requests_completed(),
                },
                cleanup,
            };
            writeln!(self.stdout, "{}", serde_json::to_string(&failure)?)?;
        } else {
            writeln!(self.stderr, "{error}")?;
            if let Some(outcome) = cleanup {
                match outcome.error.as_deref() {
                    Some(cleanup_error) => {
                        writeln!(self.stderr, "cleanup failed: {cleanup_error}")?;
                    }
                    None => writeln!(self.stderr, "cleanup completed")?,
                }
            }
        }
        Ok(exit_code)
    }
}

#[derive(Serialize)]
struct TaskFailure<'a> {
    schema: &'static str,
    ok: bool,
    command: &'static str,
    action: &'a str,
    error: TaskFailureDetail<'a>,
    #[serde(skip_serializing_if = "Option::is_none")]
    cleanup: Option<&'a TaskCleanupOutcome>,
}

#[derive(Serialize)]
struct TaskFailureDetail<'a> {
    code: &'static str,
    message: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    record_index: Option<usize>,
    #[serde(skip_serializing_if = "Option::is_none")]
    path: Option<&'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    requests_completed: Option<usize>,
}
