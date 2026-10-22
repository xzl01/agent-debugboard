use crate::client::{
    BoardBinaryUpload, BoardBytesRequest, BoardBytesResponse, BoardRequest, BoardTransport,
};
use crate::task_api::{sha256_hex, TASK_IR_CONTENT_TYPE, TASK_SOURCE_CONTENT_TYPE};
use crate::task_blob::{task_ir_section, TaskBlob};
use crate::task_command::{TaskCommandIo, TaskRunner};
use crate::task_execution::{TaskCancellation, TaskSleeper};
use anyhow::{anyhow, Result};
use reqwest::StatusCode;
use serde_json::{json, Value};
use std::cell::RefCell;
use std::collections::VecDeque;
use std::rc::Rc;
use std::time::Duration;

pub(crate) enum FakeReply {
    Output(String),
    Failure(String),
    TaskList {
        metadata: String,
        ir: Vec<u8>,
    },
    Bytes {
        status: StatusCode,
        content_type: &'static str,
        body: Vec<u8>,
    },
}

pub(crate) struct FakeTransport {
    replies: RefCell<VecDeque<FakeReply>>,
    pub(crate) requests: RefCell<Vec<BoardRequest>>,
    pub(crate) byte_requests: RefCell<Vec<BoardBytesRequest>>,
    pub(crate) events: Rc<RefCell<Vec<String>>>,
    pending_ir: RefCell<Option<Vec<u8>>>,
}

impl FakeTransport {
    pub(crate) fn new(replies: Vec<FakeReply>, events: Rc<RefCell<Vec<String>>>) -> Self {
        Self {
            replies: RefCell::new(replies.into()),
            requests: RefCell::new(Vec::new()),
            byte_requests: RefCell::new(Vec::new()),
            events,
            pending_ir: RefCell::new(None),
        }
    }
}

impl BoardTransport for FakeTransport {
    fn send_text(&self, request: BoardRequest) -> Result<String> {
        self.events
            .borrow_mut()
            .push(format!("send:{} {}", request.method, request.path));
        self.requests.borrow_mut().push(request);
        match self.replies.borrow_mut().pop_front() {
            Some(FakeReply::Output(output)) => Ok(output),
            Some(FakeReply::Failure(message)) => Err(anyhow!(message)),
            Some(FakeReply::TaskList { .. } | FakeReply::Bytes { .. }) => {
                Err(anyhow!("unexpected task reply kind for text request"))
            }
            None => Err(anyhow!("unexpected task transport request")),
        }
    }

    fn upload_binary(&self, _request: BoardBinaryUpload) -> Result<String> {
        Err(anyhow!("unexpected task binary upload"))
    }

    fn send_bytes(&self, request: BoardBytesRequest) -> Result<BoardBytesResponse> {
        self.events
            .borrow_mut()
            .push(format!("send:{} {}", request.method, request.path));
        self.byte_requests.borrow_mut().push(request.clone());
        if request.path == "/api/v1/tasks/ir" {
            if let Some(body) = self.pending_ir.borrow_mut().take() {
                return Ok(BoardBytesResponse {
                    status: StatusCode::OK,
                    content_type: Some(TASK_IR_CONTENT_TYPE.to_string()),
                    body,
                });
            }
        }
        match self.replies.borrow_mut().pop_front() {
            Some(FakeReply::Output(output)) => Ok(BoardBytesResponse {
                status: StatusCode::OK,
                content_type: Some(inferred_content_type(&request.path).to_string()),
                body: output.into_bytes(),
            }),
            Some(FakeReply::Failure(message)) => Err(anyhow!(message)),
            Some(FakeReply::TaskList { metadata, ir }) => {
                *self.pending_ir.borrow_mut() = Some(ir);
                Ok(BoardBytesResponse {
                    status: StatusCode::OK,
                    content_type: Some("application/json".to_string()),
                    body: metadata.into_bytes(),
                })
            }
            Some(FakeReply::Bytes {
                status,
                content_type,
                body,
            }) => Ok(BoardBytesResponse {
                status,
                content_type: Some(content_type.to_string()),
                body,
            }),
            None => Err(anyhow!("unexpected task byte request")),
        }
    }

    fn base_url(&self) -> &str {
        "http://task.test"
    }
}

pub(crate) struct RecordingSleeper {
    pub(crate) events: Rc<RefCell<Vec<String>>>,
}

impl TaskSleeper for RecordingSleeper {
    fn sleep(&self, delay: Duration, cancellation: &dyn TaskCancellation) -> bool {
        self.events
            .borrow_mut()
            .push(format!("sleep:{}", delay.as_millis()));
        cancellation.is_cancelled()
    }
}

pub(crate) fn record(path: &str, body: Value, wait_ms: u64) -> String {
    json!({
        "method": "PUT",
        "path": path,
        "body": body.to_string(),
        "wait_ms": wait_ms,
    })
    .to_string()
}

pub(crate) fn blob(task_id: &str, records: &[String]) -> String {
    format!(
        "# linkr-task.v1\n# task {task_id}\n{}\n",
        records.join("\n")
    )
}

pub(crate) fn list_output(task_id: &str, blob: &str) -> String {
    let tasks = if task_id.is_empty() {
        Vec::new()
    } else {
        let parsed = TaskBlob::parse(blob).ok();
        let task = parsed.as_ref().and_then(|blob| blob.task(task_id).ok());
        let section = task_ir_section(blob, task_id).ok();
        vec![json!({
            "slot": 0,
            "id": task_id,
            "request_count": task.map_or(1, |task| task.records.len()),
            "source_state": "absent",
            "source_bytes": 0,
            "source_digest": null,
            "ir_bytes": section.as_ref().map_or(blob.len(), String::len),
            "task_ir_digest": section.as_ref().map_or_else(
                || "0".repeat(64),
                |section| sha256_hex(section.as_bytes()),
            ),
        })]
    };
    json!({
        "schema": "radxa-linkr-debugger.v1",
        "ok": true,
        "command": "tasks",
        "action": "list",
        "task_store_id": "12345678-1234-4567-890a-123456789abc",
        "revision": "9007199254740993",
        "catalog_fingerprint": "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
        "full_ir_bytes": blob.len(),
        "full_ir_digest": sha256_hex(blob.as_bytes()),
        "tasks": tasks,
    })
    .to_string()
}

pub(crate) fn list_reply(task_id: &str, blob: &str) -> FakeReply {
    FakeReply::TaskList {
        metadata: list_output(task_id, blob),
        ir: blob.as_bytes().to_vec(),
    }
}

fn inferred_content_type(path: &str) -> &'static str {
    if path.contains("/slots/") && path.ends_with("/source") {
        TASK_SOURCE_CONTENT_TYPE
    } else if path.ends_with("/ir") {
        TASK_IR_CONTENT_TYPE
    } else {
        "application/json"
    }
}

pub(crate) fn run_task(
    transport: &FakeTransport,
    sleeper: &RecordingSleeper,
    task_id: &str,
) -> Result<(u8, String, String)> {
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();
    let arguments = vec![
        "task".to_string(),
        "run".to_string(),
        task_id.to_string(),
        "--confirm".to_string(),
    ];
    let code = TaskRunner::new(transport, sleeper).run(
        &arguments,
        TaskCommandIo::new(true, &mut stdout, &mut stderr),
    )?;
    Ok((code, String::from_utf8(stdout)?, String::from_utf8(stderr)?))
}
