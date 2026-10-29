use crate::client::{BoardRequest, BoardTransport};
use crate::json_contract::JSON_SCHEMA;
use crate::task_api::TaskApi;
use crate::task_blob::{TaskBlob, TaskError, TaskList};
use crate::task_catalog::FirmwareTaskCatalog;
use crate::task_envelope::{catalog_json, merged_task_catalog, BUILTIN_TASK_ID_PREFIX};
#[cfg(test)]
use crate::task_execution::NeverCancelled;
use crate::task_execution::{
    ProcessCancellation, TaskCancellation, TaskExecutor, TaskRunFailure, TaskSleeper, ThreadSleeper,
};
pub(crate) use crate::task_output::TaskCommandIo;
use crate::task_parse::{parse, TaskCommand, TASK_USAGE};
use crate::task_store::{prepare_import, prepare_store, PreparedStore, TaskStore};
use anyhow::Result;
use reqwest::Method;
use serde_json::json;

pub(crate) struct TaskRunner<'a> {
    client: &'a dyn BoardTransport,
    sleeper: &'a dyn TaskSleeper,
    cancellation: &'a dyn TaskCancellation,
}

pub(crate) enum PreparedTask {
    Command(TaskCommand),
    Store(PreparedStore),
    Import { path: String, ir: String },
}

pub(crate) fn prepare(args: &[String]) -> Result<PreparedTask, (&'static str, TaskError)> {
    let command = parse(args).map_err(|error| ("parse", error))?;
    if let TaskCommand::Delete { task_id }
    | TaskCommand::Source { task_id, .. }
    | TaskCommand::Run { task_id } = &command
    {
        crate::task_blob::validate_task_id(task_id).map_err(|error| {
            (
                "parse",
                TaskError::Command {
                    code: "usage",
                    message: error.to_string(),
                },
            )
        })?;
    }
    match command {
        TaskCommand::Store(args) => prepare_store(&args)
            .map(PreparedTask::Store)
            .map_err(|error| ("store", error)),
        TaskCommand::Import { path } => {
            let ir = prepare_import(&path).map_err(|error| ("import", error))?;
            Ok(PreparedTask::Import { path, ir })
        }
        command => Ok(PreparedTask::Command(command)),
    }
}

impl<'a> TaskRunner<'a> {
    #[cfg(test)]
    pub(crate) const fn new(client: &'a dyn BoardTransport, sleeper: &'a dyn TaskSleeper) -> Self {
        Self {
            client,
            sleeper,
            cancellation: &NeverCancelled,
        }
    }

    pub(crate) const fn with_cancellation(
        client: &'a dyn BoardTransport,
        sleeper: &'a dyn TaskSleeper,
        cancellation: &'a dyn TaskCancellation,
    ) -> Self {
        Self {
            client,
            sleeper,
            cancellation,
        }
    }

    #[cfg(test)]
    pub(crate) fn run(&self, args: &[String], mut io: TaskCommandIo<'_>) -> Result<u8> {
        let prepared = match prepare(args) {
            Ok(command) => command,
            Err((action, error)) => return io.failure(action, &error, 2),
        };
        self.run_prepared(prepared, io)
    }

    fn run_prepared(&self, prepared: PreparedTask, mut io: TaskCommandIo<'_>) -> Result<u8> {
        let command = match prepared {
            PreparedTask::Command(command) => command,
            PreparedTask::Store(prepared) => {
                return TaskStore::new(self.client).run_store(prepared, &mut io)
            }
            PreparedTask::Import { path, ir } => {
                return TaskStore::new(self.client).run_import(&path, &ir, &mut io)
            }
        };
        match command {
            TaskCommand::Help => {
                writeln!(io.stdout, "{TASK_USAGE}")?;
                Ok(0)
            }
            TaskCommand::List => self.run_list(&mut io),
            TaskCommand::Store(_) | TaskCommand::Import { .. } => {
                unreachable!("task files must be prepared locally")
            }
            TaskCommand::Delete { task_id } => {
                TaskStore::new(self.client).run_delete(&task_id, &mut io)
            }
            TaskCommand::Export { path } => TaskStore::new(self.client).run_export(&path, &mut io),
            TaskCommand::Source { task_id, path } => {
                TaskStore::new(self.client).run_source_export(&task_id, &path, &mut io)
            }
            TaskCommand::Run { task_id } => self.run_saved(&task_id, &mut io),
            TaskCommand::Clear => TaskStore::new(self.client).run_clear(&mut io),
        }
    }

    fn fetch_list(&self) -> Result<TaskList, TaskError> {
        TaskApi::new(self.client).metadata()
    }

