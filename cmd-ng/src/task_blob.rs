use serde::Deserialize;
use serde_json::Value;
use std::collections::HashSet;
use std::time::Duration;

pub(crate) use crate::task_envelope::TaskList;
pub(crate) use crate::task_error::TaskError;

pub(crate) const TASK_MARKER_VERSION: &str = "# linkr-task.v1";
const TASK_MARKER_TASK: &str = "# task ";
pub(crate) const MAX_BLOB_SIZE: usize = 4000;
const MAX_TASKS: usize = 4;
const MAX_REQUESTS: usize = 32;
const MAX_TASK_ID_LEN: usize = 63;
const MAX_REQUEST_LINE: usize = 256;
const MAX_METHOD_LEN: usize = 8;
const MAX_PATH_LEN: usize = 96;
const MAX_BODY_LEN: usize = 192;
const MAX_WAIT_MS: u64 = 60_000;
const MAX_JSON_DEPTH: usize = 16;
const ALLOWED_PATH_PREFIXES: [&str; 3] = ["/api/v1/power/", "/api/v1/gpio/", "/api/v1/switch/"];

#[derive(Debug, Clone)]
pub(crate) struct TaskRecord {
    pub(crate) path: String,
    pub(crate) body: Value,
    pub(crate) wait: Duration,
}

#[derive(Debug)]
pub(crate) struct SavedTask {
    pub(crate) id: String,
    pub(crate) records: Vec<TaskRecord>,
    section_start: usize,
    digest_end: usize,
}

#[derive(Debug, Default)]
pub(crate) struct TaskBlob {
    tasks: Vec<SavedTask>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct RawTaskRecord {
    method: String,
    path: String,
    body: String,
    #[serde(default)]
    wait_ms: u64,
}

impl TaskBlob {
    pub(crate) fn parse(text: &str) -> Result<Self, TaskError> {
        if text.is_empty() {
            return Ok(Self::default());
        }
        if text.len() > MAX_BLOB_SIZE {
            return invalid_blob(format!("blob exceeds {MAX_BLOB_SIZE} bytes"));
        }
        let header = format!("{TASK_MARKER_VERSION}\n");
        if !text.starts_with(&header) {
            return invalid_blob("blob must start with the exact linkr-task.v1 marker");
        }
        if !text.ends_with('\n') {
            return invalid_blob("blob must end with LF");
        }
        if text
            .bytes()
            .any(|byte| (byte < 0x20 && byte != b'\n') || byte == 0x7f)
        {
            return invalid_blob("blob contains a forbidden control byte");
        }

        let mut tasks = Vec::new();
        let mut task_ids = HashSet::with_capacity(MAX_TASKS);
        let mut current: Option<SavedTask> = None;
        let mut offset = header.len();
        while offset < text.len() {
            let line_start = offset;
            let relative_end = text.as_bytes()[line_start..]
                .iter()
                .position(|byte| *byte == b'\n')
                .ok_or_else(|| invalid_blob_error("blob must end with LF"))?;
            let line_end = line_start + relative_end;
            let line = &text[line_start..line_end];
            offset = line_end + 1;
            if line.len() > MAX_REQUEST_LINE {
                return invalid_blob(format!("line exceeds {MAX_REQUEST_LINE} bytes"));
            }
            if line.is_empty() {
                continue;
            }
            if line == TASK_MARKER_VERSION {
                return invalid_blob("duplicate linkr-task.v1 marker");
            }
            if let Some(task_id) = line.strip_prefix(TASK_MARKER_TASK) {
                validate_task_id(task_id)?;
                if !task_ids.insert(task_id) {
                    return invalid_blob(format!("duplicate stored task id {task_id:?}"));
                }
                if let Some(task) = current.take() {
                    push_task(&mut tasks, task)?;
                }
                current = Some(SavedTask {
                    id: task_id.to_string(),
                    records: Vec::new(),
                    section_start: line_start,
                    digest_end: offset,
                });
                continue;
            }
            if line.starts_with('#') {
                continue;
            }
            let task = current
                .as_mut()
                .ok_or_else(|| invalid_blob_error("request appears before task marker"))?;
            if task.records.len() >= MAX_REQUESTS {
                return invalid_blob(format!("task exceeds {MAX_REQUESTS} requests"));
            }
            task.records.push(parse_record(line)?);
            task.digest_end = offset;
        }
        if let Some(task) = current {
            push_task(&mut tasks, task)?;
        }
        if tasks.is_empty() {
            return invalid_blob("missing linkr-task.v1 task data");
        }
        Ok(Self { tasks })
    }

    pub(crate) fn task(&self, task_id: &str) -> Result<&SavedTask, TaskError> {
        if self.tasks.is_empty() {
            return Err(TaskError::TaskStoreEmpty);
        }
        self.tasks
            .iter()
            .find(|task| task.id == task_id)
            .ok_or_else(|| TaskError::TaskNotFound {
                task_id: task_id.to_string(),
            })
    }

