use super::controls::control_targets;
use super::events::handle_key;
use super::events_fixture::{model_with_switch, press};
use super::gpio_fixture::{item_index, projection_model};
use super::gpio_gesture::GpioGestureInput;
use super::gpio_io::{GpioAction, GpioJob};
use super::model::TuiModel;
use super::pages::ActivePage;
use crossterm::event::{KeyCode, KeyEvent, KeyEventKind, KeyEventState, KeyModifiers};
use std::time::{Duration, Instant};

fn gpio_key_model() -> TuiModel {
    let mut model = TuiModel::new("http://127.0.0.1:9".to_string(), Duration::from_millis(30));
    model.gpio_names = vec!["GP13".to_string()];
    model.gpio_levels.insert("GP13".to_string(), true);
    model.gpio_is_input.insert("GP13".to_string(), true);
    model
}

fn press_kind(model: &mut TuiModel, code: KeyCode, kind: KeyEventKind) {
    handle_key(
        model,
        KeyEvent {
            code,
            modifiers: KeyModifiers::NONE,
            kind,
            state: KeyEventState::NONE,
        },
        Instant::now(),
    )
    .unwrap();
}

fn press_with_modifiers(model: &mut TuiModel, code: KeyCode, modifiers: KeyModifiers) {
    handle_key(
        model,
        KeyEvent {
            code,
            modifiers,
            kind: KeyEventKind::Press,
            state: KeyEventState::NONE,
        },
        Instant::now(),
    )
    .unwrap();
}

fn expected(action: GpioAction) -> GpioJob {
    GpioJob {
        action,
        target: "GP13".to_string(),
    }
}

#[test]
fn lowercase_none_and_uppercase_shift_dispatch_the_three_direct_gpio_actions() {
    for (key, modifiers, action) in [
        (KeyCode::Char('l'), KeyModifiers::NONE, GpioAction::DriveLow),
        (
            KeyCode::Char('L'),
            KeyModifiers::SHIFT,
            GpioAction::DriveLow,
        ),
        (
            KeyCode::Char('o'),
            KeyModifiers::NONE,
            GpioAction::DriveHigh,
        ),
        (
            KeyCode::Char('O'),
            KeyModifiers::SHIFT,
            GpioAction::DriveHigh,
        ),
        (KeyCode::Char('i'), KeyModifiers::NONE, GpioAction::SetInput),
        (
            KeyCode::Char('I'),
            KeyModifiers::SHIFT,
            GpioAction::SetInput,
        ),
    ] {
        let mut model = gpio_key_model();
        press_with_modifiers(&mut model, key, modifiers);
        assert_eq!(
            model.gpio_pending,
            Some(expected(action)),
            "{key:?} with {modifiers:?} must dispatch {action:?}"
        );
        assert_eq!(model.gpio_levels.get("GP13"), Some(&true));
        assert_eq!(model.gpio_is_input.get("GP13"), Some(&true));
    }
}

#[test]
fn enter_space_zero_and_one_are_inert_on_gpio() {
    for key in [
        KeyCode::Enter,
        KeyCode::Char(' '),
        KeyCode::Char('0'),
        KeyCode::Char('1'),
    ] {
        let mut model = gpio_key_model();
        press(&mut model, key);
        assert_eq!(model.gpio_pending, None, "{key:?} must stay inert on GPIO");
    }
}

#[test]
fn l_is_not_right_navigation_but_right_arrow_moves_to_the_sibling() -> anyhow::Result<()> {
    let mut model = projection_model()?;
    model.control_idx = item_index(&model, "GP10")?;
    press(&mut model, KeyCode::Char('l'));
    assert_eq!(model.control_idx, item_index(&model, "GP10")?);
    assert_eq!(
        model.gpio_pending,
        Some(GpioJob {
            action: GpioAction::DriveLow,
            target: "GP10".to_string()
        })
    );

    let mut model = projection_model()?;
    model.control_idx = item_index(&model, "GP10")?;
    press(&mut model, KeyCode::Right);
    assert_eq!(model.control_idx, item_index(&model, "GP16")?);
    Ok(())
}

