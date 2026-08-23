use crate::client::{BoardBinaryUpload, BoardRawJsonRequest, BoardRequest, BoardTransport};
use crate::task_command::{TaskCommandIo, TaskRunner};
use crate::task_execution::{TaskCancellation, TaskSleeper};
use anyhow::{anyhow, Result};
use serde_json::{json, Value};
use std::cell::RefCell;
use std::collections::VecDeque;
use std::rc::Rc;
use std::time::Duration;

pub(crate) enum FakeReply {
    Output(String),
    Failure(String),
}

pub(crate) struct FakeTransport {
    replies: RefCell<VecDeque<FakeReply>>,
    pub(crate) requests: RefCell<Vec<BoardRequest>>,
    pub(crate) raw_json_requests: RefCell<Vec<BoardRawJsonRequest>>,
    pub(crate) events: Rc<RefCell<Vec<String>>>,
}

impl FakeTransport {
    pub(crate) fn new(replies: Vec<FakeReply>, events: Rc<RefCell<Vec<String>>>) -> Self {
        Self {
            replies: RefCell::new(replies.into()),
            requests: RefCell::new(Vec::new()),
            raw_json_requests: RefCell::new(Vec::new()),
            events,
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
            None => Err(anyhow!("unexpected task transport request")),
        }
    }

    fn send_raw_json(&self, request: BoardRawJsonRequest) -> Result<String> {
        self.events
            .borrow_mut()
            .push(format!("send:{} {}", request.method, request.path));
        self.raw_json_requests.borrow_mut().push(request);
        match self.replies.borrow_mut().pop_front() {
            Some(FakeReply::Output(output)) => Ok(output),
            Some(FakeReply::Failure(message)) => Err(anyhow!(message)),
            None => Err(anyhow!("unexpected task raw JSON request")),
        }
    }

    fn upload_binary(&self, _request: BoardBinaryUpload) -> Result<String> {
        Err(anyhow!("unexpected task binary upload"))
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
        vec![json!({"id": task_id, "name": task_id, "request_count": 1})]
    };
    json!({
        "schema": "radxa-linkr-debugger.v1",
        "ok": true,
        "command": "task",
        "action": "list",
        "tasks": tasks,
        "blob": blob,
    })
    .to_string()
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
