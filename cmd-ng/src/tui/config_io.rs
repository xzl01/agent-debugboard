use super::config_result::ConfigJobResult;
use super::config_state::ConfigRequest;
use crate::client::{BoardClient, BoardTransport};
use std::sync::mpsc::{self, Receiver, Sender, TryRecvError};
use std::thread::{self, JoinHandle};
use std::time::Duration;

pub(super) fn execute_request<TClient>(client: &TClient, request: ConfigRequest) -> ConfigJobResult
where
    TClient: BoardTransport,
{
    let mutation = match &request {
        ConfigRequest::Refresh => {
            return ConfigJobResult::refresh(
                client.config_show().map_err(|error| error.to_string()),
            );
        }
        ConfigRequest::Save { items, confirm } => client.config_save(items, *confirm),
        ConfigRequest::Clear => client.config_clear(),
    }
    .map_err(|error| error.to_string());
    let refresh = client.config_show().map_err(|error| error.to_string());
    ConfigJobResult::mutation(request, mutation, refresh)
}

struct ActiveJob {
    generation: u64,
    request: ConfigRequest,
    handle: JoinHandle<()>,
}

struct QueuedResult {
    generation: u64,
    result: ConfigJobResult,
}

#[cfg(test)]
type EmptyHook = Box<dyn Fn(&ConfigWorker) + Send>;

pub(super) struct ConfigWorker {
    sender: Sender<QueuedResult>,
    receiver: Receiver<QueuedResult>,
    active: Option<ActiveJob>,
    next_generation: u64,
    #[cfg(test)]
    pub(super) empty_hook: Option<EmptyHook>,
    #[cfg(test)]
    queued_for_test: Option<ConfigJobResult>,
}

impl ConfigWorker {
    pub(super) fn new() -> Self {
        let (sender, receiver) = mpsc::channel();
        Self {
            sender,
            receiver,
            active: None,
            next_generation: 0,
            #[cfg(test)]
            empty_hook: None,
            #[cfg(test)]
            queued_for_test: None,
        }
    }

    pub(super) fn start(
        &mut self,
        base_url: String,
        timeout: Duration,
        request: ConfigRequest,
    ) -> bool {
        if self.active.is_some() {
            return false;
        }
        let Some(generation) = self.next_generation.checked_add(1) else {
            return false;
        };
        self.next_generation = generation;
        let sender = self.sender.clone();
        let worker_request = request.clone();
        let handle = thread::spawn(move || {
            let result = match BoardClient::new(&base_url, timeout) {
                Ok(client) => execute_request(&client, worker_request),
                Err(error) => ConfigJobResult::transport(worker_request, error.to_string()),
            };
            let queued = QueuedResult { generation, result };
            drop(sender.send(queued));
        });
        self.active = Some(ActiveJob {
            generation,
            request,
            handle,
        });
        true
    }

    pub(super) fn poll(&mut self) -> Option<ConfigJobResult> {
        #[cfg(test)]
        if let Some(result) = self.queued_for_test.take() {
            return Some(result);
        }
        loop {
            match self.receiver.try_recv() {
                Ok(queued) => {
                    let Some(active) = self.active.as_ref() else {
                        continue;
                    };
                    if queued.generation != active.generation {
                        continue;
                    }
                    let request = queued.result.request.clone();
                    if self.join_active().is_err() {
                        return Some(ConfigJobResult::transport(
                            request,
                            "config worker stopped".to_string(),
                        ));
                    }
                    return Some(queued.result);
                }
                Err(TryRecvError::Empty) => {
                    #[cfg(test)]
                    if let Some(hook) = self.empty_hook.take() {
                        hook(self);
                    }
                    let active = self.active.as_ref()?;
                    if !active.handle.is_finished() {
                        return None;
                    }
                    let generation = active.generation;
                    let request = active.request.clone();
                    if self.join_active().is_err() {
                        return Some(ConfigJobResult::transport(
                            request,
                            "config worker stopped".to_string(),
                        ));
                    }
                    loop {
                        match self.receiver.try_recv() {
                            Ok(queued) => {
                                if queued.generation == generation {
                                    return Some(queued.result);
                                }
                            }
                            Err(TryRecvError::Empty | TryRecvError::Disconnected) => {
                                return Some(ConfigJobResult::transport(
                                    request,
                                    "config worker stopped".to_string(),
                                ));
                            }
                        }
                    }
                }
                Err(TryRecvError::Disconnected) => return None,
            }
        }
    }

    #[cfg(test)]
    pub(super) fn queue_for_test(&mut self, result: ConfigJobResult) {
        self.queued_for_test = Some(result);
    }

    #[cfg(test)]
    pub(super) fn active_finished(&self) -> bool {
        self.active
            .as_ref()
            .is_some_and(|active| active.handle.is_finished())
    }

    fn join_active(&mut self) -> Result<(), ()> {
        match self.active.take() {
            Some(active) => active.handle.join().map_err(|_| ()),
            None => Ok(()),
        }
    }
}

impl Drop for ConfigWorker {
    fn drop(&mut self) {
        let _ = self.join_active();
    }
}
