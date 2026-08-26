use super::gpio_fixture::{control_rect, item_index, projection_model};
use super::gpio_gesture::GpioGestureInput;
use super::gpio_io::{GpioAction, GpioJob};
use super::model::TuiModel;
use super::mouse_events::handle_mouse_at;
use super::mouse_fixture::draw_sized;
use super::pages::ActivePage;
use super::runtime::on_time_tick;
use anyhow::Result;
use crossterm::event::{KeyModifiers, MouseButton, MouseEvent, MouseEventKind};
use ratatui::layout::Rect;
use std::time::{Duration, Instant};

fn model() -> Result<TuiModel> {
    let mut model = projection_model()?;
    model.base_url = "http://127.0.0.1:9".to_string();
    model.last_http_poll = Some(Instant::now());
    draw_sized(&mut model, 80, 24)?;
    Ok(model)
}

fn mouse(model: &mut TuiModel, kind: MouseEventKind, rect: Rect, now: Instant) -> Result<()> {
    handle_mouse_at(
        model,
        MouseEvent {
            kind,
            column: rect.x,
            row: rect.y,
            modifiers: KeyModifiers::NONE,
        },
        now,
    )
    .map(|_| ())
}

#[test]
fn drag_on_the_same_cell_preserves_a_hold() -> Result<()> {
    let start = Instant::now();
    let mut model = model()?;
    let rect = control_rect(&model, "GP10")?;
    mouse(
        &mut model,
        MouseEventKind::Down(MouseButton::Left),
        rect,
        start,
    )?;
    mouse(
        &mut model,
        MouseEventKind::Drag(MouseButton::Left),
        rect,
        start,
    )?;
    on_time_tick(&mut model, start + Duration::from_millis(600))?;
    assert_eq!(
        model.gpio_pending,
        Some(GpioJob {
            action: GpioAction::DriveHigh,
            target: "GP10".to_string(),
        })
    );
    Ok(())
}

#[test]
fn drag_to_a_different_cell_cancels_a_hold() -> Result<()> {
    let start = Instant::now();
    let mut model = model()?;
    let first = control_rect(&model, "GP10")?;
    let second = control_rect(&model, "GP16")?;
    mouse(
        &mut model,
        MouseEventKind::Down(MouseButton::Left),
        first,
        start,
    )?;
    mouse(
        &mut model,
        MouseEventKind::Drag(MouseButton::Left),
        second,
        start,
    )?;
    on_time_tick(&mut model, start + Duration::from_millis(600))?;
    assert_eq!(model.gpio_pending, None);
    Ok(())
}

#[test]
fn drag_on_the_same_cell_preserves_await_second() -> Result<()> {
    let start = Instant::now();
    let mut model = model()?;
    let rect = control_rect(&model, "GP10")?;
    mouse(
        &mut model,
        MouseEventKind::Down(MouseButton::Left),
        rect,
        start,
    )?;
    mouse(
        &mut model,
        MouseEventKind::Up(MouseButton::Left),
        rect,
        start,
    )?;
    mouse(
        &mut model,
        MouseEventKind::Drag(MouseButton::Left),
        rect,
        start + Duration::from_millis(1),
    )?;
    on_time_tick(&mut model, start + Duration::from_millis(220))?;
    assert_eq!(
        model.gpio_pending,
        Some(GpioJob {
            action: GpioAction::DriveLow,
            target: "GP10".to_string(),
        })
    );
    Ok(())
}

#[test]
fn drag_to_a_different_cell_cancels_await_second() -> Result<()> {
    let start = Instant::now();
    let mut model = model()?;
    let first = control_rect(&model, "GP10")?;
    let second = control_rect(&model, "GP16")?;
    mouse(
        &mut model,
        MouseEventKind::Down(MouseButton::Left),
        first,
        start,
    )?;
    mouse(
        &mut model,
        MouseEventKind::Up(MouseButton::Left),
        first,
        start,
    )?;
    mouse(
        &mut model,
        MouseEventKind::Drag(MouseButton::Left),
        second,
        start + Duration::from_millis(1),
    )?;
    on_time_tick(&mut model, start + Duration::from_millis(220))?;
    assert_eq!(model.gpio_pending, None);
    Ok(())
}

#[test]
fn tick_dispatches_the_original_pin_after_selection_changes() -> Result<()> {
    let start = Instant::now();
    let mut model = model()?;
    let first = control_rect(&model, "GP10")?;
    mouse(
        &mut model,
        MouseEventKind::Down(MouseButton::Left),
        first,
        start,
    )?;
    mouse(
        &mut model,
        MouseEventKind::Up(MouseButton::Left),
        first,
        start,
    )?;
    model.control_idx = item_index(&model, "GP16")?;
    on_time_tick(&mut model, start + Duration::from_millis(220))?;
    assert_eq!(
        model.gpio_pending,
        Some(GpioJob {
            action: GpioAction::DriveLow,
            target: "GP10".to_string(),
        })
    );
    Ok(())
}

#[test]
fn saved_config_error_blocks_mouse_and_cancels_an_active_gesture() -> Result<()> {
    let now = Instant::now();
    let mut model = model()?;
    let rect = control_rect(&model, "GP10")?;
    let _ = model.gpio_gesture.down(
        GpioGestureInput {
            pin: Some("GP10"),
            column: rect.x,
            row: rect.y,
        },
        now,
    );
    model.saved_config.error = Some("failure".to_string());
    mouse(&mut model, MouseEventKind::Moved, rect, now)?;
    assert_eq!(model.gpio_gesture.holding_pin(), None);
    Ok(())
}

#[test]
fn stale_controls_hit_map_cannot_arm_or_fire_a_gesture_outside_controls() -> Result<()> {
    let start = Instant::now();
    let mut model = model()?;
    let rect = control_rect(&model, "GP10")?;
    model.set_page(ActivePage::SavedConfig);
    mouse(
        &mut model,
        MouseEventKind::Down(MouseButton::Left),
        rect,
        start,
    )?;
    mouse(
        &mut model,
        MouseEventKind::Up(MouseButton::Left),
        rect,
        start,
    )?;
    on_time_tick(&mut model, start + Duration::from_millis(220))?;
    assert_eq!(model.gpio_pending, None);
    Ok(())
}
