use super::confirm::ConfirmableCommand;
use super::controls::control_targets;
use super::events::{handle_key, KeyOutcome};
use super::events_fixture::{model_with_switch, press};
use super::gpio_fixture::current_power_outputs;
use super::model::TuiModel;
use super::pages::ActivePage;
use crate::client::DEFAULT_BASE_URL;
use crate::ws_status::{TuiStatusSwitchInfo, WsStatusSnapshot};
use crossterm::event::{KeyCode, KeyEvent, KeyEventKind, KeyEventState, KeyModifiers};
use std::time::{Duration, Instant};

#[test]
fn g_jumps_to_first_gpio_in_unified_grid() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.gpio_names = vec!["GP13".to_string(), "GP14".to_string()];
    press(&mut model, KeyCode::Char('g'));
    assert_eq!(
        model.control_idx,
        control_targets(&model).len() + model.switches.len()
    );
}

#[test]
fn power_activation_opens_confirmation_instead_of_toggling() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.apply_status_snapshot(WsStatusSnapshot {
        power_outputs: current_power_outputs(),
        ..Default::default()
    });
    model.control_idx = 0;

    press(&mut model, KeyCode::Enter);

    assert_eq!(
        model
            .hardware_confirm
            .as_ref()
            .map(|confirm| &confirm.command),
        Some(&ConfirmableCommand::SetPower {
            output: "12v_out".to_string(),
            next_state: true,
        })
    );
    assert_eq!(model.power_states.get("12v_out"), Some(&false));
    assert!(model.status.contains("12v_out"));
}

#[test]
fn confirmation_modal_blocks_other_keys_and_esc_cancels() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.apply_status_snapshot(WsStatusSnapshot {
        power_outputs: current_power_outputs(),
        ..Default::default()
    });
    model.control_idx = 0;
    press(&mut model, KeyCode::Enter);
    assert!(model.hardware_confirm.is_some());

    press(&mut model, KeyCode::Down);
    assert_eq!(model.control_idx, 0, "modal must block navigation keys");
    assert!(model.hardware_confirm.is_some());

    assert_eq!(
        handle_key(
            &mut model,
            KeyEvent::new(KeyCode::Esc, KeyModifiers::NONE),
            Instant::now(),
        )
        .unwrap(),
        KeyOutcome::Redraw
    );
    assert!(model.hardware_confirm.is_none());
    assert_eq!(model.status, "Power toggle cancelled");
}

#[test]
fn tab_cycles_pages_and_shift_tab_reverses() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    assert_eq!(model.active_page, ActivePage::Controls);

    assert_eq!(
        handle_key(
            &mut model,
            KeyEvent::new(KeyCode::Tab, KeyModifiers::NONE),
            Instant::now(),
        )
        .unwrap(),
        KeyOutcome::Redraw
    );
    assert_eq!(model.active_page, ActivePage::SavedConfig);
    press(&mut model, KeyCode::Tab);
    assert_eq!(model.active_page, ActivePage::Status);
    press(&mut model, KeyCode::Tab);
    assert_eq!(model.active_page, ActivePage::Controls);

    handle_key(
        &mut model,
        KeyEvent::new(KeyCode::BackTab, KeyModifiers::SHIFT),
        Instant::now(),
    )
    .unwrap();
    assert_eq!(model.active_page, ActivePage::Status);
}

#[test]
fn tab_is_ignored_while_hardware_confirmation_is_open() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.apply_status_snapshot(WsStatusSnapshot {
        power_outputs: current_power_outputs(),
        ..Default::default()
    });
    model.control_idx = 0;
    press(&mut model, KeyCode::Enter);
    assert!(model.hardware_confirm.is_some());

    press(&mut model, KeyCode::Tab);

    assert_eq!(model.active_page, ActivePage::Controls);
    assert!(model.hardware_confirm.is_some());
}

