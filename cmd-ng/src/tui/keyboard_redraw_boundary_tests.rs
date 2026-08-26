use super::config_result::ConfigJobKind;
use super::controls::{control_items, ControlItem};
use super::hit_types::TabTarget;
use super::keyboard_boundary_fixture::{
    confirmed_switch_model, hardware_button, key, left_down, saved_config_button,
    saved_config_model, switch_model, SAVE, SHOW,
};
use super::mock_board::{mock_server, Reply};
use super::model::TuiModel;
use super::mouse_fixture::{control_rect, draw_sized};
use super::pages::ActivePage;
use super::runtime::{drain_ready_events, EventDisposition};
use anyhow::{anyhow, Result};
use crossterm::event::{Event, KeyCode};
use std::collections::VecDeque;
use std::time::{Duration, Instant};

fn drain(
    model: &mut TuiModel,
    events: &mut VecDeque<(Event, Instant)>,
) -> Result<EventDisposition> {
    drain_ready_events(model, || Ok(events.pop_front()))
}

fn gpio_worker_model() -> Result<TuiModel> {
    let mut snapshot: crate::ws_status::WsStatusSnapshot =
        serde_json::from_str(super::gpio_fixture::STATUS_JSON)?;
    snapshot.power_outputs = super::gpio_fixture::current_power_outputs();
    snapshot.switches.insert(
        "tf_wp".to_string(),
        crate::ws_status::TuiStatusSwitchInfo {
            route: "writable".to_string(),
            routes: vec!["writable".to_string(), "protected".to_string()],
            ..Default::default()
        },
    );
    let mut model = TuiModel::new("http://127.0.0.1:9".to_string(), Duration::from_secs(1));
    model.apply_status_snapshot(snapshot);
    model.control_idx = control_items(&model)
        .iter()
        .position(|item| *item == ControlItem::Gpio("GP10".to_string()))
        .ok_or_else(|| anyhow!("missing GP10 control item"))?;
    Ok(model)
}

#[test]
fn keyboard_hardware_confirm_defers_two_stale_downs_until_redraw() -> Result<()> {
    // Given an existing hardware modal and two old-coordinate Downs behind Enter.
    let start = Instant::now();
    let (url, requests) = mock_server(vec![Reply::Http(200, "{}"), Reply::Http(200, "{}")]);
    let mut model = confirmed_switch_model(url, start)?;
    draw_sized(&mut model, 80, 8)?;
    let old_confirm = hardware_button(&model)?;
    let mut events = VecDeque::from([
        (key(KeyCode::Enter), start + Duration::from_millis(1)),
        (left_down(old_confirm), start + Duration::from_millis(2)),
        (left_down(old_confirm), start + Duration::from_millis(3)),
    ]);

    // When Enter executes the visible confirmation.
    let first = drain(&mut model, &mut events)?;

    // Then both mouse events stay queued until the closed modal is redrawn.
    assert_eq!(first, EventDisposition::Redraw);
    assert_eq!(events.len(), 2);
    assert!(model.hardware_confirm.is_none());
    assert!(requests
        .recv_timeout(Duration::from_secs(1))?
        .starts_with("PUT /api/v1/switch/tf_wp"));

    draw_sized(&mut model, 80, 8)?;
    assert_eq!(drain(&mut model, &mut events)?, EventDisposition::Redraw);
    assert_eq!(events.len(), 1, "one modal transition per redraw");
    assert!(model.hardware_confirm.is_some());
    assert!(
        requests.recv_timeout(Duration::from_millis(30)).is_err(),
        "stale Downs must not execute a hidden second PUT"
    );
    Ok(())
}

#[test]
fn keyboard_saved_config_confirm_defers_two_stale_row_downs_until_redraw() -> Result<()> {
    // Given a Saved Config modal and two Downs at its old Confirm coordinates.
    let start = Instant::now();
    let (url, requests) = mock_server(vec![Reply::Http(200, SAVE), Reply::Http(200, SHOW)]);
    let mut model = saved_config_model(url)?;
    assert!(model.saved_config.request_save().is_none());
    draw_sized(&mut model, 80, 8)?;
    let old_confirm = saved_config_button(&model)?;
    let selected_before = model.saved_config.selected_ids();
    let mut events = VecDeque::from([
        (key(KeyCode::Enter), start),
        (left_down(old_confirm), start + Duration::from_millis(1)),
        (left_down(old_confirm), start + Duration::from_millis(2)),
    ]);

    // When Enter confirms the visible save modal.
    let first = drain(&mut model, &mut events)?;

    // Then no stale row Down is consumed before a fresh Saved Config frame.
    assert_eq!(first, EventDisposition::Redraw);
    assert_eq!(events.len(), 2);
    assert_eq!(model.saved_config.selected_ids(), selected_before);
    assert!(requests
        .recv_timeout(Duration::from_secs(1))?
        .starts_with("PUT /api/v1/config"));
    assert!(requests
        .recv_timeout(Duration::from_secs(1))?
        .starts_with("GET /api/v1/config"));

    draw_sized(&mut model, 80, 8)?;
    assert_eq!(drain(&mut model, &mut events)?, EventDisposition::Redraw);
    assert_eq!(events.len(), 1, "one visible row transition per redraw");
    assert!(
        requests.recv_timeout(Duration::from_millis(30)).is_err(),
        "stale row Downs must not start another config mutation"
    );

    draw_sized(&mut model, 80, 8)?;
    assert_eq!(drain(&mut model, &mut events)?, EventDisposition::Redraw);
    assert!(events.is_empty());
    assert_eq!(model.saved_config.selected_ids(), selected_before);
    assert!(requests.recv_timeout(Duration::from_millis(30)).is_err());
    Ok(())
}