#[test]
fn direct_keys_cancel_active_gestures_then_remain_inert_off_gpio_or_pages() {
    let start = Instant::now();
    let mut model = model_with_switch();
    let _ = model.gpio_gesture.down(
        GpioGestureInput {
            pin: Some("GP13"),
            column: 0,
            row: 0,
        },
        start,
    );
    model.control_idx = 0;
    press(&mut model, KeyCode::Char('o'));
    assert_eq!(model.gpio_gesture.holding_pin(), None);
    assert_eq!(model.gpio_pending, None);

    let mut model = gpio_key_model();
    let _ = model.gpio_gesture.down(
        GpioGestureInput {
            pin: Some("GP13"),
            column: 0,
            row: 0,
        },
        start,
    );
    press(&mut model, KeyCode::Tab);
    assert_eq!(model.active_page, ActivePage::SavedConfig);
    press(&mut model, KeyCode::Char('i'));
    assert_eq!(model.gpio_gesture.holding_pin(), None);
    assert_eq!(model.gpio_pending, None);
}

#[test]
fn opening_a_confirmation_modal_cancels_an_active_gesture() {
    let start = Instant::now();
    let mut model = model_with_switch();
    let _ = model.gpio_gesture.down(
        GpioGestureInput {
            pin: Some("GP13"),
            column: 0,
            row: 0,
        },
        start,
    );
    model.control_idx = 0;
    press(&mut model, KeyCode::Enter);
    assert!(model.hardware_confirm.is_some());
    assert_eq!(model.gpio_gesture.holding_pin(), None);
}

#[test]
fn repeat_and_release_are_inert_without_cancelling_a_hold() {
    let start = Instant::now();
    let mut model = gpio_key_model();
    let _ = model.gpio_gesture.down(
        GpioGestureInput {
            pin: Some("GP13"),
            column: 0,
            row: 0,
        },
        start,
    );
    press_kind(&mut model, KeyCode::Char('o'), KeyEventKind::Repeat);
    press_kind(&mut model, KeyCode::Char('i'), KeyEventKind::Release);
    assert_eq!(model.gpio_gesture.holding_pin(), Some("GP13"));
    assert_eq!(model.gpio_pending, None);
}

#[test]
fn pause_key_cancels_an_active_gesture_immediately() {
    let start = Instant::now();
    let mut model = gpio_key_model();
    let _ = model.gpio_gesture.down(
        GpioGestureInput {
            pin: Some("GP13"),
            column: 0,
            row: 0,
        },
        start,
    );
    press(&mut model, KeyCode::Char('p'));
    assert!(model.paused);
    assert_eq!(model.gpio_gesture.holding_pin(), None);
}

#[test]
fn direct_key_reports_the_existing_in_flight_gpio_job() {
    let mut model = gpio_key_model();
    press(&mut model, KeyCode::Char('l'));
    press(&mut model, KeyCode::Char('o'));
    assert_eq!(model.gpio_pending, Some(expected(GpioAction::DriveLow)));
    assert!(model.status.contains("dropped") && model.status.contains("in flight"));
}

#[test]
fn recognized_direct_keys_cancel_gestures_on_pages_and_modals() {
    let start = Instant::now();
    let mut model = gpio_key_model();
    press(&mut model, KeyCode::Tab);
    let _ = model.gpio_gesture.down(
        GpioGestureInput {
            pin: Some("GP13"),
            column: 0,
            row: 0,
        },
        start,
    );
    press(&mut model, KeyCode::Char('l'));
    assert_eq!(model.gpio_gesture.holding_pin(), None);

    let mut model = model_with_switch();
    model.control_idx = 0;
    press(&mut model, KeyCode::Enter);
    let _ = model.gpio_gesture.down(
        GpioGestureInput {
            pin: Some("GP13"),
            column: 0,
            row: 0,
        },
        start,
    );
    press(&mut model, KeyCode::Char('o'));
    assert_eq!(model.gpio_gesture.holding_pin(), None);
}

#[test]
fn direct_gpio_keys_are_inert_on_power_and_switch_rows() {
    let mut model = model_with_switch();
    model.base_url = "http://127.0.0.1:9".to_string();
    for index in [0, control_targets(&model).len()] {
        model.control_idx = index;
        for key in [KeyCode::Char('l'), KeyCode::Char('o'), KeyCode::Char('i')] {
            press(&mut model, key);
            assert_eq!(model.gpio_pending, None);
            assert!(model.hardware_confirm.is_none());
        }
    }
}
