use super::actions::{activate_item, ControlIntent};
use super::config_result::ConfigJobKind;
use super::confirm::{ConfirmableCommand, HardwareConfirmation, CONFIRM_TIMEOUT};
use super::controls::{control_items, ControlItem};
use super::events::{handle_key, KeyOutcome};
use super::gpio_gesture::GpioGestureInput;
use super::gpio_io::{GpioAction, GpioJob};
use super::hit_types::{HardwareModalTarget, TabTarget};
use super::model::TuiModel;
use super::mouse_events::handle_mouse_at;
use super::mouse_fixture::{control_rect, draw_sized, model as controls_model};
use super::pages::ActivePage;
use super::{events_fixture, keyboard_boundary_fixture};
use anyhow::{anyhow, Result};
use crossterm::event::{KeyCode, KeyModifiers, MouseButton, MouseEvent, MouseEventKind};
use ratatui::layout::Rect;
use std::time::Instant;

fn left_down(rect: Rect) -> MouseEvent {
    MouseEvent {
        kind: MouseEventKind::Down(MouseButton::Left),
        column: rect.x,
        row: rect.y,
        modifiers: KeyModifiers::NONE,
    }
}

fn power_confirmation() -> HardwareConfirmation {
    HardwareConfirmation::new(ConfirmableCommand::SetPower {
        output: "12v_out".to_string(),
        next_state: true,
    })
}

fn item_index(model: &TuiModel, wanted: &ControlItem) -> Result<usize> {
    control_items(model)
        .iter()
        .position(|item| item == wanted)
        .ok_or_else(|| anyhow!("missing control item {wanted:?}"))
}

fn pending_gpio() -> GpioJob {
    GpioJob {
        action: GpioAction::DriveLow,
        target: "GP10".to_string(),
    }
}

#[test]
fn gpio_pending_blocks_shared_primary_power_and_switch_activation() -> Result<()> {
    // Given a GPIO job already in flight and each primary control selected.
    let mut model = events_fixture::model_with_switch();
    for item in [
        ControlItem::Power("12v_out".to_string()),
        ControlItem::Switch("sd".to_string()),
    ] {
        model.control_idx = item_index(&model, &item)?;
        model.gpio_pending = Some(pending_gpio());
        // When the shared activation seam receives a Power or Switch command.
        activate_item(&mut model, item, ControlIntent::Primary)?;
        // Then no hardware confirmation is opened.
        assert!(model.hardware_confirm.is_none());
    }
    Ok(())
}

#[test]
fn gpio_pending_blocks_keyboard_enter_and_space_for_power_and_switch() -> Result<()> {
    // Given a GPIO job in flight and keyboard focus on each non-GPIO control.
    let mut model = events_fixture::model_with_switch();
    for item in [
        ControlItem::Power("12v_out".to_string()),
        ControlItem::Switch("sd".to_string()),
    ] {
        model.control_idx = item_index(&model, &item)?;
        for code in [KeyCode::Enter, KeyCode::Char(' ')] {
            model.gpio_pending = Some(pending_gpio());
            // When Enter or Space is pressed.
            let outcome = handle_key(
                &mut model,
                crossterm::event::KeyEvent::new(code, KeyModifiers::NONE),
                Instant::now(),
            )?;
            // Then the key is inert and cannot open a modal.
            assert_eq!(outcome, KeyOutcome::Continue);
            assert!(model.hardware_confirm.is_none());
        }
    }
    Ok(())
}

#[test]
fn gpio_pending_blocks_mouse_primary_power_and_switch_activation() -> Result<()> {
    // Given rendered Power and Switch rows while a GPIO job is in flight.
    let mut model = events_fixture::model_with_switch();
    draw_sized(&mut model, 80, 24)?;
    for item in [
        ControlItem::Power("12v_out".to_string()),
        ControlItem::Switch("sd".to_string()),
    ] {
        model.control_idx = item_index(&model, &item)?;
        let rect = control_rect(&model, &item).ok_or_else(|| anyhow!("missing row"))?;
        model.gpio_pending = Some(pending_gpio());
        // When the row receives a primary mouse Down.
        handle_mouse_at(&mut model, left_down(rect), Instant::now())?;
        // Then the click cannot open a hardware confirmation.
        assert!(model.hardware_confirm.is_none());
    }
    Ok(())
}

#[test]
fn busy_keyboard_guard_preserves_saved_config_state_after_busy_redraw() -> Result<()> {
    // Given the first busy frame has already been rendered.
    let mut model = keyboard_boundary_fixture::saved_config_model("http://127.0.0.1:9".into())?;
    model.saved_config.busy = Some(ConfigJobKind::Save);
    model.saved_config.focus();
    model.saved_config.cursor = 1;
    let page = model.active_page;
    let cursor = model.saved_config.cursor;
    let selected = model.saved_config.selected_ids();
    model.gpio_names.push("GP10".to_string());
    model.gpio_gesture.down(
        GpioGestureInput {
            pin: Some("GP10"),
            column: 2,
            row: 2,
        },
        Instant::now(),
    );
    let gesture = model.gpio_gesture.clone();
    draw_sized(&mut model, 80, 12)?;

    for code in [
        KeyCode::Tab,
        KeyCode::Down,
        KeyCode::Char(' '),
        KeyCode::Char('s'),
        KeyCode::Char('x'),
        KeyCode::Esc,
    ] {
        // When a mutation, navigation, selection, or escape key arrives.
        assert_eq!(
            handle_key(
                &mut model,
                crossterm::event::KeyEvent::new(code, KeyModifiers::NONE),
                Instant::now(),
            )?,
            KeyOutcome::Continue
        );
        // Then all interaction state remains unchanged.
        assert_eq!(model.active_page, page);
        assert_eq!(model.saved_config.cursor, cursor);
        assert_eq!(model.saved_config.selected_ids(), selected);
        assert_eq!(model.gpio_gesture, gesture);
    }
    Ok(())
}

