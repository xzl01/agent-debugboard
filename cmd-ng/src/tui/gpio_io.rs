use super::board_io::{set_gpio_input, set_gpio_output};
use super::TuiActionMsg;
use std::sync::mpsc::{self, Receiver, Sender, TryRecvError};
use std::thread::{self, JoinHandle};
use std::time::Duration;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum GpioAction {
    DriveLow,
    DriveHigh,
    SetInput,
}

impl GpioAction {
    pub(super) fn status_text(self, target: &str) -> String {
        match self {
            Self::DriveLow => format!("gpio {target}=0"),
            Self::DriveHigh => format!("gpio {target}=1"),
            Self::SetInput => format!("gpio {target}=input"),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct GpioJob {
    pub(super) action: GpioAction,
    pub(super) target: String,
}

#[derive(Debug)]
pub(super) struct GpioJobResult {
    pub(super) outcome: Result<TuiActionMsg, String>,
}

struct ActiveJob {
    handle: JoinHandle<()>,
}

#[cfg(test)]
type EmptyHook = Box<dyn Fn(&GpioWorker) + Send>;

pub(super) struct GpioWorker {
    sender: Sender<GpioJobResult>,
    receiver: Receiver<GpioJobResult>,
    active: Option<ActiveJob>,
    #[cfg(test)]
    pub(super) empty_hook: Option<EmptyHook>,
}

impl GpioWorker {
    pub(super) fn new() -> Self {
        let (sender, receiver) = mpsc::channel();
        Self {
            sender,
            receiver,
            active: None,
            #[cfg(test)]
            empty_hook: None,
        }
    }

    pub(super) fn start(&mut self, base_url: String, timeout: Duration, job: GpioJob) -> bool {
        if self.active.is_some() {
            return false;
        }
        let sender = self.sender.clone();
        let handle = thread::spawn(move || {
            let outcome = execute_job(&base_url, timeout, &job);
            drop(sender.send(GpioJobResult { outcome }));
        });
        self.active = Some(ActiveJob { handle });
        true
    }

    pub(super) fn poll(&mut self) -> Option<GpioJobResult> {
        match self.receiver.try_recv() {
            Ok(result) => {
                if self.join_active().is_err() {
                    return Some(GpioJobResult {
                        outcome: Err("gpio worker stopped".to_string()),
                    });
                }
                Some(result)
            }
            Err(TryRecvError::Empty) => {
                #[cfg(test)]
                if let Some(hook) = self.empty_hook.take() {
                    hook(self);
                }
                if self
                    .active
                    .as_ref()
                    .is_some_and(|active| active.handle.is_finished())
                {
                    if self.join_active().is_err() {
                        return Some(GpioJobResult {
                            outcome: Err("gpio worker stopped".to_string()),
                        });
                    }
                    // The worker may have queued its result between the first
                    // try_recv and the finished check; drain it exactly once so
                    // no stale result can leak into the next job.
                    return Some(match self.receiver.try_recv() {
                        Ok(result) => result,
                        Err(_) => GpioJobResult {
                            outcome: Err("gpio worker stopped".to_string()),
                        },
                    });
                }
                None
            }
            Err(TryRecvError::Disconnected) => None,
        }
    }

    fn join_active(&mut self) -> Result<(), ()> {
        match self.active.take() {
            Some(active) => active.handle.join().map_err(|_| ()),
            None => Ok(()),
        }
    }

    #[cfg(test)]
    pub(super) fn active_finished(&self) -> bool {
        self.active
            .as_ref()
            .is_some_and(|active| active.handle.is_finished())
    }
}

impl Drop for GpioWorker {
    fn drop(&mut self) {
        let _ = self.join_active();
    }
}

fn execute_job(base_url: &str, timeout: Duration, job: &GpioJob) -> Result<TuiActionMsg, String> {
    let result = match job.action {
        GpioAction::DriveLow => set_gpio_output(base_url, timeout, &job.target, false),
        GpioAction::DriveHigh => set_gpio_output(base_url, timeout, &job.target, true),
        GpioAction::SetInput => set_gpio_input(base_url, timeout, &job.target),
    };
    result.map_err(|error| format!("{} failed: {error}", job.action.status_text(&job.target)))
}
