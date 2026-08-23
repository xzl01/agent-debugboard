#[derive(Debug, thiserror::Error)]
pub(crate) enum TaskError {
    #[error("failed to retrieve saved tasks: {message}")]
    Retrieval { message: String },
    #[error("invalid task response: {message}")]
    InvalidResponse { message: String },
    #[error("invalid task blob: {message}")]
    InvalidBlob { message: String },
    #[error("saved task store is empty")]
    TaskStoreEmpty,
    #[error("task {task_id:?} was not found")]
    TaskNotFound { task_id: String },
    #[error("task record {record_index} path {path:?} failed: {message}")]
    RequestFailed {
        record_index: usize,
        path: String,
        message: String,
    },
    #[error("task cancelled after {requests_completed} completed request(s); hardware state may be partial")]
    Cancelled { requests_completed: usize },
    #[error("{message}")]
    Command { code: &'static str, message: String },
}

impl TaskError {
    pub(crate) const fn code(&self) -> &'static str {
        match self {
            Self::Retrieval { .. } => "retrieval_failed",
            Self::InvalidResponse { .. } => "invalid_response",
            Self::InvalidBlob { .. } => "invalid_blob",
            Self::TaskStoreEmpty => "task_store_empty",
            Self::TaskNotFound { .. } => "task_not_found",
            Self::RequestFailed { .. } => "request_failed",
            Self::Cancelled { .. } => "cancelled",
            Self::Command { code, .. } => code,
        }
    }

    pub(crate) const fn record(&self) -> Option<(usize, &str)> {
        match self {
            Self::RequestFailed {
                record_index, path, ..
            } => Some((*record_index, path.as_str())),
            Self::Retrieval { .. }
            | Self::InvalidResponse { .. }
            | Self::InvalidBlob { .. }
            | Self::TaskStoreEmpty
            | Self::TaskNotFound { .. }
            | Self::Cancelled { .. }
            | Self::Command { .. } => None,
        }
    }

    pub(crate) const fn requests_completed(&self) -> Option<usize> {
        match self {
            Self::Cancelled { requests_completed } => Some(*requests_completed),
            Self::Retrieval { .. }
            | Self::InvalidResponse { .. }
            | Self::InvalidBlob { .. }
            | Self::TaskStoreEmpty
            | Self::TaskNotFound { .. }
            | Self::RequestFailed { .. }
            | Self::Command { .. } => None,
        }
    }
}
