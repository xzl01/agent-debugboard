use crate::task_blob::{TaskBlob, TaskError, TaskRecord, TASK_MARKER_VERSION};
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};
use std::collections::HashSet;

const CATALOG_VERSION: u8 = 1;
const MAX_CATALOG_TASKS: usize = 6;
const MAX_CATALOG_RESPONSE_LEN: usize = 9_456;
const MAX_TASK_NAME_LEN: usize = 63;

#[derive(Debug)]
pub(crate) struct FirmwareTask {
    pub(crate) id: String,
    pub(crate) name: String,
    pub(crate) records: Vec<TaskRecord>,
    pub(crate) cleanup: TaskRecord,
}

#[derive(Debug)]
pub(crate) struct FirmwareTaskCatalog {
    tasks: Vec<FirmwareTask>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct CatalogEnvelope {
    schema: String,
    ok: bool,
    command: String,
    action: String,
    version: u8,
    tasks: Vec<CatalogTaskWire>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct CatalogTaskWire {
    id: String,
    name: String,
    requests: Vec<CatalogRequestWire>,
    cleanup: CatalogRequestWire,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct CatalogRequestWire {
    method: String,
    path: String,
    body: Map<String, Value>,
    wait_ms: u64,
}

#[derive(Serialize)]
struct BlobRequest<'a> {
    method: &'a str,
    path: &'a str,
    body: &'a str,
    wait_ms: u64,
}

impl FirmwareTaskCatalog {
    pub(crate) fn from_raw(raw_json: String) -> Result<Self, TaskError> {
        if raw_json.len() > MAX_CATALOG_RESPONSE_LEN {
            return invalid_catalog(format!("catalog exceeds {MAX_CATALOG_RESPONSE_LEN} bytes"));
        }
        let envelope: CatalogEnvelope = serde_json::from_str(&raw_json)
            .map_err(|error| catalog_error(format!("invalid catalog JSON: {error}")))?;
        if envelope.schema != "radxa-linkr-debugger.v1"
            || !envelope.ok
            || envelope.command != "task"
            || envelope.action != "catalog"
            || envelope.version != CATALOG_VERSION
        {
            return invalid_catalog("expected successful task catalog v1 envelope");
        }
        if envelope.tasks.is_empty() || envelope.tasks.len() > MAX_CATALOG_TASKS {
            return invalid_catalog(format!("catalog task count must be 1..{MAX_CATALOG_TASKS}"));
        }

        let mut ids = HashSet::new();
        let mut tasks = Vec::with_capacity(envelope.tasks.len());
        for task in envelope.tasks {
            if !ids.insert(task.id.clone()) {
                return invalid_catalog(format!("duplicate catalog task id {:?}", task.id));
            }
            tasks.push(normalize_task(task)?);
        }
        Ok(Self { tasks })
    }

    pub(crate) fn tasks(&self) -> &[FirmwareTask] {
        &self.tasks
    }

    pub(crate) fn task(&self, task_id: &str) -> Option<&FirmwareTask> {
        self.tasks.iter().find(|task| task.id == task_id)
    }
}

fn normalize_task(task: CatalogTaskWire) -> Result<FirmwareTask, TaskError> {
    if task.name.is_empty() || task.name.len() > MAX_TASK_NAME_LEN {
        return invalid_catalog(format!(
            "catalog task name must be 1..{MAX_TASK_NAME_LEN} bytes"
        ));
    }
    if task.requests.is_empty() {
        return invalid_catalog("catalog task must contain at least one request");
    }
    let records = task
        .requests
        .into_iter()
        .map(canonical_record)
        .collect::<Result<Vec<_>, _>>()?;
    let cleanup = canonical_record(task.cleanup)?;
    Ok(FirmwareTask {
        id: task.id.clone(),
        name: task.name,
        records: normalized_records(&task.id, &records)?,
        cleanup: normalized_records(&task.id, &[cleanup])?
            .into_iter()
            .next()
            .ok_or_else(|| catalog_error("catalog cleanup normalization produced no record"))?,
    })
}

fn canonical_record(record: CatalogRequestWire) -> Result<String, TaskError> {
    let body = serde_json::to_string(&record.body)
        .map_err(|error| catalog_error(format!("cannot canonicalize request body: {error}")))?;
    serde_json::to_string(&BlobRequest {
        method: &record.method,
        path: &record.path,
        body: &body,
        wait_ms: record.wait_ms,
    })
    .map_err(|error| catalog_error(format!("cannot canonicalize catalog request: {error}")))
}

fn normalized_records(task_id: &str, records: &[String]) -> Result<Vec<TaskRecord>, TaskError> {
    let blob = format!(
        "{TASK_MARKER_VERSION}\n# task {task_id}\n{}\n",
        records.join("\n")
    );
    let blob = TaskBlob::parse(&blob).map_err(|error| catalog_error(error.to_string()))?;
    Ok(blob
        .task(task_id)
        .map_err(|error| catalog_error(error.to_string()))?
        .records
        .clone())
}

fn invalid_catalog<T>(message: impl Into<String>) -> Result<T, TaskError> {
    Err(catalog_error(message))
}

fn catalog_error(message: impl Into<String>) -> TaskError {
    TaskError::Command {
        code: "catalog_invalid",
        message: message.into(),
    }
}
