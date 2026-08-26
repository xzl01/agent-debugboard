use super::gpio_fixture::{control_rect, draw, projection_model};
use super::hit_types::TabTarget;
use super::model::TuiModel;
use super::mouse_events::handle_mouse_at;
use super::pages::ActivePage;
use super::runtime::on_time_tick;
use anyhow::{anyhow, Result};
use crossterm::event::{KeyModifiers, MouseButton, MouseEvent, MouseEventKind};
use ratatui::layout::Rect;
use std::time::{Duration, Instant};

fn mouse(kind: MouseEventKind, column: u16, row: u16) -> MouseEvent {
    MouseEvent {
        kind,
        column,
        row,
        modifiers: KeyModifiers::NONE,
    }
}

fn tab_rect(model: &TuiModel, page: ActivePage) -> Result<Rect> {
    model
        .hit_map
        .tabs
        .iter()
        .find(|(_, target)| **target == TabTarget(page))
        .map(|(rect, _)| *rect)
        .ok_or_else(|| anyhow!("missing {page:?} tab hit"))
}

#[test]
fn up_over_an_inactive_tab_cancels_an_active_down_gesture_before_deadlines() -> Result<()> {
    let start = Instant::now();
    let mut model = projection_model()?;
    model.last_http_poll = Some(start);
    draw(&mut model, 80, 24)?;
    let control = control_rect(&model, "GP10")?;
    let tab = tab_rect(&model, ActivePage::SavedConfig)?;

    handle_mouse_at(
        &mut model,
        mouse(
            MouseEventKind::Down(MouseButton::Left),
            control.x,
            control.y,
        ),
        start,
    )?;
    handle_mouse_at(
        &mut model,
        mouse(MouseEventKind::Up(MouseButton::Left), tab.x, tab.y),
        start + Duration::from_millis(1),
    )?;

    assert_eq!(model.active_page, ActivePage::Controls);
    assert!(!model.gpio_gesture.is_active());
    assert!(model.gpio_pending.is_none());

    on_time_tick(&mut model, start + Duration::from_millis(220))?;
    assert!(model.gpio_pending.is_none());
    on_time_tick(&mut model, start + Duration::from_millis(600))?;
    assert!(model.gpio_pending.is_none());
    Ok(())
}

#[test]
fn drag_over_an_inactive_tab_cancels_an_await_second_gesture_before_deadlines() -> Result<()> {
    let start = Instant::now();
    let mut model = projection_model()?;
    model.last_http_poll = Some(start);
    draw(&mut model, 80, 24)?;
    let control = control_rect(&model, "GP10")?;
    let tab = tab_rect(&model, ActivePage::SavedConfig)?;

    handle_mouse_at(
        &mut model,
        mouse(
            MouseEventKind::Down(MouseButton::Left),
            control.x,
            control.y,
        ),
        start,
    )?;
    handle_mouse_at(
        &mut model,
        mouse(MouseEventKind::Up(MouseButton::Left), control.x, control.y),
        start + Duration::from_millis(1),
    )?;
    assert!(model.gpio_gesture.is_active());

    handle_mouse_at(
        &mut model,
        mouse(MouseEventKind::Drag(MouseButton::Left), tab.x, tab.y),
        start + Duration::from_millis(2),
    )?;

    assert_eq!(model.active_page, ActivePage::Controls);
    assert!(!model.gpio_gesture.is_active());
    assert!(model.gpio_pending.is_none());

    on_time_tick(&mut model, start + Duration::from_millis(220))?;
    assert!(model.gpio_pending.is_none());
    on_time_tick(&mut model, start + Duration::from_millis(600))?;
    assert!(model.gpio_pending.is_none());
    Ok(())
}

#[test]
fn moved_over_an_inactive_tab_cancels_an_await_second_gesture_before_deadlines() -> Result<()> {
    let start = Instant::now();
    let mut model = projection_model()?;
    model.last_http_poll = Some(start);
    draw(&mut model, 80, 24)?;
    let control = control_rect(&model, "GP10")?;
    let tab = tab_rect(&model, ActivePage::SavedConfig)?;

    handle_mouse_at(
        &mut model,
        mouse(
            MouseEventKind::Down(MouseButton::Left),
            control.x,
            control.y,
        ),
        start,
    )?;
    handle_mouse_at(
        &mut model,
        mouse(MouseEventKind::Up(MouseButton::Left), control.x, control.y),
        start + Duration::from_millis(1),
    )?;
    assert!(model.gpio_gesture.is_active());

    handle_mouse_at(
        &mut model,
        mouse(MouseEventKind::Moved, tab.x, tab.y),
        start + Duration::from_millis(2),
    )?;

    assert_eq!(model.active_page, ActivePage::Controls);
    assert!(!model.gpio_gesture.is_active());
    assert!(model.gpio_pending.is_none());

    on_time_tick(&mut model, start + Duration::from_millis(220))?;
    assert!(model.gpio_pending.is_none());
    on_time_tick(&mut model, start + Duration::from_millis(600))?;
    assert!(model.gpio_pending.is_none());
    Ok(())
}