    fn fetch_catalog(&self) -> Result<FirmwareTaskCatalog, TaskError> {
        let output = self
            .client
            .send_text(BoardRequest {
                method: Method::GET,
                path: "/api/v1/tasks/catalog".to_string(),
                query: Vec::new(),
                body: None,
            })
            .map_err(|error| TaskError::Command {
                code: "catalog_unavailable",
                message: format!("failed to retrieve firmware task catalog: {error}"),
            })?;
        FirmwareTaskCatalog::from_raw(output)
    }

    fn run_list(&self, io: &mut TaskCommandIo<'_>) -> Result<u8> {
        let list = match self.fetch_list() {
            Ok(list) => list,
            Err(error) => return io.failure("list", &error, 1),
        };
        let firmware_catalog = self.fetch_catalog();
        let catalog_error = firmware_catalog.as_ref().err();
        let catalog = merged_task_catalog(&list, firmware_catalog.as_ref().ok());
        if io.json_output {
            writeln!(
                io.stdout,
                "{}",
                catalog_json(&list, &catalog, catalog_error)?
            )?;
        } else {
            for task in &catalog {
                let collision = if task.shadowed_stored {
                    " stored-id-shadowed"
                } else {
                    ""
                };
                writeln!(
                    io.stdout,
                    "{:<32} {:<40} requests={} source={} lossless-source={}{}",
                    task.id,
                    task.name,
                    task.request_count,
                    task.source.name(),
                    task.lossless_source,
                    collision,
                )?;
            }
            if let Some(storage) = &list.storage {
                writeln!(
                    io.stdout,
                    "storage nvs_total={} nvs_used={} nvs_free={} task_bytes={} reserve={} measured={} save_allowed={}",
                    storage.total_bytes,
                    storage.used_bytes,
                    storage.free_bytes,
                    storage.task_bytes,
                    storage.reserve_bytes,
                    storage.measured,
                    storage.save_allowed,
                )?;
            }
            if let Some(error) = catalog_error {
                writeln!(io.stderr, "catalog unavailable: {error}")?;
            }
        }
        Ok(0)
    }

    fn run_saved(&self, task_id: &str, io: &mut TaskCommandIo<'_>) -> Result<u8> {
        let result = self.execute_task(task_id);
        match result {
            Ok(executed) => {
                if io.json_output {
                    writeln!(
                        io.stdout,
                        "{}",
                        json!({
                            "schema": JSON_SCHEMA,
                            "ok": true,
                            "command": "task",
                            "action": "run",
                            "task_id": task_id,
                            "requests_executed": executed,
                        })
                    )?;
                } else {
                    writeln!(io.stdout, "task run ok task={task_id} requests={executed}")?;
                }
                Ok(0)
            }
            Err(failure) => {
                io.execution_failure("run", &failure.error, failure.cleanup.as_ref(), 1)
            }
        }
    }

    fn execute_task(&self, task_id: &str) -> Result<usize, TaskRunFailure> {
        if self.cancellation.is_cancelled() {
            return Err(TaskError::Cancelled {
                requests_completed: 0,
            }
            .into());
        }
        let executor = TaskExecutor::new(self.client, self.sleeper, self.cancellation);
        match self.fetch_catalog() {
            Ok(catalog) => {
                if let Some(task) = catalog.task(task_id) {
                    return executor.execute(&task.records, Some(&task.cleanup));
                }
                if task_id.starts_with(BUILTIN_TASK_ID_PREFIX) {
                    return Err(TaskError::Command {
                        code: "catalog_unavailable",
                        message: format!(
                            "firmware task catalog does not define reserved task {task_id:?}"
                        ),
                    }
                    .into());
                }
            }
            Err(error) if task_id.starts_with(BUILTIN_TASK_ID_PREFIX) => return Err(error.into()),
            Err(_) => {}
        }
        if self.cancellation.is_cancelled() {
            return Err(TaskError::Cancelled {
                requests_completed: 0,
            }
            .into());
        }
        let snapshot = TaskApi::new(self.client).snapshot()?;
        let blob = TaskBlob::parse(&snapshot.ir)?;
        let task = blob.task(task_id)?;
        executor.execute(&task.records, None)
    }
}

pub(crate) fn run_prepared(
    client: &dyn BoardTransport,
    command: PreparedTask,
    mut io: TaskCommandIo<'_>,
) -> Result<u8> {
    let cancellation = match ProcessCancellation::install() {
        Ok(cancellation) => cancellation,
        Err(error) => return io.failure("run", &error, 1),
    };
    TaskRunner::with_cancellation(client, &ThreadSleeper, &cancellation).run_prepared(command, io)
}
