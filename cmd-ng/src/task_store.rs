use crate::client::BoardTransport;
use crate::json_contract::JSON_SCHEMA;
use crate::task_api::{sha256_hex, TaskApi, TaskMutation, TaskMutationResult};
use crate::task_blob::{build_task_blob, TaskBlob};
use crate::task_error::TaskError;
use crate::task_output::TaskCommandIo;
use anyhow::Result;
use serde_json::json;
use std::fs;
use std::path::Path;

#[derive(Debug)]
pub(crate) struct StoreArgs {
    pub(crate) path: String,
    pub(crate) task_id: Option<String>,
}

pub(crate) struct TaskStore<'a> {
    api: TaskApi<'a>,
}

impl<'a> TaskStore<'a> {
    pub(crate) const fn new(client: &'a dyn BoardTransport) -> Self {
        Self {
            api: TaskApi::new(client),
        }
    }

    pub(crate) fn run_store(&self, args: &StoreArgs, io: &mut TaskCommandIo<'_>) -> Result<u8> {
        let text = match read_utf8(&args.path) {
            Ok(text) => text,
            Err(error) => return io.failure("store", &error, 2),
        };
        let fallback_id = args
            .task_id
            .clone()
            .unwrap_or_else(|| task_id_from_path(&args.path));
        let ir = match build_task_blob(&text, &fallback_id) {
            Ok(ir) => ir,
            Err(error) => return io.failure("store", &error, 2),
        };
        let parsed = match TaskBlob::parse(&ir) {
            Ok(parsed) => parsed,
            Err(error) => return io.failure("store", &error, 2),
        };
        if parsed.tasks().len() != 1 {
            let error = TaskError::Command {
                code: "invalid_mutation",
                message: "task store accepts exactly one task; use task import for full-set IR"
                    .to_string(),
            };
            return io.failure("store", &error, 2);
        }
        let task_id = parsed.tasks()[0].id.clone();
        if text.starts_with(crate::task_blob::TASK_MARKER_VERSION)
            && args.task_id.as_deref().is_some_and(|id| id != task_id)
        {
            let error = TaskError::Command {
                code: "invalid_mutation",
                message: format!(
                    "explicit task id {:?} differs from IR task id {task_id:?}",
                    args.task_id.as_deref().unwrap_or_default()
                ),
            };
            return io.failure("store", &error, 2);
        }
        self.run_mutation(
            "store",
            format!("task store ok task={task_id}"),
            TaskMutation::Upsert {
                id: &task_id,
                source: None,
                ir: &ir,
            },
            io,
        )
    }

    pub(crate) fn run_delete(&self, task_id: &str, io: &mut TaskCommandIo<'_>) -> Result<u8> {
        self.run_mutation(
            "delete",
            format!("task delete ok task={task_id}"),
            TaskMutation::Delete { id: task_id },
            io,
        )
    }

    pub(crate) fn run_clear(&self, io: &mut TaskCommandIo<'_>) -> Result<u8> {
        self.run_mutation(
            "clear",
            "task clear ok".to_string(),
            TaskMutation::Clear,
            io,
        )
    }

    pub(crate) fn run_import(&self, path: &str, io: &mut TaskCommandIo<'_>) -> Result<u8> {
        let ir = match read_utf8(path) {
            Ok(ir) => ir,
            Err(error) => return io.failure("import", &error, 2),
        };
        match TaskBlob::parse(&ir) {
            Ok(blob) if !blob.tasks().is_empty() => {}
            Ok(_) => {
                let error = TaskError::Command {
                    code: "invalid_mutation",
                    message: "full IR import requires at least one task; use task clear instead"
                        .to_string(),
                };
                return io.failure("import", &error, 2);
            }
            Err(error) => return io.failure("import", &error, 2),
        }
        self.run_mutation(
            "import",
            format!("task IR import ok path={path:?}"),
            TaskMutation::Import { ir: &ir },
            io,
        )
    }