    pub(crate) fn tasks(&self) -> &[SavedTask] {
        &self.tasks
    }
}

pub(crate) fn task_ir_section(text: &str, task_id: &str) -> Result<String, TaskError> {
    let blob = TaskBlob::parse(text)?;
    let task = blob.task(task_id)?;
    let mut section = format!("{TASK_MARKER_VERSION}\n");
    section.push_str(&text[task.section_start..task.digest_end]);
    Ok(section)
}

fn parse_record(line: &str) -> Result<TaskRecord, TaskError> {
    let raw: RawTaskRecord = serde_json::from_str(line)
        .map_err(|error| invalid_blob_error(format!("invalid request record: {error}")))?;
    if raw.method.len() > MAX_METHOD_LEN || raw.method != "PUT" {
        return invalid_blob("stored requests must use method PUT");
    }
    if raw.path.len() > MAX_PATH_LEN || !valid_request_path(&raw.path) {
        return invalid_blob(format!("request path {:?} is not allowed", raw.path));
    }
    if raw.body.len() > MAX_BODY_LEN {
        return invalid_blob(format!("request body exceeds {MAX_BODY_LEN} bytes"));
    }
    if raw.wait_ms > MAX_WAIT_MS {
        return invalid_blob(format!("wait_ms must be 0..{MAX_WAIT_MS}"));
    }
    let body: Value = serde_json::from_str(&raw.body)
        .map_err(|error| invalid_blob_error(format!("request body is invalid JSON: {error}")))?;
    if json_depth_exceeds(&body, MAX_JSON_DEPTH) {
        return invalid_blob(format!(
            "request body exceeds {MAX_JSON_DEPTH} JSON nesting levels"
        ));
    }
    Ok(TaskRecord {
        path: raw.path,
        body,
        wait: Duration::from_millis(raw.wait_ms),
    })
}

fn valid_request_path(path: &str) -> bool {
    let identifier = ALLOWED_PATH_PREFIXES
        .iter()
        .find_map(|prefix| path.strip_prefix(prefix));
    let Some(identifier) = identifier else {
        return false;
    };
    !identifier.is_empty()
        && !matches!(identifier, "." | "..")
        && identifier.bytes().all(|byte| {
            (0x21..0x7f).contains(&byte) && !matches!(byte, b'/' | b'\\' | b'%' | b'?' | b'#')
        })
}

fn json_depth_exceeds(value: &Value, maximum: usize) -> bool {
    let mut pending = vec![(value, 0)];
    while let Some((current, depth)) = pending.pop() {
        match current {
            Value::Array(values) => {
                if depth >= maximum {
                    return true;
                }
                pending.extend(values.iter().map(|value| (value, depth + 1)));
            }
            Value::Object(values) => {
                if depth >= maximum {
                    return true;
                }
                pending.extend(values.values().map(|value| (value, depth + 1)));
            }
            Value::Null | Value::Bool(_) | Value::Number(_) | Value::String(_) => {}
        }
    }
    false
}

pub(crate) fn validate_task_id(task_id: &str) -> Result<(), TaskError> {
    let invalid_character = task_id
        .bytes()
        .any(|byte| byte <= 0x20 || byte == 0x7f || byte == b'#');
    if task_id.is_empty() || task_id.len() > MAX_TASK_ID_LEN || invalid_character {
        return invalid_blob(format!(
            "task id must be 1..{MAX_TASK_ID_LEN} bytes without whitespace or #"
        ));
    }
    Ok(())
}

fn push_task(tasks: &mut Vec<SavedTask>, task: SavedTask) -> Result<(), TaskError> {
    if tasks.len() >= MAX_TASKS {
        return invalid_blob(format!("blob exceeds {MAX_TASKS} tasks"));
    }
    tasks.push(task);
    Ok(())
}

pub(crate) fn build_task_blob(text: &str, task_id: &str) -> Result<String, TaskError> {
    if text.starts_with(TASK_MARKER_VERSION) {
        TaskBlob::parse(text)?;
        return Ok(text.to_string());
    }
    validate_task_id(task_id)?;
    let mut blob = format!("{TASK_MARKER_VERSION}\n{TASK_MARKER_TASK}{task_id}\n");
    blob.push_str(text);
    if !blob.ends_with('\n') {
        blob.push('\n');
    }
    TaskBlob::parse(&blob)?;
    Ok(blob)
}

fn invalid_blob<T>(message: impl Into<String>) -> Result<T, TaskError> {
    Err(invalid_blob_error(message))
}

fn invalid_blob_error(message: impl Into<String>) -> TaskError {
    TaskError::InvalidBlob {
        message: message.into(),
    }
}
