use crate::client::{BoardRequest, BoardTransport};
use crate::task_blob::TaskRecord;
use crate::task_error::TaskError;
use reqwest::Method;
use serde::Serialize;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::OnceLock;
use std::time::{Duration, Instant};

const CANCEL_POLL_INTERVAL: Duration = Duration::from_millis(25);
static PROCESS_CANCELLED: AtomicBool = AtomicBool::new(false);
static PROCESS_HANDLER: OnceLock<Result<(), String>> = OnceLock::new();

pub(crate) trait TaskCancellation {
    fn is_cancelled(&self) -> bool;
}

pub(crate) trait TaskSleeper {
    fn sleep(&self, delay: Duration, cancellation: &dyn TaskCancellation) -> bool;
}

pub(crate) struct ProcessCancellation;

impl ProcessCancellation {
    pub(crate) fn install() -> Result<Self, TaskError> {
        PROCESS_CANCELLED.store(false, Ordering::SeqCst);
        let result = PROCESS_HANDLER.get_or_init(|| {
            ctrlc::set_handler(|| PROCESS_CANCELLED.store(true, Ordering::SeqCst))
                .map_err(|error| error.to_string())
        });
        match result {
            Ok(()) => Ok(Self),
            Err(message) => Err(TaskError::Command {
                code: "cancellation_unavailable",
                message: format!("failed to install Ctrl+C cancellation handler: {message}"),
            }),
        }
    }
}

impl TaskCancellation for ProcessCancellation {
    fn is_cancelled(&self) -> bool {
        PROCESS_CANCELLED.load(Ordering::SeqCst)
    }
}

#[cfg(test)]
pub(crate) struct NeverCancelled;

#[cfg(test)]
impl TaskCancellation for NeverCancelled {
    fn is_cancelled(&self) -> bool {
        false
    }
}

pub(crate) struct ThreadSleeper;

impl TaskSleeper for ThreadSleeper {
    fn sleep(&self, delay: Duration, cancellation: &dyn TaskCancellation) -> bool {
        let started = Instant::now();
        while started.elapsed() < delay {
            if cancellation.is_cancelled() {
                return true;
            }
            let remaining = delay.saturating_sub(started.elapsed());
            std::thread::sleep(remaining.min(CANCEL_POLL_INTERVAL));
        }
        cancellation.is_cancelled()
    }
}

#[derive(Debug, Serialize)]
pub(crate) struct TaskCleanupOutcome {
    pub(crate) attempted: bool,
    pub(crate) ok: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) error: Option<String>,
}

pub(crate) struct TaskRunFailure {
    pub(crate) error: TaskError,
    pub(crate) cleanup: Option<TaskCleanupOutcome>,
}

impl From<TaskError> for TaskRunFailure {
    fn from(error: TaskError) -> Self {
        Self {
            error,
            cleanup: None,
        }
    }
}

pub(crate) struct TaskExecutor<'a> {
    client: &'a dyn BoardTransport,
    sleeper: &'a dyn TaskSleeper,
    cancellation: &'a dyn TaskCancellation,
}

impl<'a> TaskExecutor<'a> {
    pub(crate) const fn new(
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

    pub(crate) fn execute(
        &self,
        records: &[TaskRecord],
        cleanup: Option<&TaskRecord>,
    ) -> Result<usize, TaskRunFailure> {
        let mut completed = 0;
        for (offset, record) in records.iter().enumerate() {
            if self.cancellation.is_cancelled() {
                return Err(cancelled(self.client, cleanup, completed));
            }
            if let Err(error) = send_record(self.client, record) {
                return Err(TaskRunFailure {
                    error: TaskError::RequestFailed {
                        record_index: offset + 1,
                        path: record.path.clone(),
                        message: error.to_string(),
                    },
                    cleanup: attempt_cleanup(self.client, cleanup),
                });
            }
            completed += 1;
            if !record.wait.is_zero() && self.sleeper.sleep(record.wait, self.cancellation) {
                return Err(cancelled(self.client, cleanup, completed));
            }
        }
        Ok(completed)
    }
}

fn cancelled(
    client: &dyn BoardTransport,
    cleanup: Option<&TaskRecord>,
    completed: usize,
) -> TaskRunFailure {
    TaskRunFailure {
        error: TaskError::Cancelled {
            requests_completed: completed,
        },
        cleanup: (completed > 0)
            .then(|| attempt_cleanup(client, cleanup))
            .flatten(),
    }
}

fn attempt_cleanup(
    client: &dyn BoardTransport,
    cleanup: Option<&TaskRecord>,
) -> Option<TaskCleanupOutcome> {
    cleanup.map(|record| match send_record(client, record) {
        Ok(_) => TaskCleanupOutcome {
            attempted: true,
            ok: true,
            error: None,
        },
        Err(error) => TaskCleanupOutcome {
            attempted: true,
            ok: false,
            error: Some(error.to_string()),
        },
    })
}

fn send_record(client: &dyn BoardTransport, record: &TaskRecord) -> anyhow::Result<String> {
    client.send_text(BoardRequest {
        method: Method::PUT,
        path: record.path.clone(),
        query: Vec::new(),
        body: Some(record.body.clone()),
    })
}
