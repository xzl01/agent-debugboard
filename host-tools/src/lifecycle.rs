//! One admission boundary for tasks that may outlive an HTTP upgrade.
use std::{
    future::Future,
    sync::{Arc, Mutex},
    time::Duration,
};

use tokio::task::{AbortHandle, JoinSet};
use tokio_util::sync::CancellationToken;

pub(crate) const SHUTDOWN_GRACE: Duration = Duration::from_secs(1);

#[derive(Clone, Default)]
pub(crate) struct Lifecycle {
    pub(crate) cancel: CancellationToken,
    tasks: Arc<Mutex<JoinSet<()>>>,
}

impl Lifecycle {
    pub(crate) fn spawn(
        &self,
        future: impl Future<Output = ()> + Send + 'static,
    ) -> Option<AbortHandle> {
        let mut tasks = self.tasks.lock().expect("lifecycle tasks");
        if self.cancel.is_cancelled() {
            return None;
        }
        while tasks.try_join_next().is_some() {}
        Some(tasks.spawn(future))
    }

    pub(crate) fn close(&self) {
        // Serialize cancellation with admission, including late upgrade callbacks.
        let _tasks = self.tasks.lock().expect("lifecycle tasks");
        self.cancel.cancel();
    }

    pub(crate) fn abort(&self) {
        let mut tasks = self.tasks.lock().expect("lifecycle tasks");
        self.cancel.cancel();
        tasks.abort_all();
    }

    pub(crate) async fn shutdown(&self) {
        self.close();
        let mut tasks = std::mem::take(&mut *self.tasks.lock().expect("lifecycle tasks"));
        if tokio::time::timeout(SHUTDOWN_GRACE, async {
            while tasks.join_next().await.is_some() {}
        })
        .await
        .is_err()
        {
            // Only async tasks are registered here; abort drops their sockets/futures.
            tasks.shutdown().await;
        }
    }
}

/// Also close admission if the tray aborts the owner instead of requesting shutdown.
pub(crate) struct CancelOnDrop(pub(crate) Lifecycle);

impl Drop for CancelOnDrop {
    fn drop(&mut self) {
        self.0.abort();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn shutdown_reaps_uncooperative_tasks_and_rejects_late_admission() {
        let lifecycle = Lifecycle::default();
        let task = lifecycle.spawn(std::future::pending()).unwrap();
        tokio::time::timeout(Duration::from_secs(2), lifecycle.shutdown())
            .await
            .unwrap();
        assert!(task.is_finished());
        assert!(lifecycle.spawn(async { panic!("late task ran") }).is_none());
        assert!(lifecycle.cancel.child_token().is_cancelled());
    }
}
