use crate::client::{BoardBytesRequest, BoardBytesResponse, BoardTransport};
use crate::json_contract::JSON_SCHEMA;
use crate::task_blob::{task_ir_section, validate_task_id, TaskBlob, MAX_BLOB_SIZE};
use crate::task_envelope::{
    decode_sha256, parse_revision, validate_store_id, validate_summary, TaskList, TaskSourceState,
    TaskSummary,
};
use crate::task_error::TaskError;
use reqwest::{Method, StatusCode};
use serde::Deserialize;
use sha2::{Digest, Sha256};

pub(crate) const TASK_MUTATION_CONTENT_TYPE: &str = "application/json";
pub(crate) const TASK_IR_CONTENT_TYPE: &str = "application/vnd.radxa.linkr-task-ir.v1";
pub(crate) const TASK_SOURCE_CONTENT_TYPE: &str = "application/vnd.radxa.linkr-test.v1+ndjson";
const JSON_CONTENT_TYPE: &str = "application/json";
pub(crate) const TASK_MUTATION_MAX_BYTES: usize = 12288;
const SOURCE_MAX_BYTES: usize = 4000;

#[derive(Debug)]
pub(crate) struct TaskSnapshot {
    pub(crate) metadata: TaskList,
    pub(crate) ir: String,
}

#[derive(Debug)]
pub(crate) struct TaskSourceSnapshot {
    pub(crate) metadata: TaskList,
    pub(crate) task: TaskSummary,
    pub(crate) source: Vec<u8>,
}

#[derive(Clone, Copy)]
pub(crate) enum TaskMutation<'a> {
    Upsert {
        id: &'a str,
        source: Option<&'a [u8]>,
        ir: &'a str,
    },
    Delete {
        id: &'a str,
    },
    Clear,
    Import {
        ir: &'a str,
    },
}

impl TaskMutation<'_> {
    pub(crate) const fn action(self) -> &'static str {
        match self {
            Self::Upsert { .. } => "upsert",
            Self::Delete { .. } => "delete",
            Self::Clear => "clear",
            Self::Import { .. } => "import",
        }
    }
}

#[derive(Debug)]
pub(crate) struct TaskMutationResult {
    pub(crate) raw_json: String,
    pub(crate) task_store_id: String,
    pub(crate) revision_text: String,
    pub(crate) catalog_fingerprint: String,
    pub(crate) task: Option<TaskSummary>,
}

pub(crate) struct TaskApi<'a> {
    client: &'a dyn BoardTransport,
}

impl<'a> TaskApi<'a> {
    pub(crate) const fn new(client: &'a dyn BoardTransport) -> Self {
        Self { client }
    }

    pub(crate) fn metadata(&self) -> Result<TaskList, TaskError> {
        let response = self
            .client
            .send_bytes(bytes_request(
                Method::GET,
                "/api/v1/tasks",
                JSON_CONTENT_TYPE,
                None,
                Vec::new(),
            ))
            .map_err(|error| TaskError::Retrieval {
                message: error.to_string(),
            })?;
        let body = successful_body(response, JSON_CONTENT_TYPE)?;
        let text = String::from_utf8(body).map_err(|error| TaskError::InvalidResponse {
            message: format!("task metadata is not UTF-8: {error}"),
        })?;
        TaskList::from_raw(text)
    }

    pub(crate) fn snapshot(&self) -> Result<TaskSnapshot, TaskError> {
        for _ in 0..2 {
            let metadata = self.metadata()?;
            let response = self
                .client
                .send_bytes(bytes_request(
                    Method::GET,
                    "/api/v1/tasks/ir",
                    TASK_IR_CONTENT_TYPE,
                    None,
                    Vec::new(),
                ))
                .map_err(|error| TaskError::Retrieval {
                    message: error.to_string(),
                })?;
            let body = successful_body(response, TASK_IR_CONTENT_TYPE)?;
            if body.len() != metadata.full_ir_bytes || sha256_hex(&body) != metadata.full_ir_digest
            {
                continue;
            }
            let ir = String::from_utf8(body).map_err(|error| TaskError::InvalidResponse {
                message: format!("task IR is not UTF-8: {error}"),
            })?;
            verify_snapshot(&metadata, &ir)?;
            return Ok(TaskSnapshot { metadata, ir });
        }
        Err(TaskError::Command {
            code: "digest_mismatch",
            message: "task IR changed or failed digest verification during both reads".to_string(),
        })
    }

