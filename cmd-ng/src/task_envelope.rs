use crate::json_contract::JSON_SCHEMA;
use crate::task_catalog::FirmwareTaskCatalog;
use crate::task_error::TaskError;
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::HashSet;

pub(crate) const BUILTIN_TASK_ID_PREFIX: &str = "builtin/";

#[derive(Debug, Clone, Deserialize)]
pub(crate) struct TaskSummary {
    pub(crate) id: String,
    pub(crate) name: String,
    pub(crate) request_count: usize,
}

#[derive(Debug, Deserialize)]
struct TaskListEnvelope {
    schema: String,
    ok: bool,
    command: String,
    action: String,
    tasks: Vec<TaskSummary>,
    blob: String,
}

#[derive(Debug)]
pub(crate) struct TaskList {
    pub(crate) raw_json: String,
    pub(crate) tasks: Vec<TaskSummary>,
    pub(crate) blob: String,
}

impl TaskList {
    pub(crate) fn from_raw(raw_json: String) -> Result<Self, TaskError> {
        let envelope: TaskListEnvelope =
            serde_json::from_str(&raw_json).map_err(|error| TaskError::InvalidResponse {
                message: error.to_string(),
            })?;
        if envelope.schema != JSON_SCHEMA
            || !envelope.ok
            || envelope.command != "task"
            || envelope.action != "list"
        {
            return Err(TaskError::InvalidResponse {
                message: "expected successful task list envelope".to_string(),
            });
        }
        Ok(Self {
            raw_json,
            tasks: envelope.tasks,
            blob: envelope.blob,
        })
    }
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
                name: task.name.clone(),
                request_count: task.request_count,
                source: TaskSource::Stored,
                shadowed_stored: false,
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
