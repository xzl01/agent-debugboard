use crate::client::{BoardRawJsonRequest, BoardRequest, BoardTransport};
use crate::json_contract::JSON_SCHEMA;
use crate::task_blob::build_task_blob;
use crate::task_error::TaskError;
use crate::task_output::TaskCommandIo;
use anyhow::Result;
use reqwest::Method;
use serde::Deserialize;
use std::fs;
use std::path::Path;

#[derive(Debug)]
pub(crate) struct StoreArgs {
    pub(crate) path: String,
    pub(crate) task_id: Option<String>,
}

pub(crate) struct TaskStore<'a> {
    client: &'a dyn BoardTransport,
}

impl<'a> TaskStore<'a> {
    pub(crate) const fn new(client: &'a dyn BoardTransport) -> Self {
        Self { client }
    }

    pub(crate) fn run_store(&self, args: &StoreArgs, io: &mut TaskCommandIo<'_>) -> Result<u8> {
        let text = match fs::read_to_string(&args.path) {
            Ok(text) => text,
            Err(error) => {
                let error = TaskError::Command {
                    code: "read_failed",
                    message: format!("cannot read {:?}: {error}", args.path),
                };
                return io.failure("store", &error, 2);
            }
        };
        let task_id = match args.task_id.as_deref() {
            Some(task_id) => task_id.to_string(),
            None => task_id_from_path(&args.path),
        };
        let blob = match build_task_blob(&text, &task_id) {
            Ok(blob) => blob,
            Err(error) => return io.failure("store", &error, 2),
        };
        self.run_operation(
            Operation {
                action: "store",
                success: format!("task store ok task={task_id}"),
                request: OperationRequest::RawJson(BoardRawJsonRequest {
                    method: Method::PUT,
                    path: "/api/v1/tasks".to_string(),
                    query: Vec::new(),
                    body: blob,
                }),
            },
            io,
        )
    }

    pub(crate) fn run_clear(&self, io: &mut TaskCommandIo<'_>) -> Result<u8> {
        self.run_operation(
            Operation {
                action: "clear",
                success: "task clear ok".to_string(),
                request: OperationRequest::Structured(BoardRequest {
                    method: Method::DELETE,
                    path: "/api/v1/tasks".to_string(),
                    query: Vec::new(),
                    body: None,
                }),
            },
            io,
        )
    }

    fn run_operation(&self, operation: Operation, io: &mut TaskCommandIo<'_>) -> Result<u8> {
        let output = match operation.request {
            OperationRequest::Structured(request) => self.client.send_text(request),
            OperationRequest::RawJson(request) => self.client.send_raw_json(request),
        };
        let output = match output {
            Ok(output) => output,
            Err(error) => {
                let error = TaskError::Command {
                    code: "operation_failed",
                    message: error.to_string(),
                };
                return io.failure(operation.action, &error, 1);
            }
        };
        let envelope: OperationEnvelope = match serde_json::from_str(&output) {
            Ok(envelope) => envelope,
            Err(error) => {
                let error = TaskError::InvalidResponse {
                    message: error.to_string(),
                };
                return io.failure(operation.action, &error, 1);
            }
        };
        if envelope.schema != JSON_SCHEMA
            || envelope.command != "task"
            || envelope.action != operation.action
        {
            let error = TaskError::InvalidResponse {
                message: format!("expected task {} envelope", operation.action),
            };
            return io.failure(operation.action, &error, 1);
        }
        if io.json_output {
            writeln!(io.stdout, "{}", output.trim())?;
        } else if envelope.ok {
            writeln!(io.stdout, "{}", operation.success)?;
        } else {
            writeln!(io.stderr, "{}", output.trim())?;
        }
        Ok(if envelope.ok { 0 } else { 1 })
    }
}

struct Operation {
    action: &'static str,
    success: String,
    request: OperationRequest,
}

enum OperationRequest {
    Structured(BoardRequest),
    RawJson(BoardRawJsonRequest),
}

#[derive(Deserialize)]
struct OperationEnvelope {
    schema: String,
    ok: bool,
    command: String,
    action: String,
}

fn task_id_from_path(path: &str) -> String {
    let stem = Path::new(path)
        .file_stem()
        .and_then(|stem| stem.to_str())
        .unwrap_or("task");
    stem.chars()
        .map(|character| {
            if character.is_ascii_alphanumeric() || matches!(character, '-' | '_') {
                character
            } else {
                '-'
            }
        })
        .collect()
}