    pub(crate) fn run_export(&self, path: &str, io: &mut TaskCommandIo<'_>) -> Result<u8> {
        let snapshot = match self.api.snapshot() {
            Ok(snapshot) => snapshot,
            Err(error) => return io.failure("export", &error, 1),
        };
        if let Err(error) = fs::write(path, snapshot.ir.as_bytes()) {
            let error = TaskError::Command {
                code: "write_failed",
                message: format!("cannot write {path:?}: {error}"),
            };
            return io.failure("export", &error, 2);
        }
        if io.json_output {
            writeln!(
                io.stdout,
                "{}",
                json!({
                    "schema": JSON_SCHEMA,
                    "ok": true,
                    "command": "task",
                    "action": "export",
                    "path": path,
                    "task_store_id": snapshot.metadata.task_store_id,
                    "revision": snapshot.metadata.revision_text,
                    "bytes": snapshot.ir.len(),
                    "sha256": snapshot.metadata.full_ir_digest,
                })
            )?;
        } else {
            writeln!(
                io.stdout,
                "task IR export ok path={path:?} bytes={}",
                snapshot.ir.len()
            )?;
        }
        Ok(0)
    }

    pub(crate) fn run_source_export(
        &self,
        task_id: &str,
        path: &str,
        io: &mut TaskCommandIo<'_>,
    ) -> Result<u8> {
        let snapshot = match self.api.source(task_id) {
            Ok(snapshot) => snapshot,
            Err(error) => return io.failure("source", &error, 1),
        };
        if let Err(error) = fs::write(path, &snapshot.source) {
            let error = TaskError::Command {
                code: "write_failed",
                message: format!("cannot write {path:?}: {error}"),
            };
            return io.failure("source", &error, 2);
        }
        if io.json_output {
            writeln!(
                io.stdout,
                "{}",
                json!({
                    "schema": JSON_SCHEMA,
                    "ok": true,
                    "command": "task",
                    "action": "source",
                    "task_id": task_id,
                    "path": path,
                    "task_store_id": snapshot.metadata.task_store_id,
                    "revision": snapshot.metadata.revision_text,
                    "slot": snapshot.task.slot,
                    "bytes": snapshot.source.len(),
                    "source_digest": sha256_hex(&snapshot.source),
                    "task_ir_digest": snapshot.task.task_ir_digest,
                    "lossless_source": true,
                    "editor_opened": false,
                })
            )?;
        } else {
            writeln!(
                io.stdout,
                "task source export ok task={task_id} path={path:?} lossless=yes editor-opened=no"
            )?;
        }
        Ok(0)
    }

    fn run_mutation(
        &self,
        action: &str,
        success: String,
        mutation: TaskMutation<'_>,
        io: &mut TaskCommandIo<'_>,
    ) -> Result<u8> {
        let result = match self.api.mutate_latest(mutation) {
            Ok(result) => result,
            Err(error) => return io.failure(action, &error, 1),
        };
        write_mutation_success(io, result, success)
    }
}

fn write_mutation_success(
    io: &mut TaskCommandIo<'_>,
    result: TaskMutationResult,
    success: String,
) -> Result<u8> {
    if io.json_output {
        writeln!(io.stdout, "{}", result.raw_json.trim())?;
    } else {
        writeln!(
            io.stdout,
            "{success} revision={} store={} catalog={} affected={}",
            result.revision_text,
            result.task_store_id,
            result.catalog_fingerprint,
            result.task.as_ref().map_or("none", |task| task.id.as_str())
        )?;
    }
    Ok(0)
}

fn read_utf8(path: &str) -> Result<String, TaskError> {
    fs::read_to_string(path).map_err(|error| TaskError::Command {
        code: "read_failed",
        message: format!("cannot read {path:?}: {error}"),
    })
}

fn task_id_from_path(path: &str) -> String {
    let stem = Path::new(path)
        .file_stem()
        .and_then(|stem| stem.to_str())
        .filter(|stem| !stem.is_empty())
        .unwrap_or("task");
    let mut task_id = stem
        .chars()
        .map(|character| {
            if !character.is_control() && !character.is_whitespace() && character != '#' {
                character
            } else {
                '-'
            }
        })
        .collect::<String>();
    while task_id.len() > 63 {
        task_id.pop();
    }
    task_id
}
