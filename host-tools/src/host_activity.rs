use std::sync::{
    atomic::{AtomicUsize, Ordering},
    Arc, Mutex,
};
use std::time::{Duration, Instant};

use serde::Serialize;

const LOGIC_ACTIVITY_LATCH: Duration = Duration::from_secs(1);

#[derive(Clone, Default)]
pub(crate) struct HostActivity {
    logic_analyzer_sessions: Arc<AtomicUsize>,
    last_logic_activity: Arc<Mutex<Option<Instant>>>,
}

#[derive(Debug, Serialize)]
pub(crate) struct HostActivitySnapshot {
    pub(crate) logic_analyzer_sessions: usize,
    pub(crate) logic_analyzer_active: bool,
}

pub(crate) struct LogicAnalyzerSession {
    activity: HostActivity,
}

impl HostActivity {
    pub(crate) fn begin_logic_analyzer(&self) -> LogicAnalyzerSession {
        self.logic_analyzer_sessions.fetch_add(1, Ordering::Relaxed);
        self.mark_logic_activity();
        LogicAnalyzerSession {
            activity: self.clone(),
        }
    }

    pub(crate) fn snapshot(&self) -> HostActivitySnapshot {
        let logic_analyzer_sessions = self.logic_analyzer_sessions.load(Ordering::Relaxed);
        let last_activity = *self
            .last_logic_activity
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        // ponytail: 1 s latch covers the tray's 500 ms indicator probe without an event channel.
        let logic_analyzer_active =
            logic_analyzer_active_at(logic_analyzer_sessions, last_activity, Instant::now());
        HostActivitySnapshot {
            logic_analyzer_sessions,
            logic_analyzer_active,
        }
    }

    fn mark_logic_activity(&self) {
        *self
            .last_logic_activity
            .lock()
            .unwrap_or_else(|error| error.into_inner()) = Some(Instant::now());
    }
}

fn logic_analyzer_active_at(sessions: usize, last_activity: Option<Instant>, now: Instant) -> bool {
    sessions > 0
        || last_activity.is_some_and(|at| now.saturating_duration_since(at) < LOGIC_ACTIVITY_LATCH)
}

impl Drop for LogicAnalyzerSession {
    fn drop(&mut self) {
        self.activity.mark_logic_activity();
        self.activity
            .logic_analyzer_sessions
            .fetch_sub(1, Ordering::Relaxed);
    }
}

#[cfg(test)]
mod tests {
    use super::{logic_analyzer_active_at, LOGIC_ACTIVITY_LATCH};
    use std::time::{Duration, Instant};

    #[test]
    fn logic_activity_latch_covers_one_indicator_probe_and_then_expires() {
        let activity_at = Instant::now();

        assert!(logic_analyzer_active_at(
            0,
            Some(activity_at),
            activity_at + LOGIC_ACTIVITY_LATCH - Duration::from_millis(1),
        ));
        assert!(!logic_analyzer_active_at(
            0,
            Some(activity_at),
            activity_at + LOGIC_ACTIVITY_LATCH,
        ));
        assert!(logic_analyzer_active_at(
            1,
            Some(activity_at),
            activity_at + LOGIC_ACTIVITY_LATCH,
        ));
    }
}
