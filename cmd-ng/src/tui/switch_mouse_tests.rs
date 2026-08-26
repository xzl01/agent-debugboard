use super::actions::{confirm_hardware, ControlIntent};
use super::confirm::{ConfirmableCommand, CONFIRM_TIMEOUT};
use super::controls::{control_items, ControlItem};
use super::hit_types::HardwareModalTarget;
use super::mock_board::{mock_server, Reply};
use super::model::TuiModel;
use super::mouse_events::handle_mouse_at;
use super::mouse_fixture::{control_rect, draw_sized};
use crate::ws_status::{TuiStatusSwitchInfo, WsStatusSnapshot};
use anyhow::{anyhow, Result};
use crossterm::event::{KeyModifiers, MouseButton, MouseEvent, MouseEventKind};
use ratatui::layout::Rect;
use std::time::{Duration, Instant};

const SWITCH_NAME: &str = "tf_wp";

fn switch_model(base_url: String) -> TuiModel {
    let mut snapshot = WsStatusSnapshot::default();
    snapshot.switches.insert(
        SWITCH_NAME.to_string(),
        TuiStatusSwitchInfo {
            route: "writable".to_string(),
            routes: vec!["writable".to_string(), "protected".to_string()],
            requires_confirm: false,
        },
    );
    let mut model = TuiModel::new(base_url, Duration::from_secs(1));
    model.apply_status_snapshot(snapshot);
    model
}

fn left_down(rect: Rect) -> MouseEvent {
    MouseEvent {
        kind: MouseEventKind::Down(MouseButton::Left),
        column: rect.x,
        row: rect.y,
        modifiers: KeyModifiers::NONE,
    }
}

fn switch_rect(model: &TuiModel) -> Result<Rect> {
    control_rect(model, &ControlItem::Switch(SWITCH_NAME.to_string()))
        .ok_or_else(|| anyhow!("missing {SWITCH_NAME} row hit"))
}

#[test]
fn click_selects_switch_then_fresh_confirm_routes_exactly_once() -> Result<()> {
    let (url, requests) = mock_server(vec![Reply::Http(200, "{}")]);
    let mut model = switch_model(url);
    draw_sized(&mut model, 80, 24)?;
    let rect = switch_rect(&model)?;

    handle_mouse_at(&mut model, left_down(rect), Instant::now())?;

    let expected_index = control_items(&model)
        .iter()
        .position(|item| item == &ControlItem::Switch(SWITCH_NAME.to_string()))
        .ok_or_else(|| anyhow!("missing {SWITCH_NAME} control item"))?;
    assert_eq!(model.control_idx, expected_index);
    assert_eq!(
        model
            .hardware_confirm
            .as_ref()
            .map(|confirmation| &confirmation.command),
        Some(&ConfirmableCommand::RouteSwitch {
            name: SWITCH_NAME.to_string(),
            route: "protected".to_string(),
        })
    );
    assert!(requests.recv_timeout(Duration::from_millis(30)).is_err());

    confirm_hardware(&mut model, Instant::now())?;

    let request = requests.recv_timeout(Duration::from_secs(1))?;
    assert!(request.starts_with("PUT /api/v1/switch/tf_wp HTTP/1.1"));
    assert!(request.contains(r#"{"route":"protected"}"#));
    assert_eq!(
        model.switches[SWITCH_NAME].pending_route.as_deref(),
        Some("protected")
    );
    assert!(model.switches[SWITCH_NAME].route_intent_active);
    confirm_hardware(&mut model, Instant::now())?;
    assert!(requests.recv_timeout(Duration::from_millis(30)).is_err());
    Ok(())
}

#[test]
fn switch_modal_cancel_routes_zero_times() -> Result<()> {
    let (url, requests) = mock_server(vec![Reply::Http(200, "{}")]);
    let mut model = switch_model(url);
    draw_sized(&mut model, 80, 24)?;
    let switch = switch_rect(&model)?;
    handle_mouse_at(&mut model, left_down(switch), Instant::now())?;
    draw_sized(&mut model, 80, 24)?;
    let cancel = model
        .hit_map
        .hardware_modal
        .iter()
        .find_map(|(rect, target)| (*target == HardwareModalTarget::cancel()).then_some(*rect))
        .ok_or_else(|| anyhow!("missing hardware Cancel button"))?;

    handle_mouse_at(&mut model, left_down(cancel), Instant::now())?;

    assert!(model.hardware_confirm.is_none());
    assert_eq!(model.status, "Switch cancelled");
    assert!(requests.recv_timeout(Duration::from_millis(30)).is_err());
    Ok(())
}

#[test]
fn switch_confirmation_at_timeout_routes_zero_times() -> Result<()> {
    let (url, requests) = mock_server(vec![Reply::Http(200, "{}")]);
    let mut model = switch_model(url);
    draw_sized(&mut model, 80, 24)?;
    let switch = switch_rect(&model)?;
    handle_mouse_at(&mut model, left_down(switch), Instant::now())?;
    let started = model
        .hardware_confirm
        .as_ref()
        .map(|confirmation| confirmation.started)
        .ok_or_else(|| anyhow!("missing switch confirmation"))?;

    confirm_hardware(&mut model, started + CONFIRM_TIMEOUT)?;

    assert!(model.hardware_confirm.is_none());
    assert_eq!(model.status, "Switch confirmation timed out");
    assert!(requests.recv_timeout(Duration::from_millis(30)).is_err());
    assert_eq!(
        super::actions::resolve_activation(
            &model,
            &ControlItem::Switch(SWITCH_NAME.to_string()),
            ControlIntent::Primary,
        ),
        super::actions::Activation::Confirm(ConfirmableCommand::RouteSwitch {
            name: SWITCH_NAME.to_string(),
            route: "protected".to_string(),
        })
    );
    Ok(())
}
