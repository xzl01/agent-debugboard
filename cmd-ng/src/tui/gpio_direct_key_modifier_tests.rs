use super::events::{handle_key, KeyOutcome};
use super::gpio_fixture::{item_index, projection_model};
use super::gpio_gesture::GpioGestureInput;
use super::gpio_io::{GpioAction, GpioJob};
use super::model::TuiModel;
use anyhow::Result;
use crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
use std::time::Instant;

fn model_with_active_gpio_gesture() -> Result<TuiModel> {
    let mut model = projection_model()?;
    model.control_idx = item_index(&model, "GP10")?;
    let start = Instant::now();
    let _ = model.gpio_gesture.down(
        GpioGestureInput {
            pin: Some("GP10"),
            column: 0,
            row: 0,
        },
        start,
    );
    Ok(model)
}

fn assert_modified_key_is_inert(code: KeyCode, modifiers: KeyModifiers) -> Result<()> {
    let mut model = model_with_active_gpio_gesture()?;
    let outcome = handle_key(&mut model, KeyEvent::new(code, modifiers), Instant::now())?;

    assert_eq!(outcome, KeyOutcome::Continue);
    assert_eq!(model.gpio_pending, None);
    assert_eq!(model.gpio_gesture.holding_pin(), Some("GP10"));
    Ok(())
}

fn assert_shifted_uppercase_key_dispatches(code: KeyCode, action: GpioAction) -> Result<()> {
    let mut model = model_with_active_gpio_gesture()?;
    let outcome = handle_key(
        &mut model,
        KeyEvent::new(code, KeyModifiers::SHIFT),
        Instant::now(),
    )?;

    assert_eq!(outcome, KeyOutcome::Redraw);
    assert_eq!(
        model.gpio_pending,
        Some(GpioJob {
            action,
            target: "GP10".to_string()
        })
    );
    assert_eq!(model.gpio_gesture.holding_pin(), None);
    Ok(())
}

#[test]
fn shifted_uppercase_direct_gpio_keys_remain_valid() -> Result<()> {
    for (code, action) in [
        (KeyCode::Char('L'), GpioAction::DriveLow),
        (KeyCode::Char('O'), GpioAction::DriveHigh),
        (KeyCode::Char('I'), GpioAction::SetInput),
    ] {
        assert_shifted_uppercase_key_dispatches(code, action)?;
    }
    Ok(())
}

#[test]
fn control_alt_super_modified_direct_gpio_keys_are_inert() -> Result<()> {
    for modifiers in [
        KeyModifiers::CONTROL,
        KeyModifiers::ALT,
        KeyModifiers::SUPER,
        KeyModifiers::CONTROL | KeyModifiers::ALT,
        KeyModifiers::CONTROL | KeyModifiers::SUPER,
        KeyModifiers::ALT | KeyModifiers::SUPER,
        KeyModifiers::CONTROL | KeyModifiers::ALT | KeyModifiers::SUPER,
        KeyModifiers::CONTROL | KeyModifiers::ALT | KeyModifiers::SHIFT,
        KeyModifiers::CONTROL | KeyModifiers::SUPER | KeyModifiers::SHIFT,
        KeyModifiers::ALT | KeyModifiers::SUPER | KeyModifiers::SHIFT,
        KeyModifiers::CONTROL | KeyModifiers::ALT | KeyModifiers::SUPER | KeyModifiers::SHIFT,
    ] {
        for code in [KeyCode::Char('l'), KeyCode::Char('o'), KeyCode::Char('i')] {
            assert_modified_key_is_inert(code, modifiers)?;
        }
    }
    Ok(())
}

#[test]
fn uppercase_without_shift_direct_gpio_keys_are_inert_when_represented() -> Result<()> {
    for code in [KeyCode::Char('L'), KeyCode::Char('O'), KeyCode::Char('I')] {
        assert_modified_key_is_inert(code, KeyModifiers::NONE)?;
    }
    Ok(())
}

#[test]
fn lowercase_with_shift_direct_gpio_keys_are_inert_when_represented() -> Result<()> {
    for code in [KeyCode::Char('l'), KeyCode::Char('o'), KeyCode::Char('i')] {
        assert_modified_key_is_inert(code, KeyModifiers::SHIFT)?;
    }
    Ok(())
}
