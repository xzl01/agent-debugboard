use super::events::handle_key;
use super::gpio_fixture::current_power_outputs;
use super::model::TuiModel;
use crate::client::DEFAULT_BASE_URL;
use crate::ws_status::{TuiStatusSwitchInfo, WsStatusSnapshot};
use crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
use std::time::{Duration, Instant};

pub(super) fn press(model: &mut TuiModel, code: KeyCode) {
    handle_key(
        model,
        KeyEvent::new(code, KeyModifiers::NONE),
        Instant::now(),
    )
    .unwrap();
}

pub(super) fn model_with_switch() -> TuiModel {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.width = 120;
    let mut snapshot = WsStatusSnapshot {
        power_outputs: current_power_outputs(),
        ..Default::default()
    };
    snapshot.switches.insert(
        "sd".to_string(),
        TuiStatusSwitchInfo {
            route: "target".to_string(),
            routes: vec!["target".to_string(), "usb-reader".to_string()],
            ..Default::default()
        },
    );
    model.apply_status_snapshot(snapshot);
    model
}