#[test]
fn keyboard_enter_opening_switch_modal_defers_queued_mouse() -> Result<()> {
    // Given a selected switch and a queued Down over its rendered row.
    let start = Instant::now();
    let mut model = switch_model("http://127.0.0.1:9".to_string())?;
    draw_sized(&mut model, 80, 8)?;
    let switch = control_rect(&model, &ControlItem::Switch("tf_wp".to_string()))
        .ok_or_else(|| anyhow!("missing switch row"))?;
    let mut events = VecDeque::from([
        (key(KeyCode::Enter), start),
        (left_down(switch), start + Duration::from_millis(1)),
    ]);

    // When Enter opens the switch confirmation modal.
    let disposition = drain(&mut model, &mut events)?;

    // Then the mouse event remains queued for the rendered modal geometry.
    assert_eq!(disposition, EventDisposition::Redraw);
    assert_eq!(events.len(), 1);
    assert!(model.hardware_confirm.is_some());
    Ok(())
}

#[test]
fn three_queued_enters_advance_one_confirmation_stage_per_redraw() -> Result<()> {
    // Given three ready Enters on a selected switch and a board mock for one PUT.
    let start = Instant::now();
    let (url, requests) = mock_server(vec![Reply::Http(200, "{}")]);
    let mut model = switch_model(url)?;
    draw_sized(&mut model, 80, 8)?;
    let mut events = VecDeque::from([
        (key(KeyCode::Enter), start),
        (key(KeyCode::Enter), start + Duration::from_millis(1)),
        (key(KeyCode::Enter), start + Duration::from_millis(2)),
    ]);

    // When each redraw boundary admits the next queued Enter.
    assert_eq!(drain(&mut model, &mut events)?, EventDisposition::Redraw);
    assert_eq!(events.len(), 2);
    assert!(model.hardware_confirm.is_some());
    assert!(requests.recv_timeout(Duration::from_millis(30)).is_err());

    draw_sized(&mut model, 80, 8)?;
    assert_eq!(drain(&mut model, &mut events)?, EventDisposition::Redraw);
    assert_eq!(events.len(), 1);
    assert!(model.hardware_confirm.is_none());
    assert!(requests
        .recv_timeout(Duration::from_secs(1))?
        .starts_with("PUT /api/v1/switch/tf_wp"));

    draw_sized(&mut model, 80, 8)?;
    assert_eq!(drain(&mut model, &mut events)?, EventDisposition::Redraw);

    // Then the third Enter only opens the next visible modal and sends no second PUT.
    assert!(events.is_empty());
    assert!(model.hardware_confirm.is_some());
    assert!(requests.recv_timeout(Duration::from_millis(30)).is_err());
    Ok(())
}

#[test]
fn gpio_key_redraw_defers_queued_power_and_switch_downs() -> Result<()> {
    for target in [
        ControlItem::Power("12v_out".to_string()),
        ControlItem::Switch("tf_wp".to_string()),
    ] {
        // Given a selected GPIO and a ready Down over another control domain.
        let start = Instant::now();
        let mut model = gpio_worker_model()?;
        draw_sized(&mut model, 80, 30)?;
        let target_rect = control_rect(&model, &target)
            .ok_or_else(|| anyhow!("missing queued target {target:?}"))?;
        let selected_before = model.control_idx;
        let mut events = VecDeque::from([
            (key(KeyCode::Char('l')), start),
            (left_down(target_rect), start + Duration::from_millis(1)),
        ]);

        // When the direct GPIO action starts its worker.
        let disposition = drain(&mut model, &mut events)?;

        // Then the redraw boundary stops the stale Power/Switch Down.
        assert_eq!(disposition, EventDisposition::Redraw);
        assert_eq!(events.len(), 1, "the cross-domain Down must remain queued");
        assert!(model.gpio_pending.is_some());
        assert!(model.hardware_confirm.is_none());
        assert_eq!(model.control_idx, selected_before);
    }
    Ok(())
}

#[test]
fn config_clear_redraw_defers_queued_saved_config_row_and_tab_downs() -> Result<()> {
    // Given a rendered Saved Config page and stale row/tab coordinates behind x.
    let start = Instant::now();
    let mut model = saved_config_model("http://127.0.0.1:9".to_string())?;
    draw_sized(&mut model, 80, 12)?;
    let row = model
        .hit_map
        .saved_config_rows
        .iter()
        .next()
        .map(|(rect, _)| *rect)
        .ok_or_else(|| anyhow!("missing Saved Config row"))?;
    let tab = model
        .hit_map
        .tabs
        .iter()
        .find(|(_, target)| **target == TabTarget(ActivePage::Status))
        .map(|(rect, _)| *rect)
        .ok_or_else(|| anyhow!("missing Status tab"))?;
    let selected_before = model.saved_config.selected_ids();
    let cursor_before = model.saved_config.cursor;
    let mut events = VecDeque::from([
        (key(KeyCode::Char('x')), start),
        (left_down(row), start + Duration::from_millis(1)),
        (left_down(tab), start + Duration::from_millis(2)),
    ]);

    // When x starts the Saved Config worker.
    let disposition = drain(&mut model, &mut events)?;

    // Then both stale interactions remain queued until the busy frame is drawn.
    assert_eq!(disposition, EventDisposition::Redraw);
    assert_eq!(events.len(), 2);
    assert_eq!(model.saved_config.busy, Some(ConfigJobKind::Clear));
    assert_eq!(model.active_page, ActivePage::SavedConfig);
    assert_eq!(model.saved_config.selected_ids(), selected_before);
    assert_eq!(model.saved_config.cursor, cursor_before);
    Ok(())
}