    pub(crate) fn source(&self, task_id: &str) -> Result<TaskSourceSnapshot, TaskError> {
        for attempt in 0..2 {
            let metadata = self.metadata()?;
            let task = metadata
                .tasks
                .iter()
                .find(|task| task.id == task_id)
                .cloned()
                .ok_or_else(|| TaskError::TaskNotFound {
                    task_id: task_id.to_string(),
                })?;
            if task.source_state == TaskSourceState::Absent {
                return Err(TaskError::Command {
                    code: "task_source_absent",
                    message: format!(
                        "task {task_id:?} is IR-only and has no lossless workflow source"
                    ),
                });
            }
            let path = format!("/api/v1/tasks/slots/{}/source", task.slot);
            let response = self
                .client
                .send_bytes(bytes_request(
                    Method::GET,
                    &path,
                    TASK_SOURCE_CONTENT_TYPE,
                    None,
                    Vec::new(),
                ))
                .map_err(|error| TaskError::Retrieval {
                    message: error.to_string(),
                })?;
            if !response.status.is_success() {
                let error = response_error(response);
                if attempt == 0
                    && matches!(
                        &error,
                        TaskError::Command {
                            code: "task_source_absent",
                            ..
                        }
                    )
                {
                    continue;
                }
                return Err(error);
            }
            ensure_content_type(&response, TASK_SOURCE_CONTENT_TYPE)?;
            let expected_digest =
                task.source_digest
                    .as_deref()
                    .ok_or_else(|| TaskError::InvalidResponse {
                        message: "present source metadata omitted its digest".to_string(),
                    })?;
            if response.body.len() != task.source_bytes
                || sha256_hex(&response.body) != expected_digest
            {
                continue;
            }
            std::str::from_utf8(&response.body).map_err(|error| TaskError::InvalidResponse {
                message: format!("task source is not UTF-8: {error}"),
            })?;
            return Ok(TaskSourceSnapshot {
                metadata,
                task,
                source: response.body,
            });
        }
        Err(TaskError::Command {
            code: "digest_mismatch",
            message: "task source changed or failed digest verification during both reads"
                .to_string(),
        })
    }

    pub(crate) fn mutate_latest(
        &self,
        mutation: TaskMutation<'_>,
    ) -> Result<TaskMutationResult, TaskError> {
        let metadata = self.metadata()?;
        let body = encode_task_mutation(&metadata, mutation)?;
        let response = self
            .client
            .send_bytes(bytes_request(
                Method::POST,
                "/api/v1/tasks/mutations",
                JSON_CONTENT_TYPE,
                Some(TASK_MUTATION_CONTENT_TYPE),
                body,
            ))
            .map_err(|error| TaskError::Command {
                code: "operation_failed",
                message: error.to_string(),
            })?;
        if !response.status.is_success() {
            return Err(response_error(response));
        }
        ensure_content_type(&response, JSON_CONTENT_TYPE)?;
        let raw_json =
            String::from_utf8(response.body).map_err(|error| TaskError::InvalidResponse {
                message: format!("task mutation response is not UTF-8: {error}"),
            })?;
        parse_mutation_success(raw_json, mutation.action(), &metadata)
    }
}