#[test]
fn down_crosses_sections_and_right_stays_through_key_handling() {
    let mut model = model_with_switch();

    model.control_idx = control_targets(&model).len() - 1;
    press(&mut model, KeyCode::Down);
    assert_eq!(model.control_idx, control_targets(&model).len());
    press(&mut model, KeyCode::Down);
    assert_eq!(model.control_idx, control_targets(&model).len());
    // Up from the switch row moves one object row back into the power section.
    press(&mut model, KeyCode::Up);
    assert_eq!(model.control_idx, control_targets(&model).len() - 1);
    model.control_idx = control_targets(&model).len() - 1;
    press(&mut model, KeyCode::Right);
    assert_eq!(model.control_idx, control_targets(&model).len() - 1);
}

#[test]
fn status_page_up_down_scroll_without_moving_selection() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    press(&mut model, KeyCode::Tab);
    press(&mut model, KeyCode::Tab);
    assert_eq!(model.active_page, ActivePage::Status);

    press(&mut model, KeyCode::Down);
    press(&mut model, KeyCode::Down);
    assert_eq!(model.page_scroll(), 2);
    press(&mut model, KeyCode::Up);
    assert_eq!(model.page_scroll(), 1);
    assert_eq!(model.control_idx, 0);

    press(&mut model, KeyCode::PageDown);
    assert_eq!(model.page_scroll(), 4);
    press(&mut model, KeyCode::Char('['));
    assert_eq!(model.page_scroll(), 1);
}

#[test]
fn page_keys_move_selection_on_the_controls_page() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.apply_status_snapshot(WsStatusSnapshot {
        power_outputs: current_power_outputs(),
        ..Default::default()
    });
    // Narrow layout: one power chip per rendered row.
    model.width = 24;

    press(&mut model, KeyCode::PageDown);
    assert_eq!(model.control_idx, 3);
    press(&mut model, KeyCode::Char('['));
    assert_eq!(model.control_idx, 0);
    press(&mut model, KeyCode::Char(']'));
    assert_eq!(model.control_idx, 3);
}

#[test]
fn enter_on_the_status_page_does_not_activate_anything() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    press(&mut model, KeyCode::Tab);
    press(&mut model, KeyCode::Tab);

    press(&mut model, KeyCode::Enter);

    assert!(model.hardware_confirm.is_none());
    assert_eq!(model.status, "HTTP mode");
}

#[test]
fn keyboard_activation_confirms_a_switch_without_the_firmware_risk_flag() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    let mut snapshot = WsStatusSnapshot::default();
    snapshot.switches.insert(
        "tf_wp".to_string(),
        TuiStatusSwitchInfo {
            route: "writable".to_string(),
            routes: vec!["writable".to_string(), "protected".to_string()],
            requires_confirm: false,
        },
    );
    model.apply_status_snapshot(snapshot);
    model.control_idx = control_targets(&model).len();

    press(&mut model, KeyCode::Enter);

    assert_eq!(
        model
            .hardware_confirm
            .as_ref()
            .map(|confirm| &confirm.command),
        Some(&ConfirmableCommand::RouteSwitch {
            name: "tf_wp".to_string(),
            route: "protected".to_string(),
        })
    );
}

#[test]
fn quit_key_closes_tui() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    let quit = handle_key(
        &mut model,
        KeyEvent::new(KeyCode::Char('c'), KeyModifiers::CONTROL),
        Instant::now(),
    )
    .unwrap();

    assert_eq!(quit, KeyOutcome::Exit);
    assert!(model.closed);
}

#[test]
fn non_press_key_events_are_ignored() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    let outcome = handle_key(
        &mut model,
        KeyEvent {
            code: KeyCode::Char('c'),
            modifiers: KeyModifiers::CONTROL,
            kind: KeyEventKind::Release,
            state: KeyEventState::NONE,
        },
        Instant::now(),
    )
    .unwrap();

    assert_eq!(outcome, KeyOutcome::Continue);
    assert!(!model.closed);

    let before_idx = model.control_idx;
    handle_key(
        &mut model,
        KeyEvent {
            code: KeyCode::Right,
            modifiers: KeyModifiers::NONE,
            kind: KeyEventKind::Repeat,
            state: KeyEventState::NONE,
        },
        Instant::now(),
    )
    .unwrap();

    assert_eq!(model.control_idx, before_idx);
}
