use super::config_result::ConfigJobKind;
use super::confirm::{ConfirmableCommand, HardwareConfirmation};
use super::controls::control_items;
use super::hit_types::{HardwareModalTarget, SavedConfigModalTarget, TabTarget};
use super::mock_board::{mock_server, Reply};
use super::model::{TuiModel, TuiSwitchState};
use super::mouse_fixture::{draw_sized, model as controls_model};
use super::pages::ActivePage;
use super::runtime::{drain_ready_events, EventDisposition};
use crate::persistent_config::{ConfigAction, PersistentConfigResponse, PersistentConfigStatus};
use anyhow::{anyhow, Result};
use crossterm::event::{Event, KeyModifiers, MouseButton, MouseEvent, MouseEventKind};
use ratatui::layout::Rect;
use std::collections::VecDeque;
use std::sync::{Arc, Barrier};
use std::time::{Duration, Instant};

const SWITCH_NAME: &str = "tf_wp";
const SHOW: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":1},"pending":0,"items":[{"id":"power/alpha","kind":"power","current":{"state":"off"},"saved":{"state":"on"},"selected":true,"requires_confirm":true,"apply_state":"applied"},{"id":"switch/beta","kind":"switch","current":{"route":"pc"},"saved":{"route":"target"},"selected":false,"requires_confirm":false,"apply_state":"applied"}]}"#;
const SAVE: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"save","saved_items":["power/alpha"],"confirmation_items":["power/alpha"],"applied_items":["power/alpha"],"snapshot":{"present":true,"version":1},"pending":0}"#;

fn left_down(rect: Rect) -> Event {
    Event::Mouse(MouseEvent {
        kind: MouseEventKind::Down(MouseButton::Left),
        column: rect.x,
        row: rect.y,
        modifiers: KeyModifiers::NONE,
    })
}

fn saved_config_model(base_url: String) -> Result<TuiModel> {
    let response = PersistentConfigResponse::from_raw(SHOW.to_string())?;
    response.validate(&ConfigAction::Get, None)?;
    let mut model = TuiModel::new(base_url, Duration::from_secs(1));
    model
        .saved_config
        .observe_summary(Some(PersistentConfigStatus {
            available: true,
            reason: "ready".to_string(),
            saved_count: 1,
            pending_count: 0,
        }));
    model
        .saved_config
        .apply_authoritative(response)
        .map_err(anyhow::Error::msg)?;
    model.set_page(ActivePage::SavedConfig);
    Ok(model)
}

fn hardware_button(model: &TuiModel, target: HardwareModalTarget) -> Result<Rect> {
    model
        .hit_map
        .hardware_modal
        .iter()
        .find_map(|(rect, candidate)| (*candidate == target).then_some(*rect))
        .ok_or_else(|| anyhow!("missing hardware modal button"))
}

fn saved_config_button(model: &TuiModel, target: SavedConfigModalTarget) -> Result<Rect> {
    model
        .hit_map
        .saved_config_modal
        .iter()
        .find_map(|(rect, candidate)| (*candidate == target).then_some(*rect))
        .ok_or_else(|| anyhow!("missing Saved Config modal button"))
}