/// Stable text/JSON mutation contract: a UTF-8 JSON object with the string
/// fields `schema`, `action`, `expected_revision`, `catalog_fingerprint` and
/// the action-scoped optional fields `id`, `source_base64` and `ir_base64`.
/// The base64 fields are strict canonical RFC 4648 strings carrying the
/// exact source/IR bytes in a bounded body.
pub(crate) fn encode_task_mutation(
    metadata: &TaskList,
    mutation: TaskMutation<'_>,
) -> Result<Vec<u8>, TaskError> {
    let (id, source, ir) = match mutation {
        TaskMutation::Upsert { id, source, ir } => {
            validate_task_id(id)?;
            let parsed = TaskBlob::parse(ir)?;
            if parsed.tasks().len() != 1 || parsed.tasks()[0].id != id {
                return invalid_mutation("upsert requires standalone one-task IR matching its id");
            }
            if let Some(source) = source {
                validate_source(source)?;
            }
            (id, source.unwrap_or_default(), ir)
        }
        TaskMutation::Delete { id } => {
            validate_task_id(id)?;
            (id, &[][..], "")
        }
        TaskMutation::Clear => ("", &[][..], ""),
        TaskMutation::Import { ir } => {
            let parsed = TaskBlob::parse(ir)?;
            if parsed.tasks().is_empty() {
                return invalid_mutation("full IR import requires at least one task");
            }
            ("", &[][..], ir)
        }
    };
    if source.len() > SOURCE_MAX_BYTES || ir.len() > MAX_BLOB_SIZE {
        return Err(TaskError::Command {
            code: "body_too_large",
            message: "task source or IR exceeds 4000 bytes".to_string(),
        });
    }
    let mut body = String::from("{\"schema\":\"radxa-linkr-debugger.v1\",\"action\":\"");
    body.push_str(mutation.action());
    body.push_str("\",\"expected_revision\":\"");
    body.push_str(&metadata.revision_text);
    body.push_str("\",\"catalog_fingerprint\":\"");
    body.push_str(&metadata.catalog_fingerprint);
    body.push('"');
    if !id.is_empty() {
        body.push_str(",\"id\":");
        body.push_str(&json_string(id)?);
    }
    if !source.is_empty() {
        body.push_str(",\"source_base64\":\"");
        body.push_str(&base64_encode(source));
        body.push('"');
    }
    if !ir.is_empty() {
        body.push_str(",\"ir_base64\":\"");
        body.push_str(&base64_encode(ir.as_bytes()));
        body.push('"');
    }
    body.push('}');
    if body.len() > TASK_MUTATION_MAX_BYTES {
        return Err(TaskError::Command {
            code: "body_too_large",
            message: "task mutation exceeds 12288 bytes".to_string(),
        });
    }
    Ok(body.into_bytes())
}

/// Canonical RFC 4648 base64 with padding and zero pad bits.
pub(crate) fn base64_encode(bytes: &[u8]) -> String {
    const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for chunk in bytes.chunks(3) {
        let remain = chunk.len();
        let value = (u32::from(chunk[0]) << 16)
            | (if remain > 1 {
                u32::from(chunk[1]) << 8
            } else {
                0
            })
            | if remain > 2 { u32::from(chunk[2]) } else { 0 };
        out.push(ALPHABET[(value >> 18 & 0x3f) as usize] as char);
        out.push(ALPHABET[(value >> 12 & 0x3f) as usize] as char);
        out.push(if remain > 1 {
            ALPHABET[(value >> 6 & 0x3f) as usize] as char
        } else {
            '='
        });
        out.push(if remain > 2 {
            ALPHABET[(value & 0x3f) as usize] as char
        } else {
            '='
        });
    }
    out
}

fn json_string(value: &str) -> Result<String, TaskError> {
    serde_json::to_string(value).map_err(|error| TaskError::InvalidResponse {
        message: format!("cannot encode task mutation JSON string: {error}"),
    })
}

