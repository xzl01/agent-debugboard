use super::controls::ControlItem;
use super::gpio_io::{GpioAction, GpioJob};
use super::model::TuiModel;
use super::mouse_events::handle_mouse_at;
use super::mouse_fixture::{control_rect, draw, draw_sized, model};
use super::runtime::on_time_tick;
use anyhow::{anyhow, Result};
use crossterm::event::{KeyModifiers, MouseButton, MouseEvent, MouseEventKind};
use ratatui::layout::Rect;
use std::time::{Duration, Instant};

fn gpio_click_model() -> Result<TuiModel> {
    let mut model = super::gpio_fixture::projection_model()?;
    model.base_url = "http://127.0.0.1:9".to_string();
    model.timeout = Duration::from_millis(30);
    model.last_http_poll = Some(Instant::now());
    draw_sized(&mut model, 80, 24)?;
    Ok(model)
}

fn gpio_rect(model: &TuiModel, gpio: &str) -> Result<Rect> {
    control_rect(model, &ControlItem::Gpio(gpio.to_string()))
        .ok_or_else(|| anyhow!("no hit rect for {gpio}"))
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

fn job(action: GpioAction, target: &str) -> Option<GpioJob> {
    Some(GpioJob {
        action,
        target: target.to_string(),
    })
}

#[test]
fn left_down_on_gpio_arms_only_then_short_tick_dispatches_low() -> Result<()> {
    let start = Instant::now();
    let mut model = gpio_click_model()?;
    let rect = gpio_rect(&model, "GP10")?;
    mouse(
        &mut model,
        MouseEventKind::Down(MouseButton::Left),
        rect,
        start,
    )?;
    assert_eq!(model.gpio_pending, None);
    mouse(
        &mut model,
        MouseEventKind::Up(MouseButton::Left),
        rect,
        start,
    )?;
    on_time_tick(&mut model, start + Duration::from_millis(219))?;
    assert_eq!(model.gpio_pending, None);
    on_time_tick(&mut model, start + Duration::from_millis(220))?;
    assert_eq!(model.gpio_pending, job(GpioAction::DriveLow, "GP10"));
    Ok(())
}

#[test]
fn held_left_down_dispatches_high_once_and_up_is_inert() -> Result<()> {
    let start = Instant::now();
    let mut model = gpio_click_model()?;
    let rect = gpio_rect(&model, "GP10")?;
    mouse(
        &mut model,
        MouseEventKind::Down(MouseButton::Left),
        rect,
        start,
    )?;
    on_time_tick(&mut model, start + Duration::from_millis(600))?;
    assert_eq!(model.gpio_pending, job(GpioAction::DriveHigh, "GP10"));
    mouse(
        &mut model,
        MouseEventKind::Up(MouseButton::Left),
        rect,
        start + Duration::from_millis(601),
    )?;
    assert_eq!(model.gpio_pending, job(GpioAction::DriveHigh, "GP10"));
    Ok(())
}

#[test]
fn same_pin_double_click_dispatches_input_without_low() -> Result<()> {
    let start = Instant::now();
    let mut model = gpio_click_model()?;
    let rect = gpio_rect(&model, "GP10")?;
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
        MouseEventKind::Down(MouseButton::Left),
        rect,
        start + Duration::from_millis(100),
    )?;
    mouse(
        &mut model,
        MouseEventKind::Up(MouseButton::Left),
        rect,
        start + Duration::from_millis(101),
    )?;
    assert_eq!(model.gpio_pending, job(GpioAction::SetInput, "GP10"));
    on_time_tick(&mut model, start + Duration::from_millis(220))?;
    assert_eq!(model.gpio_pending, job(GpioAction::SetInput, "GP10"));
    Ok(())
}

#[test]
fn different_or_non_gpio_second_down_cancels_and_consumes() -> Result<()> {
    let start = Instant::now();
    let mut model = gpio_click_model()?;
    let first = gpio_rect(&model, "GP10")?;
    let second = gpio_rect(&model, "GP16")?;
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
        MouseEventKind::Down(MouseButton::Left),
        second,
        start + Duration::from_millis(1),
    )?;
    on_time_tick(&mut model, start + Duration::from_millis(220))?;
    assert_eq!(model.gpio_pending, None);

    mouse(
        &mut model,
        MouseEventKind::Down(MouseButton::Left),
        first,
        start + Duration::from_millis(300),
    )?;
    mouse(
        &mut model,
        MouseEventKind::Up(MouseButton::Left),
        first,
        start + Duration::from_millis(300),
    )?;
    mouse(
        &mut model,
        MouseEventKind::Down(MouseButton::Left),
        Rect::new(0, 0, 1, 1),
        start + Duration::from_millis(301),
    )?;
    assert!(model.hardware_confirm.is_none());
    Ok(())
}

#[test]
fn moved_to_a_different_cell_cancels_a_hold() -> Result<()> {
    let start = Instant::now();
    let mut model = gpio_click_model()?;
    let first = gpio_rect(&model, "GP10")?;
    let second = gpio_rect(&model, "GP16")?;
    mouse(
        &mut model,
        MouseEventKind::Down(MouseButton::Left),
        first,
        start,
    )?;
    mouse(
        &mut model,
        MouseEventKind::Moved,
        second,
        start + Duration::from_millis(1),
    )?;
    on_time_tick(&mut model, start + Duration::from_millis(600))?;
    assert_eq!(model.gpio_pending, None);
    Ok(())
}

#[test]
fn middle_and_right_are_inert_and_power_primary_is_preserved() -> Result<()> {
    let now = Instant::now();
    let mut gpio_model = gpio_click_model()?;
    let gpio = gpio_rect(&gpio_model, "GP10")?;
    for button in [MouseButton::Middle, MouseButton::Right] {
        mouse(&mut gpio_model, MouseEventKind::Down(button), gpio, now)?;
        assert_eq!(gpio_model.gpio_pending, None);
    }

    let mut model = model();
    draw(&mut model)?;
    let power = control_rect(&model, &ControlItem::Power("12v_out".to_string()))
        .ok_or_else(|| anyhow!("missing 12v_out power row"))?;
    mouse(
        &mut model,
        MouseEventKind::Down(MouseButton::Left),
        power,
        now,
    )?;
    assert!(model.hardware_confirm.is_some());
    Ok(())
}
