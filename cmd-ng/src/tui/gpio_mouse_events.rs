use super::actions::{activate_item, ControlIntent};
use super::controls::{control_items, ControlItem};
use super::events::{activate_gpio_gesture, select_item};
use super::gpio_gesture::{GpioGestureInput, SHORT_PRESS_WINDOW};
use super::model::TuiModel;
use anyhow::Result;
use crossterm::event::{MouseButton, MouseEvent, MouseEventKind};
use std::time::Instant;

pub(super) fn handle_controls_mouse_at(
    model: &mut TuiModel,
    mouse: MouseEvent,
    now: Instant,
) -> Result<()> {
    let item = model.hit_map.controls.at(mouse.column, mouse.row).cloned();
    if item
        .as_ref()
        .is_some_and(|target| !control_items(model).contains(target))
    {
        return Ok(());
    }
    let input = GpioGestureInput {
        pin: match &item {
            Some(ControlItem::Gpio(pin)) => Some(pin),
            Some(ControlItem::Power(_)) | Some(ControlItem::Switch(_)) | None => None,
        },
        column: mouse.column,
        row: mouse.row,
    };
    match mouse.kind {
        MouseEventKind::Down(MouseButton::Left) => on_left_down(model, item.as_ref(), input, now),
        MouseEventKind::Up(MouseButton::Left) => on_left_up(model, input, now),
        MouseEventKind::Moved | MouseEventKind::Drag(MouseButton::Left) => {
            if model.gpio_gesture.is_active() {
                model.gpio_gesture.moved(input);
            }
            Ok(())
        }
        MouseEventKind::Down(MouseButton::Middle)
        | MouseEventKind::Down(MouseButton::Right)
        | MouseEventKind::Up(MouseButton::Middle)
        | MouseEventKind::Up(MouseButton::Right)
        | MouseEventKind::Drag(MouseButton::Middle)
        | MouseEventKind::Drag(MouseButton::Right) => Ok(()),
        _ => Ok(()),
    }
}

fn on_left_down(
    model: &mut TuiModel,
    item: Option<&ControlItem>,
    input: GpioGestureInput<'_>,
    now: Instant,
) -> Result<()> {
    match item {
        Some(item @ ControlItem::Gpio(_)) => {
            if model.gpio_pending.is_some() {
                model.gpio_gesture.cancel();
                return Ok(());
            }
            if !model.gpio_gesture.is_active() {
                select_item(model, item);
            }
            if let Some(action) = model.gpio_gesture.down(input, now) {
                activate_gpio_gesture(model, action)?;
            } else {
                model.gpio_poll_defer_until = Some(now + std::time::Duration::from_millis(600));
            }
            Ok(())
        }
        Some(item @ ControlItem::Power(_)) | Some(item @ ControlItem::Switch(_)) => {
            if model.gpio_pending.is_some() {
                model.gpio_gesture.cancel();
                return Ok(());
            }
            if settle_active_gesture(model, input, now)? {
                return Ok(());
            }
            select_item(model, item);
            activate_item(model, item.clone(), ControlIntent::Primary)
        }
        None => {
            let _ = settle_active_gesture(model, input, now)?;
            Ok(())
        }
    }
}

fn settle_active_gesture(
    model: &mut TuiModel,
    input: GpioGestureInput<'_>,
    now: Instant,
) -> Result<bool> {
    if !model.gpio_gesture.is_active() {
        return Ok(false);
    }
    if let Some(action) = model.gpio_gesture.down(input, now) {
        activate_gpio_gesture(model, action)?;
    }
    Ok(true)
}

fn on_left_up(model: &mut TuiModel, input: GpioGestureInput<'_>, now: Instant) -> Result<()> {
    if model.gpio_pending.is_some() {
        model.gpio_gesture.cancel();
        return Ok(());
    }
    if let Some(action) = model.gpio_gesture.up(input, now) {
        activate_gpio_gesture(model, action)?;
    } else if model.gpio_gesture.is_active() {
        model.gpio_poll_defer_until = Some(now + SHORT_PRESS_WINDOW);
    }
    Ok(())
}