fn validate_source(source: &[u8]) -> Result<(), TaskError> {
    if source.is_empty() {
        return invalid_mutation("task source must contain 1..4000 UTF-8 bytes");
    }
    std::str::from_utf8(source).map_err(|_| TaskError::Command {
        code: "invalid_task",
        message: "task source is not UTF-8".to_string(),
    })?;
    if source
        .iter()
        .any(|byte| (*byte < 0x20 && !matches!(*byte, b'\n' | b'\t' | b'\r')) || *byte == 0x7f)
    {
        return Err(TaskError::Command {
            code: "invalid_task",
            message: "task source contains a forbidden control byte".to_string(),
        });
    }
    Ok(())
}

fn verify_snapshot(metadata: &TaskList, ir: &str) -> Result<(), TaskError> {
    let blob = TaskBlob::parse(ir)?;
    if blob.tasks().len() != metadata.tasks.len() {
        return invalid_response("task IR count differs from metadata");
    }
    for summary in &metadata.tasks {
        let task = blob.task(&summary.id)?;
        let section = task_ir_section(ir, &summary.id)?;
        if task.records.len() != summary.request_count
            || section.len() != summary.ir_bytes
            || sha256_hex(section.as_bytes()) != summary.task_ir_digest
        {
            return Err(TaskError::Command {
                code: "digest_mismatch",
                message: format!("task {:?} IR differs from metadata", summary.id),
            });
        }
    }
    Ok(())
}

fn bytes_request(
    method: Method,
    path: &str,
    accept: &'static str,
    content_type: Option<&'static str>,
    body: Vec<u8>,
) -> BoardBytesRequest {
    BoardBytesRequest {
        method,
        path: path.to_string(),
        query: Vec::new(),
        accept,
        content_type,
        body,
    }
}

fn successful_body(
    response: BoardBytesResponse,
    expected_content_type: &str,
) -> Result<Vec<u8>, TaskError> {
    if !response.status.is_success() {
        return Err(response_error(response));
    }
    ensure_content_type(&response, expected_content_type)?;
    Ok(response.body)
}

fn ensure_content_type(response: &BoardBytesResponse, expected: &str) -> Result<(), TaskError> {
    let actual = response
        .content_type
        .as_deref()
        .and_then(|value| value.split(';').next())
        .map(str::trim);
    if actual != Some(expected) {
        return invalid_response(format!(
            "unexpected task content type {:?}; expected {expected}",
            response.content_type
        ));
    }
    Ok(())
}

#[derive(Deserialize)]
struct MutationSuccessEnvelope {
    schema: String,
    ok: bool,
    command: String,
    action: String,
    task_store_id: String,
    revision: String,
    catalog_fingerprint: String,
    task: Option<TaskSummary>,
}

fn parse_mutation_success(
    raw_json: String,
    expected_action: &str,
    expected: &TaskList,
) -> Result<TaskMutationResult, TaskError> {
    let response: MutationSuccessEnvelope =
        serde_json::from_str(&raw_json).map_err(|error| TaskError::InvalidResponse {
            message: error.to_string(),
        })?;
    if response.schema != JSON_SCHEMA
        || !response.ok
        || response.command != "tasks"
        || response.action != expected_action
    {
        return invalid_response(format!(
            "expected successful tasks {expected_action} envelope"
        ));
    }
    validate_store_id(&response.task_store_id)?;
    parse_revision(&response.revision)?;
    decode_sha256(&response.catalog_fingerprint)?;
    if response.task_store_id != expected.task_store_id
        || response.catalog_fingerprint != expected.catalog_fingerprint
    {
        return invalid_response("mutation response changed task store/catalog identity");
    }
    if let Some(task) = &response.task {
        validate_summary(task)?;
    }
    if (expected_action == "upsert") != response.task.is_some() {
        return invalid_response("mutation affected-task metadata does not match its action");
    }
    Ok(TaskMutationResult {
        raw_json,
        task_store_id: response.task_store_id,
        revision_text: response.revision,
        catalog_fingerprint: response.catalog_fingerprint,
        task: response.task,
    })
}

#[derive(Deserialize)]
struct ErrorDetail {
    code: String,
    message: String,
}

