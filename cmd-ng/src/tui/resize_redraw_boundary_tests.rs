use super::confirm::{ConfirmableCommand, HardwareConfirmation};
use super::gpio_fixture::{control_rect, draw, projection_model};
use super::gpio_gesture_types::GpioGestureState;
use super::hit_types::{
    HardwareModalTarget, SavedConfigModalTarget, SavedConfigRowTarget, TabTarget,
};
use super::model::TuiModel;
use super::mouse_events::handle_mouse_at;
use super::pages::ActivePage;
use super::runtime::{drain_ready_events, EventDisposition};
use crate::persistent_config::ConfigItemId;
use anyhow::{anyhow, Result};
use crossterm::event::{Event, KeyModifiers, MouseButton, MouseEvent, MouseEventKind};
use ratatui::layout::Rect;
use std::collections::VecDeque;
use std::time::{Duration, Instant};

fn rendered_model(start: Instant) -> Result<(TuiModel, Rect)> {
    let mut model = projection_model()?;
    model.base_url = "http://127.0.0.1:0".to_string();
    model.last_http_poll = Some(start);
    model.hardware_confirm = Some(HardwareConfirmation {
        command: ConfirmableCommand::SetPower {
            output: "12v_out".to_string(),
            next_state: true,
        },
        started: start,
    });
    draw(&mut model, 80, 24)?;
    if model.hit_map.hardware_modal.is_empty() {
        return Err(anyhow!(
            "rendered hardware confirmation must register modal hit rectangles"
        ));
    }
    model.hardware_confirm = None;
    let rect = control_rect(&model, "GP10")?;
    Ok((model, rect))
}

fn mouse(kind: MouseEventKind, rect: Rect) -> MouseEvent {
    MouseEvent {
        kind,
        column: rect.x,
        row: rect.y,
        modifiers: KeyModifiers::NONE,
    }
}

fn assert_resize_redraw_boundary(release_before_resize: bool) -> Result<()> {
    let start = Instant::now();
    let (mut model, rect) = rendered_model(start)?;
    handle_mouse_at(
        &mut model,
        mouse(MouseEventKind::Down(MouseButton::Left), rect),
        start,
    )?;
    if release_before_resize {
        handle_mouse_at(
            &mut model,
            mouse(MouseEventKind::Up(MouseButton::Left), rect),
            start + Duration::from_millis(1),
        )?;
    }
    if release_before_resize {
        assert!(matches!(
            model.gpio_gesture.state,
            GpioGestureState::AwaitSecond { .. }
        ));
    } else {
        assert!(matches!(
            model.gpio_gesture.state,
            GpioGestureState::Down { .. }
        ));
    }
    assert!(!model.hit_map.controls.is_empty());
    assert!(!model.hit_map.hardware_modal.is_empty());
    model
        .hit_map
        .tabs
        .push(rect, TabTarget(ActivePage::SavedConfig));
    model.hit_map.saved_config_rows.push(
        rect,
        SavedConfigRowTarget(ConfigItemId("power/alpha".to_string())),
    );
    model
        .hit_map
        .saved_config_modal
        .push(rect, SavedConfigModalTarget::confirm());
    model
        .hit_map
        .hardware_modal
        .push(rect, HardwareModalTarget::cancel());
    assert!(!model.hit_map.tabs.is_empty());
    assert!(!model.hit_map.saved_config_rows.is_empty());
    assert!(!model.hit_map.saved_config_modal.is_empty());

    let mut events = VecDeque::from([
        (Event::Resize(120, 32), start + Duration::from_millis(2)),
        (
            Event::Mouse(mouse(MouseEventKind::Moved, rect)),
            start + Duration::from_millis(3),
        ),
    ]);
    let disposition = drain_ready_events(&mut model, || Ok(events.pop_front()))?;

    assert_eq!(disposition, EventDisposition::Redraw);
    assert_eq!(events.len(), 1, "Resize must stop the ready-event drain");
    assert!(!model.gpio_gesture.is_active());
    assert!(model.hit_map.controls.is_empty());
    assert!(model.hit_map.tabs.is_empty());
    assert!(model.hit_map.saved_config_rows.is_empty());
    assert!(model.hit_map.hardware_modal.is_empty());
    assert!(model.hit_map.saved_config_modal.is_empty());
    Ok(())
}

#[test]
fn resize_cancels_down_gesture_and_defers_queued_mouse_until_redraw() -> Result<()> {
    assert_resize_redraw_boundary(false)
}

#[test]
fn resize_cancels_await_second_and_defers_queued_mouse_until_redraw() -> Result<()> {
    assert_resize_redraw_boundary(true)
}
