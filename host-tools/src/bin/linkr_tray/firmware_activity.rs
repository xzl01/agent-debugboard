use std::{net::SocketAddr, time::Duration};

use reqwest::Client;
use serde::Deserialize;
use tao::event_loop::EventLoopProxy;
use tokio::time::MissedTickBehavior;
use url::Url;

use super::{
    icon::IndicatorState, UserEvent, BOARD_STATUS_INTERVAL, HEALTH_TIMEOUT, HOST_ACTIVITY_INTERVAL,
};

const MAIN_RAILS: [&str; 3] = ["5v_out", "12v_out", "20v_out"];

#[derive(Debug, PartialEq, Eq)]
pub(super) struct IndicatorSnapshot {
    indicator: IndicatorState,
}

impl IndicatorSnapshot {
    pub(super) const fn indicator(&self) -> IndicatorState {
        self.indicator
    }
}

#[derive(Debug, PartialEq, Eq)]
pub(super) struct HostIndicatorSnapshot {
    uart_bridge_active: bool,
    logic_analyzer_active: bool,
}

impl HostIndicatorSnapshot {
    pub(super) const fn new(uart_bridge_active: bool, logic_analyzer_active: bool) -> Self {
        Self {
            uart_bridge_active,
            logic_analyzer_active,
        }
    }
}

pub(super) fn complete_probe(sample: Option<IndicatorSnapshot>) -> IndicatorState {
    sample.map_or_else(IndicatorState::default, |snapshot| snapshot.indicator())
}

pub(super) fn complete_host_activity_probe(
    indicator: IndicatorState,
    sample: Option<HostIndicatorSnapshot>,
) -> IndicatorState {
    let (uart_bridge_active, logic_analyzer_active) = sample.map_or((false, false), |sample| {
        (sample.uart_bridge_active, sample.logic_analyzer_active)
    });
    indicator.with_host_activity(uart_bridge_active, logic_analyzer_active)
}

#[derive(Debug, Deserialize, PartialEq, Eq)]
struct PowerState {
    name: String,
    state: String,
    value: Option<i64>,
}

#[derive(Deserialize)]
struct StatusResponse {
    ok: bool,
    power_outputs: Vec<PowerState>,
}

#[derive(Deserialize)]
struct HostStatusResponse {
    ok: bool,
    serial_logging: SerialLoggingStatus,
    activity: HostActivityStatus,
}

#[derive(Deserialize)]
struct SerialLoggingStatus {
    active_sessions: usize,
}

#[derive(Deserialize)]
struct HostActivityStatus {
    logic_analyzer_active: bool,
}

fn probe_client(timeout: Duration) -> Option<Client> {
    Client::builder()
        .pool_max_idle_per_host(1)
        .connect_timeout(timeout)
        .timeout(timeout)
        .build()
        .ok()
}

async fn fetch_host_activity(client: &Client, url: &Url) -> Option<HostIndicatorSnapshot> {
    let host = client
        .get(url.clone())
        .send()
        .await
        .ok()?
        .error_for_status()
        .ok()?
        .json::<HostStatusResponse>()
        .await
        .ok()?;
    if !host.ok {
        return None;
    }
    Some(HostIndicatorSnapshot::new(
        host.serial_logging.active_sessions > 0,
        host.activity.logic_analyzer_active,
    ))
}

async fn fetch_firmware(client: &Client, url: &Url) -> Option<IndicatorSnapshot> {
    let status = client
        .get(url.clone())
        .send()
        .await
        .ok()?
        .error_for_status()
        .ok()?
        .json::<StatusResponse>()
        .await
        .ok()?;
    if !status.ok {
        return None;
    }
    let rails = MAIN_RAILS.map(|name| {
        status
            .power_outputs
            .iter()
            .find(|output| output.name == name)
            .is_some_and(|output| output.state == "on" || output.value == Some(1))
    });
    Some(IndicatorSnapshot {
        indicator: IndicatorState::new(rails, false, false),
    })
}

pub(super) async fn run_indicator_probes(
    proxy: EventLoopProxy<UserEvent>,
    board_url: Url,
    host_address: SocketAddr,
) {
    let Some(client) = probe_client(HEALTH_TIMEOUT) else {
        return;
    };
    let Ok(firmware_url) = board_url.join("api/v1/status") else {
        return;
    };
    let Ok(host_url) = Url::parse(&format!("http://{host_address}/host/api/v1/status")) else {
        return;
    };
    let mut interval = tokio::time::interval(BOARD_STATUS_INTERVAL.min(HOST_ACTIVITY_INTERVAL));
    interval.set_missed_tick_behavior(MissedTickBehavior::Skip);
    loop {
        interval.tick().await;
        let (firmware, host) = tokio::join!(
            fetch_firmware(&client, &firmware_url),
            fetch_host_activity(&client, &host_url)
        );
        if proxy
            .send_event(UserEvent::IndicatorProbe { firmware, host })
            .is_err()
        {
            return;
        }
    }
}

#[cfg(test)]
pub(super) fn probe_host_activity(
    address: SocketAddr,
    timeout: Duration,
) -> Option<HostIndicatorSnapshot> {
    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .ok()?;
    let client = probe_client(timeout)?;
    let url = Url::parse(&format!("http://{address}/host/api/v1/status")).ok()?;
    runtime.block_on(fetch_host_activity(&client, &url))
}

#[cfg(test)]
pub(super) fn probe_firmware(board_url: &Url, timeout: Duration) -> Option<IndicatorSnapshot> {
    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .ok()?;
    let client = probe_client(timeout)?;
    let status_url = board_url.join("api/v1/status").ok()?;
    runtime.block_on(fetch_firmware(&client, &status_url))
}

#[cfg(test)]
#[path = "firmware_activity_test.rs"]
mod tests;