#[derive(Deserialize)]
struct ErrorEnvelope {
    schema: String,
    ok: bool,
    command: String,
    error: ErrorDetail,
    revision: Option<String>,
    catalog_fingerprint: Option<String>,
}

fn response_error(response: BoardBytesResponse) -> TaskError {
    if ensure_content_type(&response, JSON_CONTENT_TYPE).is_err() {
        return TaskError::InvalidResponse {
            message: format!(
                "HTTP {} task error has unexpected content type {:?}",
                response.status, response.content_type
            ),
        };
    }
    let parsed = serde_json::from_slice::<ErrorEnvelope>(&response.body);
    let Ok(envelope) = parsed else {
        return TaskError::InvalidResponse {
            message: format!("HTTP {} task error is not valid JSON", response.status),
        };
    };
    if envelope.schema != JSON_SCHEMA || envelope.ok || envelope.command != "tasks" {
        return TaskError::InvalidResponse {
            message: format!(
                "HTTP {} returned an invalid tasks error envelope",
                response.status
            ),
        };
    }
    let Some((code, expected_status)) = stable_error(&envelope.error.code) else {
        return TaskError::InvalidResponse {
            message: format!("unknown task error code {:?}", envelope.error.code),
        };
    };
    if response.status != expected_status {
        return TaskError::InvalidResponse {
            message: format!(
                "task error {code} used HTTP {}; expected {expected_status}",
                response.status
            ),
        };
    }
    let mut message = envelope.error.message;
    if matches!(code, "revision_conflict" | "catalog_conflict") {
        let current_revision = envelope
            .revision
            .as_deref()
            .and_then(|value| parse_revision(value).ok());
        let current_catalog = envelope
            .catalog_fingerprint
            .as_deref()
            .filter(|value| decode_sha256(value).is_ok());
        if current_revision.is_none() || current_catalog.is_none() {
            return TaskError::InvalidResponse {
                message: format!("task {code} omitted current revision/catalog identity"),
            };
        }
        message = format!(
            "{message}; current revision={} catalog={}; mutation was not retried",
            current_revision.expect("checked above"),
            current_catalog.expect("checked above")
        );
    }
    TaskError::Command { code, message }
}

fn stable_error(code: &str) -> Option<(&'static str, StatusCode)> {
    Some(match code {
        "revision_conflict" => ("revision_conflict", StatusCode::CONFLICT),
        "catalog_conflict" => ("catalog_conflict", StatusCode::CONFLICT),
        "invalid_mutation" => ("invalid_mutation", StatusCode::BAD_REQUEST),
        "invalid_task" => ("invalid_task", StatusCode::BAD_REQUEST),
        "body_too_large" => ("body_too_large", StatusCode::BAD_REQUEST),
        "task_source_absent" => ("task_source_absent", StatusCode::NOT_FOUND),
        "busy" => ("busy", StatusCode::CONFLICT),
        "storage_full" => ("storage_full", StatusCode::INSUFFICIENT_STORAGE),
        "readback_failed" => ("readback_failed", StatusCode::INTERNAL_SERVER_ERROR),
        "storage_error" => ("storage_error", StatusCode::INTERNAL_SERVER_ERROR),
        _ => return None,
    })
}

pub(crate) fn sha256_hex(bytes: &[u8]) -> String {
    let digest = Sha256::digest(bytes);
    let mut output = String::with_capacity(64);
    for byte in digest {
        use std::fmt::Write;
        write!(output, "{byte:02x}").expect("writing to String cannot fail");
    }
    output
}

fn invalid_mutation<T>(message: impl Into<String>) -> Result<T, TaskError> {
    Err(TaskError::Command {
        code: "invalid_mutation",
        message: message.into(),
    })
}

fn invalid_response<T>(message: impl Into<String>) -> Result<T, TaskError> {
    Err(TaskError::InvalidResponse {
        message: message.into(),
    })
}
