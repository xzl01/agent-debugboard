use super::TuiActionMsg;
use crate::client::{BoardRequest, BoardTransport};
use crate::ws_status::TuiPdStatus;
use anyhow::Result;
use reqwest::{Method, Url};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

const PD_RETRY_INTERVAL: Duration = Duration::from_secs(5);

struct PdRead {
    generation: u64,
    handle: JoinHandle<Result<TuiPdStatus, String>>,
}

#[derive(Default)]
pub(super) struct PdWorker {
    active: Option<PdRead>,
    generation: u64,
    retry_after: Option<Instant>,
}

impl PdWorker {
    pub(super) fn start(&mut self, base_url: String, timeout: Duration, now: Instant) -> bool {
        if self.active.is_some() || self.retry_after.is_some_and(|deadline| now < deadline) {
            return false;
        }
        let generation = self.generation;
        self.active = Some(PdRead {
            generation,
            handle: thread::spawn(move || {
                read_pd(&base_url, timeout).map_err(|error| error.to_string())
            }),
        });
        true
    }

    pub(super) fn poll(&mut self, now: Instant) -> Option<Result<TuiPdStatus, String>> {
        if !self.active.as_ref()?.handle.is_finished() {
            return None;
        }
        let active = self.active.take()?;
        let result = active
            .handle
            .join()
            .unwrap_or_else(|_| Err("PD worker stopped".to_string()));
        // Disconnect invalidates the result, but keeps the request in flight
        // until completion so a quick replug cannot start a duplicate read.
        if active.generation != self.generation {
            return None;
        }
        self.retry_after = Some(now + PD_RETRY_INTERVAL);
        Some(result)
    }

    pub(super) fn reset(&mut self) {
        self.generation = self.generation.wrapping_add(1);
        self.retry_after = None;
    }

    #[cfg(test)]
    pub(super) fn active_finished(&self) -> bool {
        self.active
            .as_ref()
            .is_some_and(|read| read.handle.is_finished())
    }
}

impl Drop for PdWorker {
    fn drop(&mut self) {
        if let Some(active) = self.active.take() {
            let _ = active.handle.join();
        }
    }
}

pub(super) fn read_pd(base_url: &str, timeout: Duration) -> Result<TuiPdStatus> {
    let client = crate::client::BoardClient::new(base_url, timeout)?;
    let data = client.send_text(BoardRequest {
        method: Method::GET,
        path: "/api/v1/pd".to_string(),
        query: vec![],
        body: None,
    })?;
    Ok(serde_json::from_str(&data)?)
}

fn board_path(kind: &str, name: &str) -> Result<String> {
    if matches!(name, "." | "..") {
        anyhow::bail!("invalid {kind} name {name:?}: dot path segments are not allowed");
    }

    let mut url = Url::parse("http://localhost/api/v1")?;
    {
        let mut segments = url
            .path_segments_mut()
            .map_err(|()| anyhow::anyhow!("build board I/O request path"))?;
        segments.push(kind).push(name);
    }
    Ok(url.path().to_string())
}

pub(super) fn perform_control_action(
    base_url: &str,
    timeout: Duration,
    output: &str,
    current_state: bool,
) -> Result<TuiActionMsg> {
    let next_state = if current_state { "off" } else { "on" };
    let path = board_path("power", output)?;
    let client = crate::client::BoardClient::new(base_url, timeout)?;
    client.send_text(BoardRequest {
        method: Method::PUT,
        path,
        query: vec![],
        body: Some(serde_json::json!({ "state": next_state })),
    })?;
    Ok(TuiActionMsg {
        status: format!("power {output}={next_state}"),
        err: None,
    })
}

pub(super) fn set_switch_route(
    base_url: &str,
    timeout: Duration,
    name: &str,
    route: &str,
) -> Result<TuiActionMsg> {
    let path = board_path("switch", name)?;
    let client = crate::client::BoardClient::new(base_url, timeout)?;
    client.send_text(BoardRequest {
        method: Method::PUT,
        path,
        query: vec![],
        body: Some(serde_json::json!({ "route": route })),
    })?;
    Ok(TuiActionMsg {
        status: format!("switch {name}={route}"),
        err: None,
    })
}

pub(super) fn set_pd_fixed(
    base_url: &str,
    timeout: Duration,
    voltage_mv: u16,
) -> Result<TuiActionMsg> {
    let client = crate::client::BoardClient::new(base_url, timeout)?;
    client.send_text(BoardRequest {
        method: Method::PUT,
        path: "/api/v1/pd".to_string(),
        query: vec![],
        body: Some(serde_json::json!({ "mode": 0, "voltage_mv": voltage_mv })),
    })?;
    Ok(TuiActionMsg {
        status: format!("pd fixed={}mV", voltage_mv),
        err: None,
    })
}

pub(super) fn reboot_board(base_url: &str, timeout: Duration) -> Result<TuiActionMsg> {
    let client = crate::client::BoardClient::new(base_url, timeout)?;
    client.send_reboot()?;
    Ok(TuiActionMsg {
        status: "Normal reboot accepted; waiting for reconnect (not ROM BOOTSEL)".to_string(),
        err: None,
    })
}

pub(super) fn set_gpio_output(
    base_url: &str,
    timeout: Duration,
    gpio: &str,
    value: bool,
) -> Result<TuiActionMsg> {
    let path = board_path("gpio", gpio)?;
    let client = crate::client::BoardClient::new(base_url, timeout)?;
    client.send_text(BoardRequest {
        method: Method::PUT,
        path,
        query: vec![],
        body: Some(
            serde_json::json!({ "direction": "output", "value": if value { 1 } else { 0 } }),
        ),
    })?;
    Ok(TuiActionMsg {
        status: format!("gpio {}={}", gpio, if value { 1 } else { 0 }),
        err: None,
    })
}

pub(super) fn set_gpio_input(
    base_url: &str,
    timeout: Duration,
    gpio: &str,
) -> Result<TuiActionMsg> {
    let path = board_path("gpio", gpio)?;
    let client = crate::client::BoardClient::new(base_url, timeout)?;
    client.send_text(BoardRequest {
        method: Method::PUT,
        path,
        query: vec![],
        body: Some(serde_json::json!({ "direction": "input" })),
    })?;
    Ok(TuiActionMsg {
        status: format!("gpio {}=input", gpio),
        err: None,
    })
}
