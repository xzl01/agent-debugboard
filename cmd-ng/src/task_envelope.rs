use crate::json_contract::JSON_SCHEMA;
use crate::task_blob::{validate_task_id, MAX_BLOB_SIZE};
use crate::task_catalog::FirmwareTaskCatalog;
use crate::task_error::TaskError;
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::HashSet;

pub(crate) const BUILTIN_TASK_ID_PREFIX: &str = "builtin/";
const MAX_TASKS: usize = 4;
const MAX_REQUESTS: usize = 32;
const MAX_SOURCE_BYTES: usize = 4000;

#[derive(Debug, Clone, Copy, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub(crate) enum TaskSourceState {
    Present,
    Absent,
}

impl TaskSourceState {
    pub(crate) const fn name(self) -> &'static str {
        match self {
            Self::Present => "present",
            Self::Absent => "absent",
        }
    }
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub(crate) struct TaskSummary {
    pub(crate) slot: u8,
    pub(crate) id: String,
    pub(crate) request_count: usize,
    pub(crate) source_state: TaskSourceState,
    pub(crate) source_bytes: usize,
    pub(crate) source_digest: Option<String>,
    pub(crate) ir_bytes: usize,
    pub(crate) task_ir_digest: String,
}

#[derive(Debug, Clone, Deserialize)]
pub(crate) struct TaskStorage {
    pub(crate) total_bytes: usize,
    pub(crate) used_bytes: usize,
    pub(crate) free_bytes: usize,
    pub(crate) task_bytes: usize,
    pub(crate) reserve_bytes: usize,
    pub(crate) measured: bool,
    pub(crate) save_allowed: bool,
}

#[derive(Debug, Deserialize)]
struct TaskListEnvelope {
    schema: String,
    ok: bool,
    command: String,
    action: String,
    task_store_id: String,
    revision: String,
    catalog_fingerprint: String,
    #[serde(default)]
    storage: Option<TaskStorage>,
    full_ir_bytes: usize,
    full_ir_digest: String,
    tasks: Vec<TaskSummary>,
}

#[derive(Debug, Clone)]
pub(crate) struct TaskList {
    pub(crate) raw_json: String,
    pub(crate) task_store_id: String,
    pub(crate) revision_text: String,
    pub(crate) catalog_fingerprint: String,
    pub(crate) storage: Option<TaskStorage>,
    pub(crate) full_ir_bytes: usize,
    pub(crate) full_ir_digest: String,
    pub(crate) tasks: Vec<TaskSummary>,
}

impl TaskList {
    pub(crate) fn from_raw(raw_json: String) -> Result<Self, TaskError> {
        let envelope: TaskListEnvelope =
            serde_json::from_str(&raw_json).map_err(|error| TaskError::InvalidResponse {
                message: error.to_string(),
            })?;
        if envelope.schema != JSON_SCHEMA
            || !envelope.ok
            || envelope.command != "tasks"
            || envelope.action != "list"
        {
            return invalid_response("expected successful tasks list envelope");
        }
        validate_store_id(&envelope.task_store_id)?;
        parse_revision(&envelope.revision)?;
        decode_sha256(&envelope.catalog_fingerprint)?;
        decode_sha256(&envelope.full_ir_digest)?;
        if envelope.full_ir_bytes > MAX_BLOB_SIZE || envelope.tasks.len() > MAX_TASKS {
            return invalid_response("task metadata exceeds firmware limits");
        }
        let mut slots = HashSet::with_capacity(envelope.tasks.len());
        let mut ids = HashSet::with_capacity(envelope.tasks.len());
        for task in &envelope.tasks {
            validate_summary(task)?;
            if !slots.insert(task.slot) || !ids.insert(task.id.as_str()) {
                return invalid_response("duplicate task slot or id in metadata");
            }
        }
        Ok(Self {
            raw_json,
            task_store_id: envelope.task_store_id,
            revision_text: envelope.revision,
            catalog_fingerprint: envelope.catalog_fingerprint,
            storage: envelope.storage,
            full_ir_bytes: envelope.full_ir_bytes,
            full_ir_digest: envelope.full_ir_digest,
            tasks: envelope.tasks,
        })
    }
}

pub(crate) fn parse_revision(value: &str) -> Result<u64, TaskError> {
    if value.is_empty()
        || (value.len() > 1 && value.starts_with('0'))
        || !value.bytes().all(|byte| byte.is_ascii_digit())
    {
        return invalid_response("revision must be a canonical uint64 decimal string");
    }
    value
        .parse::<u64>()
        .map_err(|_| TaskError::InvalidResponse {
            message: "revision exceeds uint64".to_string(),
        })
}

pub(crate) fn decode_sha256(value: &str) -> Result<[u8; 32], TaskError> {
    if value.len() != 64
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    {
        return invalid_response("digest must be 64 lowercase hexadecimal characters");
    }
    let mut digest = [0_u8; 32];
    for (index, pair) in value.as_bytes().as_chunks::<2>().0.iter().enumerate() {
        digest[index] = (hex_nibble(pair[0]) << 4) | hex_nibble(pair[1]);
    }
    Ok(digest)
}

fn hex_nibble(byte: u8) -> u8 {
    match byte {
        b'0'..=b'9' => byte - b'0',
        b'a'..=b'f' => byte - b'a' + 10,
        _ => unreachable!("validated lowercase hexadecimal digit"),
    }
}