#[test]
fn hardware_confirm_at_80x8_defers_second_down_over_stale_control_row() -> Result<()> {
    // Given: a compact hardware modal whose Confirm button covers another control row.
    let start = Instant::now();
    let (url, requests) = mock_server(vec![Reply::Http(200, "{}")]);
    let mut model = controls_model();
    model.base_url = url;
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
    model.hardware_confirm = Some(HardwareConfirmation {
        command: ConfirmableCommand::RouteSwitch {
            name: SWITCH_NAME.to_string(),
            route: "protected".to_string(),
        },
        started: start,
    });
    draw_sized(&mut model, 80, 8)?;
    let confirm = hardware_button(&model, HardwareModalTarget::confirm())?;
    let covered = model
        .hit_map
        .controls
        .at(confirm.x, confirm.y)
        .cloned()
        .ok_or_else(|| anyhow!("hardware button must cover a control row at 80x8"))?;
    assert_eq!(
        model
            .hit_map
            .controls
            .iter()
            .find(|(_, item)| **item == covered)
            .map(|(rect, _)| rect.width),
        Some(80)
    );
    assert_ne!(covered, control_items(&model)[model.control_idx]);
    let mut events = VecDeque::from([
        (left_down(confirm), start + Duration::from_millis(1)),
        (left_down(confirm), start + Duration::from_millis(2)),
    ]);

    // When: both Downs are ready in one drain pass.
    let disposition = drain_ready_events(&mut model, || Ok(events.pop_front()))?;
    let request = requests.recv_timeout(Duration::from_secs(1))?;

    // Then: only the confirmation runs; stale geometry cannot select or reactivate a row.
    assert_eq!(disposition, EventDisposition::Redraw);
    assert_eq!(events.len(), 1);
    assert!(model.hardware_confirm.is_none());
    assert_eq!(model.control_idx, 0);
    assert!(request.starts_with("PUT /api/v1/switch/tf_wp HTTP/1.1"));
    assert!(request.contains(r#"{"route":"protected"}"#));
    assert!(requests.recv_timeout(Duration::from_millis(30)).is_err());
    Ok(())
}

#[test]
fn saved_config_confirm_at_80x8_defers_second_down_over_stale_item_row() -> Result<()> {
    // Given: a compact Saved Config modal whose Confirm button covers a full-width item row.
    let start = Instant::now();
    let gate = Arc::new(Barrier::new(2));
    let (url, requests) = mock_server(vec![
        Reply::Gated(200, SAVE, gate.clone()),
        Reply::Http(200, SHOW),
    ]);
    let mut model = saved_config_model(url)?;
    assert!(model.saved_config.request_save().is_none());
    draw_sized(&mut model, 80, 8)?;
    let confirm = saved_config_button(&model, SavedConfigModalTarget::confirm())?;
    let covered = model
        .hit_map
        .saved_config_rows
        .at(confirm.x, confirm.y)
        .cloned()
        .ok_or_else(|| anyhow!("Saved Config button must cover an item row at 80x8"))?;
    assert_eq!(
        model
            .hit_map
            .saved_config_rows
            .iter()
            .find(|(_, target)| **target == covered)
            .map(|(rect, _)| rect.width),
        Some(80)
    );
    let selected_before = model.saved_config.selected_ids();
    let mut events = VecDeque::from([
        (left_down(confirm), start + Duration::from_millis(1)),
        (left_down(confirm), start + Duration::from_millis(2)),
    ]);

    // When: both Downs are ready in one drain pass.
    let disposition = drain_ready_events(&mut model, || Ok(events.pop_front()))?;
    let mutation = requests.recv_timeout(Duration::from_secs(1))?;
    gate.wait();
    let refresh = requests.recv_timeout(Duration::from_secs(1))?;

    // Then: one confirmed save starts and the stale row click remains queued.
    assert_eq!(disposition, EventDisposition::Redraw);
    assert_eq!(events.len(), 1);
    assert!(model.saved_config.confirmation().is_none());
    assert_eq!(model.saved_config.busy, Some(ConfigJobKind::Save));
    assert_eq!(model.saved_config.selected_ids(), selected_before);
    assert!(mutation.starts_with("PUT /api/v1/config HTTP/1.1"));
    assert!(mutation.contains(r#"{"confirm":true,"items":["power/alpha"]}"#));
    assert!(refresh.starts_with("GET /api/v1/config HTTP/1.1"));
    assert!(requests.recv_timeout(Duration::from_millis(30)).is_err());
    Ok(())
}

#[test]
fn actual_page_switch_defers_the_next_ready_down() -> Result<()> {
    // Given: two Downs target the current Saved Config tab geometry.
    let start = Instant::now();
    let mut model = controls_model();
    draw_sized(&mut model, 80, 8)?;
    let tab = model
        .hit_map
        .tabs
        .iter()
        .find_map(|(rect, target)| (*target == TabTarget(ActivePage::SavedConfig)).then_some(*rect))
        .ok_or_else(|| anyhow!("missing Saved Config tab"))?;
    let mut events = VecDeque::from([
        (left_down(tab), start),
        (left_down(tab), start + Duration::from_millis(1)),
    ]);

    // When: the first Down changes the active page.
    let disposition = drain_ready_events(&mut model, || Ok(events.pop_front()))?;

    // Then: the old tab geometry is not reused for the second Down.
    assert_eq!(disposition, EventDisposition::Redraw);
    assert_eq!(events.len(), 1);
    assert_eq!(model.active_page, ActivePage::SavedConfig);
    Ok(())
}

#[test]
fn successful_saved_config_row_toggle_defers_the_next_ready_down() -> Result<()> {
    // Given: two Downs target the same Saved Config row.
    let start = Instant::now();
    let mut model = saved_config_model("http://127.0.0.1:9".to_string())?;
    draw_sized(&mut model, 80, 8)?;
    let row = model
        .hit_map
        .saved_config_rows
        .iter()
        .find_map(|(rect, target)| (target.0.as_str() == "switch/beta").then_some(*rect))
        .ok_or_else(|| anyhow!("missing switch/beta row"))?;
    let mut events = VecDeque::from([
        (left_down(row), start),
        (left_down(row), start + Duration::from_millis(1)),
    ]);

    // When: the first Down focuses and toggles the row.
    let disposition = drain_ready_events(&mut model, || Ok(events.pop_front()))?;

    // Then: redraw occurs before a stale second Down can toggle it back.
    assert_eq!(disposition, EventDisposition::Redraw);
    assert_eq!(events.len(), 1);
    assert_eq!(
        model.saved_config.selected_ids(),
        ["power/alpha", "switch/beta"]
    );
    assert_eq!(model.saved_config.cursor, 1);
    Ok(())
}

#[test]
fn active_tab_down_is_inert_and_does_not_stop_ready_drain() -> Result<()> {
    // Given: two Downs target the already-active Controls tab.
    let start = Instant::now();
    let mut model = controls_model();
    draw_sized(&mut model, 80, 8)?;
    let tab = model
        .hit_map
        .tabs
        .iter()
        .find_map(|(rect, target)| (*target == TabTarget(ActivePage::Controls)).then_some(*rect))
        .ok_or_else(|| anyhow!("missing Controls tab"))?;
    let mut events = VecDeque::from([
        (left_down(tab), start),
        (left_down(tab), start + Duration::from_millis(1)),
    ]);

    // When: the ready queue is drained.
    let disposition = drain_ready_events(&mut model, || Ok(events.pop_front()))?;

    // Then: covered but inert input remains Continue and both events are consumed.
    assert_eq!(disposition, EventDisposition::Continue);
    assert!(events.is_empty());
    assert_eq!(model.active_page, ActivePage::Controls);
    Ok(())
}
