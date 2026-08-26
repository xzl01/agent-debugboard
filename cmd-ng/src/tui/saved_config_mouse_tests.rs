use super::config_result::ConfigJobKind;
use super::hit_types::{SavedConfigModalTarget, SavedConfigRowTarget, TabTarget};
use super::mock_board::{mock_server, Reply};
use super::model::TuiModel;
use super::mouse_events::handle_mouse_at;
use super::mouse_fixture::draw_sized;
use super::pages::ActivePage;
use crate::persistent_config::{
    ConfigAction, ConfigItemId, PersistentConfigResponse, PersistentConfigStatus,
};
use anyhow::{anyhow, Result};
use crossterm::event::{KeyModifiers, MouseButton, MouseEvent, MouseEventKind};
use ratatui::layout::Rect;
use std::sync::{Arc, Barrier};
use std::time::{Duration, Instant};

const SHOW: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":1},"pending":0,"items":[{"id":"power/alpha","kind":"power","current":{"state":"off"},"saved":{"state":"on"},"selected":true,"requires_confirm":true,"apply_state":"applied"},{"id":"switch/beta","kind":"switch","current":{"route":"pc"},"saved":{"route":"target"},"selected":false,"requires_confirm":false,"apply_state":"applied"}]}"#;
const SAVE: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"save","saved_items":["power/alpha"],"confirmation_items":["power/alpha"],"applied_items":["power/alpha"],"snapshot":{"present":true,"version":1},"pending":0}"#;

