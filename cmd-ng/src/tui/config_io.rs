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
    request: ConfigRequest,
    handle: JoinHandle<()>,
}

pub(super) struct ConfigWorker {
    sender: Sender<ConfigJobResult>,
    receiver: Receiver<ConfigJobResult>,
    active: Option<ActiveJob>,
}

impl ConfigWorker {
    pub(super) fn new() -> Self {
        let (sender, receiver) = mpsc::channel();
        Self {
            sender,
            receiver,
            active: None,
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
        let sender = self.sender.clone();
        let worker_request = request.clone();
        let handle = thread::spawn(move || {
            let result = match BoardClient::new(&base_url, timeout) {
                Ok(client) => execute_request(&client, worker_request),
                Err(error) => ConfigJobResult::transport(worker_request, error.to_string()),
            };
            drop(sender.send(result));
        });
        self.active = Some(ActiveJob { request, handle });
        true
    }

    pub(super) fn poll(&mut self) -> Option<ConfigJobResult> {
        match self.receiver.try_recv() {
            Ok(result) => {
                let request = result.request.clone();
                if self.join_active().is_err() {
                    return Some(ConfigJobResult::transport(
                        request,
                        "config worker stopped".to_string(),
                    ));
                }
                Some(result)
            }
            Err(TryRecvError::Empty) => {
                if self
                    .active
                    .as_ref()
                    .is_some_and(|active| active.handle.is_finished())
                {
                    let request = self.active.as_ref().map(|active| active.request.clone())?;
                    let _ = self.join_active();
                    return Some(ConfigJobResult::transport(
                        request,
                        "config worker stopped".to_string(),
                    ));
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
}

impl Drop for ConfigWorker {
    fn drop(&mut self) {
        let _ = self.join_active();
    }
}