#[test]
fn busy_mouse_guard_preserves_navigation_selection_and_gesture_after_redraw() -> Result<()> {
    // Given a busy Saved Config frame with current tab and row hit geometry.
    let mut model = keyboard_boundary_fixture::saved_config_model("http://127.0.0.1:9".into())?;
    model.saved_config.busy = Some(ConfigJobKind::Save);
    model.saved_config.focus();
    draw_sized(&mut model, 80, 12)?;
    let tab = model
        .hit_map
        .tabs
        .iter()
        .find(|(_, target)| **target == TabTarget(ActivePage::Status))
        .map(|(rect, _)| *rect)
        .ok_or_else(|| anyhow!("missing status tab"))?;
    let row = model
        .hit_map
        .saved_config_rows
        .iter()
        .next()
        .map(|(rect, _)| *rect)
        .ok_or_else(|| anyhow!("missing config row"))?;
    let page = model.active_page;
    let cursor = model.saved_config.cursor;
    let selected = model.saved_config.selected_ids();

    for rect in [tab, row] {
        // When a tab or row mouse Down arrives after that redraw.
        handle_mouse_at(&mut model, left_down(rect), Instant::now())?;
        // Then page, cursor, and selection remain unchanged.
        assert_eq!(model.active_page, page);
        assert_eq!(model.saved_config.cursor, cursor);
        assert_eq!(model.saved_config.selected_ids(), selected);
    }
    Ok(())
}

#[test]
fn busy_input_keeps_hardware_modal_and_gpio_gesture_unchanged() -> Result<()> {
    // Given busy state, an existing hardware modal, and an armed GPIO gesture.
    let mut model = controls_model();
    model.base_url = "http://127.0.0.1:0".to_string();
    model.saved_config.busy = Some(ConfigJobKind::Save);
    model.hardware_confirm = Some(power_confirmation());
    model.gpio_names.push("GP10".to_string());
    model.gpio_gesture.down(
        GpioGestureInput {
            pin: Some("GP10"),
            column: 2,
            row: 2,
        },
        Instant::now(),
    );
    let confirmation = model.hardware_confirm.clone();
    let gesture = model.gpio_gesture.clone();
    draw_sized(&mut model, 80, 12)?;
    let cancel = model
        .hit_map
        .hardware_modal
        .iter()
        .find(|(_, target)| **target == HardwareModalTarget::cancel())
        .map(|(rect, _)| *rect)
        .ok_or_else(|| anyhow!("missing hardware cancel"))?;

    // When the modal Cancel hit receives a mouse Down.
    handle_mouse_at(&mut model, left_down(cancel), Instant::now())?;
    // Then busy priority leaves both modal and gesture state untouched.
    assert!(model.hardware_confirm.is_some());
    assert_eq!(
        model.hardware_confirm.as_ref().map(|c| &c.command),
        confirmation.as_ref().map(|c| &c.command)
    );
    assert_eq!(model.gpio_gesture, gesture);
    Ok(())
}

#[test]
fn busy_keyboard_guard_keeps_hardware_modal_unchanged_after_redraw() -> Result<()> {
    // Given a busy frame with an already expired hardware confirmation.
    let mut model = controls_model();
    model.saved_config.busy = Some(ConfigJobKind::Save);
    model.hardware_confirm = Some(HardwareConfirmation {
        command: ConfirmableCommand::SetPower {
            output: "12v_out".to_string(),
            next_state: true,
        },
        started: Instant::now() - CONFIRM_TIMEOUT,
    });
    let confirmation = model.hardware_confirm.clone();
    draw_sized(&mut model, 80, 12)?;

    // When Enter reaches the modal while the config worker is busy.
    let outcome = handle_key(
        &mut model,
        crossterm::event::KeyEvent::new(KeyCode::Enter, KeyModifiers::NONE),
        Instant::now(),
    )?;

    // Then the global busy guard wins before expiry or hardware execution.
    assert_eq!(outcome, KeyOutcome::Continue);
    assert_eq!(
        model.hardware_confirm.as_ref().map(|c| &c.command),
        confirmation.as_ref().map(|c| &c.command)
    );
    Ok(())
}

#[test]
fn busy_keyboard_guard_still_allows_quit_keys() -> Result<()> {
    // Given Saved Config is busy.
    for (code, modifiers) in [
        (KeyCode::Char('q'), KeyModifiers::NONE),
        (KeyCode::Char('c'), KeyModifiers::CONTROL),
    ] {
        let mut model = controls_model();
        model.saved_config.busy = Some(ConfigJobKind::Save);

        // When q or Ctrl-C is pressed.
        let outcome = handle_key(
            &mut model,
            crossterm::event::KeyEvent::new(code, modifiers),
            Instant::now(),
        )?;

        // Then the existing exit contract remains available.
        assert_eq!(outcome, KeyOutcome::Exit);
        assert!(model.closed);
    }
    Ok(())
}