fn model(base_url: String) -> Result<TuiModel> {
    let response =
        PersistentConfigResponse::from_raw(SHOW.to_string()).map_err(anyhow::Error::msg)?;
    response
        .validate(&ConfigAction::Get, None)
        .map_err(anyhow::Error::msg)?;
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

fn mouse(kind: MouseEventKind, rect: Rect) -> MouseEvent {
    MouseEvent {
        kind,
        column: rect.x,
        row: rect.y,
        modifiers: KeyModifiers::NONE,
    }
}

fn row_rect(model: &TuiModel, id: &str) -> Result<Rect> {
    model
        .hit_map
        .saved_config_rows
        .iter()
        .find(|(_, target)| target == &&SavedConfigRowTarget(ConfigItemId(id.to_string())))
        .map(|(rect, _)| *rect)
        .ok_or_else(|| anyhow!("missing Saved Config row hit for {id}"))
}

fn modal_rect(model: &TuiModel, target: SavedConfigModalTarget) -> Result<Rect> {
    model
        .hit_map
        .saved_config_modal
        .iter()
        .find(|(_, candidate)| **candidate == target)
        .map(|(rect, _)| *rect)
        .ok_or_else(|| anyhow!("missing Saved Config modal hit"))
}

#[test]
fn left_down_re_resolves_stable_id_and_toggles_once() -> Result<()> {
    let mut model = model("http://127.0.0.1:9".to_string())?;
    draw_sized(&mut model, 80, 12)?;
    let alpha = row_rect(&model, "power/alpha")?;
    model.saved_config.items.swap(0, 1);
    model.saved_config.cursor = 0;
    model.saved_config.blur();

    handle_mouse_at(
        &mut model,
        mouse(MouseEventKind::Down(MouseButton::Left), alpha),
        Instant::now(),
    )?;

    assert!(model.saved_config.focused);
    assert_eq!(model.saved_config.cursor, 1);
    assert!(model.saved_config.selected_ids().is_empty());
    Ok(())
}

#[test]
fn stale_missing_row_id_is_inert() -> Result<()> {
    let mut model = model("http://127.0.0.1:9".to_string())?;
    draw_sized(&mut model, 80, 12)?;
    let alpha = row_rect(&model, "power/alpha")?;
    model.saved_config.items.remove(0);
    model.saved_config.blur();

    handle_mouse_at(
        &mut model,
        mouse(MouseEventKind::Down(MouseButton::Left), alpha),
        Instant::now(),
    )?;

    assert!(!model.saved_config.focused);
    assert_eq!(model.saved_config.cursor, 0);
    assert!(model.saved_config.selected_ids().is_empty());
    Ok(())
}

#[test]
fn non_left_down_row_events_are_inert() -> Result<()> {
    let mut model = model("http://127.0.0.1:9".to_string())?;
    draw_sized(&mut model, 80, 12)?;
    let alpha = row_rect(&model, "power/alpha")?;
    model.saved_config.blur();

    for kind in [
        MouseEventKind::Up(MouseButton::Left),
        MouseEventKind::Drag(MouseButton::Left),
        MouseEventKind::Moved,
        MouseEventKind::ScrollUp,
        MouseEventKind::ScrollDown,
        MouseEventKind::Down(MouseButton::Middle),
        MouseEventKind::Down(MouseButton::Right),
    ] {
        handle_mouse_at(&mut model, mouse(kind, alpha), Instant::now())?;
        assert!(!model.saved_config.focused);
        assert_eq!(model.saved_config.selected_ids(), ["power/alpha"]);
    }
    Ok(())
}

#[test]
fn modal_confirm_starts_one_confirmed_save_request() -> Result<()> {
    let gate = Arc::new(Barrier::new(2));
    let (url, requests) = mock_server(vec![
        Reply::Gated(200, SAVE, gate.clone()),
        Reply::Http(200, SHOW),
    ]);
    let mut model = model(url)?;
    assert!(model.saved_config.request_save().is_none());
    draw_sized(&mut model, 80, 12)?;
    let confirm = modal_rect(&model, SavedConfigModalTarget::confirm())?;

    handle_mouse_at(
        &mut model,
        mouse(MouseEventKind::Down(MouseButton::Left), confirm),
        Instant::now(),
    )?;
    handle_mouse_at(
        &mut model,
        mouse(MouseEventKind::Down(MouseButton::Left), confirm),
        Instant::now(),
    )?;

    let mutation = requests.recv_timeout(Duration::from_secs(1))?;
    assert!(mutation.starts_with("PUT /api/v1/config HTTP/1.1"));
    assert!(mutation.contains(r#"{"confirm":true,"items":["power/alpha"]}"#));
    gate.wait();
    assert!(requests
        .recv_timeout(Duration::from_secs(1))?
        .starts_with("GET /api/v1/config HTTP/1.1"));
    assert!(requests.recv_timeout(Duration::from_millis(50)).is_err());
    assert_eq!(model.saved_config.busy, Some(ConfigJobKind::Save));
    assert!(model.saved_config.confirmation().is_none());
    Ok(())
}

#[test]
fn modal_cancel_only_cancels() -> Result<()> {
    let mut model = model("http://127.0.0.1:9".to_string())?;
    assert!(model.saved_config.request_save().is_none());
    draw_sized(&mut model, 80, 12)?;
    let cancel = modal_rect(&model, SavedConfigModalTarget::cancel())?;

    handle_mouse_at(
        &mut model,
        mouse(MouseEventKind::Down(MouseButton::Left), cancel),
        Instant::now(),
    )?;

    assert!(model.saved_config.confirmation().is_none());
    assert!(model.saved_config.busy.is_none());
    Ok(())
}

#[test]
fn modal_precedence_blocks_rows_tabs_outside_and_other_events() -> Result<()> {
    let mut model = model("http://127.0.0.1:9".to_string())?;
    assert!(model.saved_config.request_save().is_none());
    draw_sized(&mut model, 80, 12)?;
    let row = row_rect(&model, "power/alpha")?;
    let tab = model
        .hit_map
        .tabs
        .iter()
        .find(|(_, target)| **target == TabTarget(ActivePage::Status))
        .map(|(rect, _)| *rect)
        .ok_or_else(|| anyhow!("missing Status tab hit"))?;
    let confirm = modal_rect(&model, SavedConfigModalTarget::confirm())?;

    for event in [
        mouse(MouseEventKind::Down(MouseButton::Left), row),
        mouse(MouseEventKind::Down(MouseButton::Left), tab),
        mouse(
            MouseEventKind::Down(MouseButton::Left),
            Rect::new(0, 0, 1, 1),
        ),
        mouse(MouseEventKind::Up(MouseButton::Left), confirm),
        mouse(MouseEventKind::Drag(MouseButton::Left), confirm),
        mouse(MouseEventKind::Moved, confirm),
        mouse(MouseEventKind::ScrollUp, confirm),
        mouse(MouseEventKind::Down(MouseButton::Middle), confirm),
        mouse(MouseEventKind::Down(MouseButton::Right), confirm),
    ] {
        handle_mouse_at(&mut model, event, Instant::now())?;
        assert!(model.saved_config.confirmation().is_some());
        assert_eq!(model.active_page, ActivePage::SavedConfig);
        assert_eq!(model.saved_config.selected_ids(), ["power/alpha"]);
        assert!(model.saved_config.busy.is_none());
    }
    Ok(())
}
