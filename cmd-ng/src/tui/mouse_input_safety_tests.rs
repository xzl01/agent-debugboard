use super::controls::{control_items, ControlItem};
use super::gpio_worker_fixture::{ADC_EMPTY, GPIO_OK};
use super::keyboard_boundary_fixture::{key, left_down, switch_model, SWITCH_NAME};
use super::mock_board::{mock_server, Reply};
use super::model::TuiModel;
use super::mouse_fixture::{control_rect, draw_sized, model as controls_model};
use super::runtime::{drain_ready_events, process_event, EventDisposition};
use anyhow::{anyhow, Result};
use crossterm::event::KeyCode;
use ratatui::layout::Rect;
use std::collections::VecDeque;
use std::sync::mpsc::Receiver;
use std::time::{Duration, Instant};

#[derive(Debug, Clone, Copy)]
enum ModalTarget {
    Power,
    Switch,
}

fn modal_target_model(target: ModalTarget, base_url: String) -> Result<(TuiModel, ControlItem)> {
    match target {
        ModalTarget::Power => {
            let mut model = controls_model();
            model.base_url = base_url;
            Ok((model, ControlItem::Power("12v_out".to_string())))
        }
        ModalTarget::Switch => Ok((
            switch_model(base_url)?,
            ControlItem::Switch(SWITCH_NAME.to_string()),
        )),
    }
}

fn assert_mouse_down_defers_queued_enter(target: ModalTarget) -> Result<()> {
    // Given: a rendered hardware row followed by a ready Enter event.
    let (url, requests) = mock_server(vec![Reply::Http(200, "{}")]);
    let (mut model, item) = modal_target_model(target, url)?;
    draw_sized(&mut model, 80, 8)?;
    let row =
        control_rect(&model, &item).ok_or_else(|| anyhow!("missing {target:?} control row"))?;
    let start = Instant::now();
    let mut events = VecDeque::from([
        (left_down(row), start),
        (key(KeyCode::Enter), start + Duration::from_millis(1)),
    ]);

    // When: the ready queue starts with the row's left-button Down.
    let disposition = drain_ready_events(&mut model, || Ok(events.pop_front()))?;

    // Then: Enter remains queued until the newly opened modal is rendered.
    assert_eq!(disposition, EventDisposition::Redraw);
    assert_eq!(events.len(), 1);
    assert!(model.hardware_confirm.is_some());
    assert!(
        requests.recv_timeout(Duration::from_millis(30)).is_err(),
        "{target:?} must not issue a PUT before modal redraw"
    );
    Ok(())
}

fn assert_stale_target_is_inert(item: ControlItem) -> Result<()> {
    // Given: an old hit target absent from the current authoritative controls.
    let (url, requests) = mock_server(vec![Reply::Http(200, GPIO_OK)]);
    let mut model = controls_model();
    model.base_url = url;
    model.control_idx = 1;
    let selected = control_items(&model)
        .get(model.control_idx)
        .cloned()
        .ok_or_else(|| anyhow!("missing selected control"))?;
    let stale_rect = Rect::new(0, 0, 1, 1);
    model.hit_map.controls.push(stale_rect, item.clone());
    let start = Instant::now();

    // When: the stale target receives a complete activation gesture.
    assert_eq!(
        process_event(&mut model, left_down(stale_rect), start)?,
        EventDisposition::Continue
    );
    if matches!(&item, ControlItem::Gpio(_)) {
        let left_up = crossterm::event::Event::Mouse(crossterm::event::MouseEvent {
            kind: crossterm::event::MouseEventKind::Up(crossterm::event::MouseButton::Left),
            column: stale_rect.x,
            row: stale_rect.y,
            modifiers: crossterm::event::KeyModifiers::NONE,
        });
        assert_eq!(
            process_event(&mut model, left_up, start + Duration::from_millis(600))?,
            EventDisposition::Continue
        );
    }

    // Then: it cannot select, arm, dispatch, or open a confirmation.
    assert_eq!(
        control_items(&model).get(model.control_idx),
        Some(&selected)
    );
    assert!(model.hardware_confirm.is_none(), "stale target: {item:?}");
    assert!(!model.gpio_gesture.is_active(), "stale target: {item:?}");
    assert!(model.gpio_pending.is_none(), "stale target: {item:?}");
    assert!(requests.recv_timeout(Duration::from_millis(30)).is_err());
    Ok(())
}

fn assert_refresh_requests(requests: &Receiver<String>) -> Result<()> {
    assert!(requests
        .recv_timeout(Duration::from_secs(1))?
        .starts_with("GET /api/v1/status HTTP/1.1"));
    assert!(requests
        .recv_timeout(Duration::from_secs(1))?
        .starts_with("GET /api/v1/adc/read HTTP/1.1"));
    Ok(())
}

#[test]
fn power_mouse_down_defers_queued_enter_until_modal_redraw() -> Result<()> {
    assert_mouse_down_defers_queued_enter(ModalTarget::Power)
}

#[test]
fn dynamic_switch_mouse_down_defers_queued_enter_until_modal_redraw() -> Result<()> {
    assert_mouse_down_defers_queued_enter(ModalTarget::Switch)
}

#[test]
fn stale_power_hit_target_is_inert() -> Result<()> {
    assert_stale_target_is_inert(ControlItem::Power("removed_power".to_string()))
}

#[test]
fn stale_switch_hit_target_is_inert() -> Result<()> {
    assert_stale_target_is_inert(ControlItem::Switch("removed_switch".to_string()))
}

#[test]
fn stale_gpio_hit_target_is_inert() -> Result<()> {
    assert_stale_target_is_inert(ControlItem::Gpio("removed_gpio".to_string()))
}

#[test]
fn refresh_removing_power_defers_stale_coordinate_until_fresh_render() -> Result<()> {
    // Given: r will remove the rendered power row while its old coordinate is queued.
    let (url, requests) = mock_server(vec![Reply::Http(200, "{}"), Reply::Http(200, ADC_EMPTY)]);
    let mut model = controls_model();
    model.base_url = url;
    draw_sized(&mut model, 80, 8)?;
    let old_row = control_rect(&model, &ControlItem::Power("12v_out".to_string()))
        .ok_or_else(|| anyhow!("missing 12v_out row"))?;
    let start = Instant::now();
    let mut events = VecDeque::from([
        (key(KeyCode::Char('r')), start),
        (left_down(old_row), start + Duration::from_millis(1)),
    ]);

    // When: the authoritative refresh completes during ready-event drain.
    let disposition = drain_ready_events(&mut model, || Ok(events.pop_front()))?;

    // Then: the old coordinate remains queued until a frame with no such hit exists.
    assert_eq!(disposition, EventDisposition::Redraw);
    assert_eq!(events.len(), 1);
    assert!(control_items(&model).is_empty());
    assert!(model.hardware_confirm.is_none());
    assert_refresh_requests(&requests)?;

    draw_sized(&mut model, 80, 8)?;
    assert!(model.hit_map.controls.at(old_row.x, old_row.y).is_none());
    assert_eq!(
        drain_ready_events(&mut model, || Ok(events.pop_front()))?,
        EventDisposition::Continue
    );
    assert!(events.is_empty());
    assert!(model.hardware_confirm.is_none());
    assert!(model.gpio_pending.is_none());
    assert!(requests.recv_timeout(Duration::from_millis(30)).is_err());
    Ok(())
}
