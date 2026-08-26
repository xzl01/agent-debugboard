use super::confirm::{ConfirmableCommand, HardwareConfirmation};
use super::controls::{control_items, ControlItem};
use super::gpio_fixture::current_power_outputs;
use super::hit_types::{HardwareModalTarget, SavedConfigModalTarget};
use super::model::{TuiModel, TuiSwitchState};
use super::pages::ActivePage;
use crate::persistent_config::{ConfigAction, PersistentConfigResponse, PersistentConfigStatus};
use crate::ws_status::{TuiStatusGpio, WsStatusSnapshot};
use anyhow::{anyhow, Result};
use crossterm::event::{
    Event, KeyCode, KeyEvent, KeyModifiers, MouseButton, MouseEvent, MouseEventKind,
};
use ratatui::layout::Rect;
use std::time::{Duration, Instant};

pub(super) const SWITCH_NAME: &str = "tf_wp";
pub(super) const SHOW: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":1},"pending":0,"items":[{"id":"power/alpha","kind":"power","current":{"state":"off"},"saved":{"state":"on"},"selected":true,"requires_confirm":true,"apply_state":"applied"},{"id":"switch/beta","kind":"switch","current":{"route":"pc"},"saved":{"route":"target"},"selected":false,"requires_confirm":false,"apply_state":"applied"}]}"#;
pub(super) const SAVE: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"save","saved_items":["power/alpha"],"confirmation_items":["power/alpha"],"applied_items":["power/alpha"],"snapshot":{"present":true,"version":1},"pending":0}"#;

pub(super) fn key(code: KeyCode) -> Event {
    Event::Key(KeyEvent::new(code, KeyModifiers::NONE))
}

pub(super) fn left_down(rect: Rect) -> Event {
    Event::Mouse(MouseEvent {
        kind: MouseEventKind::Down(MouseButton::Left),
        column: rect.x,
        row: rect.y,
        modifiers: KeyModifiers::NONE,
    })
}

pub(super) fn switch_model(base_url: String) -> Result<TuiModel> {
    let mut model = super::mouse_fixture::model();
    model.base_url = base_url;
    model.switches.insert(
        SWITCH_NAME.to_string(),
        TuiSwitchState {
            name: SWITCH_NAME.to_string(),
            desired_route: "writable".to_string(),
            actual_route: "writable".to_string(),
            routes: vec!["writable".to_string(), "protected".to_string()],
            ..Default::default()
        },
    );
    model.control_idx = item_index(&model, &ControlItem::Switch(SWITCH_NAME.to_string()))?;
    Ok(model)
}

pub(super) fn confirmed_switch_model(base_url: String, start: Instant) -> Result<TuiModel> {
    let mut model = switch_model(base_url)?;
    model.hardware_confirm = Some(HardwareConfirmation {
        command: ConfirmableCommand::RouteSwitch {
            name: SWITCH_NAME.to_string(),
            route: "protected".to_string(),
        },
        started: start,
    });
    Ok(model)
}

pub(super) fn saved_config_model(base_url: String) -> Result<TuiModel> {
    let response = PersistentConfigResponse::from_raw(SHOW.to_string())?;
    response.validate(&ConfigAction::Get, None)?;
    let mut model = TuiModel::new(base_url, Duration::from_secs(1));
    model.saved_config.observe_summary(Some(config_summary()));
    model
        .saved_config
        .apply_authoritative(response)
        .map_err(anyhow::Error::msg)?;
    model.set_page(ActivePage::SavedConfig);
    Ok(model)
}

pub(super) fn error_model(page: ActivePage, selected: ControlItem) -> Result<TuiModel> {
    let mut snapshot = WsStatusSnapshot {
        power_outputs: current_power_outputs(),
        config: Some(config_summary()),
        gpios: vec![TuiStatusGpio {
            name: "GP13".to_string(),
            pin: 13,
            value: Some(0),
            direction: "input".to_string(),
            ..Default::default()
        }],
        ..Default::default()
    };
    snapshot.switches.insert(
        SWITCH_NAME.to_string(),
        crate::ws_status::TuiStatusSwitchInfo {
            route: "writable".to_string(),
            routes: vec!["writable".to_string(), "protected".to_string()],
            ..Default::default()
        },
    );
    let mut model = TuiModel::new("http://127.0.0.1:9".to_string(), Duration::from_millis(30));
    model.apply_status_snapshot(snapshot);
    let response = PersistentConfigResponse::from_raw(SHOW.to_string())?;
    response.validate(&ConfigAction::Get, None)?;
    model
        .saved_config
        .apply_authoritative(response)
        .map_err(anyhow::Error::msg)?;
    model.set_page(page);
    model.control_idx = item_index(&model, &selected)?;
    model.saved_config.error = Some("storage_error".to_string());
    Ok(model)
}

pub(super) fn hardware_button(model: &TuiModel) -> Result<Rect> {
    model
        .hit_map
        .hardware_modal
        .iter()
        .find_map(|(rect, target)| (*target == HardwareModalTarget::confirm()).then_some(*rect))
        .ok_or_else(|| anyhow!("missing hardware confirmation button"))
}

pub(super) fn saved_config_button(model: &TuiModel) -> Result<Rect> {
    model
        .hit_map
        .saved_config_modal
        .iter()
        .find_map(|(rect, target)| (*target == SavedConfigModalTarget::confirm()).then_some(*rect))
        .ok_or_else(|| anyhow!("missing Saved Config confirmation button"))
}

fn config_summary() -> PersistentConfigStatus {
    PersistentConfigStatus {
        available: true,
        reason: "ready".to_string(),
        saved_count: 1,
        pending_count: 0,
    }
}

fn item_index(model: &TuiModel, selected: &ControlItem) -> Result<usize> {
    control_items(model)
        .iter()
        .position(|item| item == selected)
        .ok_or_else(|| anyhow!("missing control item {selected:?}"))
}