pub(crate) fn validate_store_id(value: &str) -> Result<(), TaskError> {
    let parsed = uuid::Uuid::parse_str(value).map_err(|_| TaskError::InvalidResponse {
        message: "task_store_id must be a canonical UUID".to_string(),
    })?;
    if parsed.hyphenated().to_string() != value {
        return invalid_response("task_store_id must be a lowercase canonical UUID");
    }
    Ok(())
}

pub(crate) fn validate_summary(task: &TaskSummary) -> Result<(), TaskError> {
    if task.slot >= MAX_TASKS as u8
        || task.request_count > MAX_REQUESTS
        || task.ir_bytes > MAX_BLOB_SIZE
        || validate_task_id(&task.id).is_err()
    {
        return invalid_response("invalid task summary limits or identity");
    }
    decode_sha256(&task.task_ir_digest)?;
    match task.source_state {
        TaskSourceState::Present => {
            if task.source_bytes == 0 || task.source_bytes > MAX_SOURCE_BYTES {
                return invalid_response("present task source has invalid length");
            }
            let digest =
                task.source_digest
                    .as_deref()
                    .ok_or_else(|| TaskError::InvalidResponse {
                        message: "present task source is missing its digest".to_string(),
                    })?;
            decode_sha256(digest)?;
        }
        TaskSourceState::Absent => {
            if task.source_bytes != 0 || task.source_digest.is_some() {
                return invalid_response("IR-only task claims source bytes or digest");
            }
        }
    }
    Ok(())
}

fn invalid_response<T>(message: impl Into<String>) -> Result<T, TaskError> {
    Err(TaskError::InvalidResponse {
        message: message.into(),
    })
}

#[derive(Clone, Copy, Serialize)]
#[serde(rename_all = "lowercase")]
pub(crate) enum TaskSource {
    Builtin,
    Stored,
}

impl TaskSource {
    pub(crate) const fn name(self) -> &'static str {
        match self {
            Self::Builtin => "builtin",
            Self::Stored => "stored",
        }
    }
}

#[derive(Serialize)]
pub(crate) struct CatalogTask {
    pub(crate) id: String,
    pub(crate) name: String,
    pub(crate) request_count: usize,
    pub(crate) source: TaskSource,
    pub(crate) shadowed_stored: bool,
    pub(crate) lossless_source: &'static str,
    pub(crate) slot: Option<u8>,
    pub(crate) source_bytes: Option<usize>,
    pub(crate) source_digest: Option<String>,
    pub(crate) ir_bytes: Option<usize>,
    pub(crate) task_ir_digest: Option<String>,
}

pub(crate) fn merged_task_catalog(
    list: &TaskList,
    firmware_catalog: Option<&FirmwareTaskCatalog>,
) -> Vec<CatalogTask> {
    let stored_ids = list
        .tasks
        .iter()
        .map(|task| task.id.as_str())
        .collect::<HashSet<_>>();
    let built_ins = firmware_catalog
        .map(FirmwareTaskCatalog::tasks)
        .unwrap_or_default();
    let built_in_ids = built_ins
        .iter()
        .map(|task| task.id.as_str())
        .collect::<HashSet<_>>();
    let mut catalog = built_ins
        .iter()
        .map(|task| CatalogTask {
            id: task.id.clone(),
            name: task.name.clone(),
            request_count: task.records.len(),
            source: TaskSource::Builtin,
            shadowed_stored: stored_ids.contains(task.id.as_str()),
            lossless_source: "unavailable",
            slot: None,
            source_bytes: None,
            source_digest: None,
            ir_bytes: None,
            task_ir_digest: None,
        })
        .collect::<Vec<_>>();
    catalog.extend(
        list.tasks
            .iter()
            .filter(|task| {
                !task.id.starts_with(BUILTIN_TASK_ID_PREFIX)
                    && !built_in_ids.contains(task.id.as_str())
            })
            .map(|task| CatalogTask {
                id: task.id.clone(),
                name: task.id.clone(),
                request_count: task.request_count,
                source: TaskSource::Stored,
                shadowed_stored: false,
                lossless_source: task.source_state.name(),
                slot: Some(task.slot),
                source_bytes: Some(task.source_bytes),
                source_digest: task.source_digest.clone(),
                ir_bytes: Some(task.ir_bytes),
                task_ir_digest: Some(task.task_ir_digest.clone()),
            }),
    );
    catalog
}

pub(crate) fn catalog_json(
    list: &TaskList,
    catalog: &[CatalogTask],
    catalog_error: Option<&TaskError>,
) -> Result<String, TaskError> {
    let mut value: Value =
        serde_json::from_str(&list.raw_json).map_err(|error| TaskError::InvalidResponse {
            message: error.to_string(),
        })?;
    let object = value
        .as_object_mut()
        .ok_or_else(|| TaskError::InvalidResponse {
            message: "task list response is not an object".to_string(),
        })?;
    object.insert(
        "tasks".to_string(),
        serde_json::to_value(catalog).map_err(|error| TaskError::InvalidResponse {
            message: error.to_string(),
        })?,
    );
    object.insert("task_count".to_string(), Value::from(catalog.len()));
    object.insert(
        "stored_task_count".to_string(),
        Value::from(list.tasks.len()),
    );
    object.insert(
        "catalog_available".to_string(),
        Value::Bool(catalog_error.is_none()),
    );
    if let Some(error) = catalog_error {
        object.insert(
            "catalog_error".to_string(),
            serde_json::json!({"code": error.code(), "message": error.to_string()}),
        );
    }
    Ok(value.to_string())
}
