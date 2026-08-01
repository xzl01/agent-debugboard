use super::config_result::ConfigJobKind;
use super::config_state::ConfigConfirmation;
use super::{handle_key, TuiModel};
use crate::persistent_config::{ConfigAction, PersistentConfigResponse, PersistentConfigStatus};
use crate::ws_client::WsStatusSnapshot;
use crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
use std::time::Duration;

const SHOW: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":1},"pending":1,"items":[{"id":"power/alpha","kind":"power","current":{"state":"off"},"saved":{"state":"on"},"selected":true,"requires_confirm":true,"apply_state":"pending"},{"id":"switch/beta","kind":"switch","current":{"route":"pc"},"saved":{"route":"target"},"selected":false,"requires_confirm":false,"apply_state":"applied"}]}"#;

fn model() -> TuiModel {
    let response = PersistentConfigResponse::from_raw(SHOW.to_string()).unwrap();
    response.validate(&ConfigAction::Get, None).unwrap();
    let mut model = TuiModel::new("http://127.0.0.1:0".to_string(), Duration::from_millis(100));
    model
        .saved_config
        .observe_summary(Some(PersistentConfigStatus {
            available: true,
            reason: "ready".to_string(),
            saved_count: 1,
            pending_count: 1,
        }));
    model.saved_config.apply_authoritative(response).unwrap();
    model
}

fn press(model: &mut TuiModel, code: KeyCode) {
    press_with_modifiers(model, code, KeyModifiers::NONE);
}

fn press_with_modifiers(model: &mut TuiModel, code: KeyCode, modifiers: KeyModifiers) -> bool {
    handle_key(model, KeyEvent::new(code, modifiers)).unwrap()
}

#[test]
fn config_focus_arrows_and_space_preserve_hardware_cursor() {
    let mut model = model();
    let hardware_cursor = model.control_idx;

    press(&mut model, KeyCode::Char('c'));
    press(&mut model, KeyCode::Down);
    press(&mut model, KeyCode::Char(' '));

    assert!(model.saved_config.focused);
    assert_eq!(model.saved_config.cursor, 1);
    assert_eq!(
        model.saved_config.selected_ids(),
        ["power/alpha", "switch/beta"]
    );
    assert_eq!(model.control_idx, hardware_cursor);
}

#[test]
fn lowercase_c_focuses_and_blurs_saved_config() {
    let mut model = model();

    press(&mut model, KeyCode::Char('c'));
    assert!(model.saved_config.focused);

    press(&mut model, KeyCode::Char('c'));
    assert!(!model.saved_config.focused);
    assert!(!model.closed);
}

#[test]
fn ctrl_c_quits_while_saved_config_is_focused() {
    let mut model = model();
    press(&mut model, KeyCode::Char('c'));

    let quit = press_with_modifiers(&mut model, KeyCode::Char('c'), KeyModifiers::CONTROL);

    assert!(quit);
    assert!(model.closed);
}

#[test]
fn ctrl_c_quits_while_save_confirmation_is_open() {
    let mut model = model();
    press(&mut model, KeyCode::Char('s'));

    let quit = press_with_modifiers(&mut model, KeyCode::Char('c'), KeyModifiers::CONTROL);

    assert!(quit);
    assert!(model.closed);
    assert!(matches!(
        model.saved_config.confirmation(),
        Some(ConfigConfirmation::Save { .. })
    ));
}

#[test]
fn ctrl_c_quits_while_apply_confirmation_is_open() {
    let mut model = model();
    press(&mut model, KeyCode::Char('a'));

    let quit = press_with_modifiers(&mut model, KeyCode::Char('c'), KeyModifiers::CONTROL);

    assert!(quit);
    assert!(model.closed);
    assert!(matches!(
        model.saved_config.confirmation(),
        Some(ConfigConfirmation::Apply { .. })
    ));
}

#[test]
fn ctrl_c_quits_while_saved_config_error_is_visible() {
    let mut model = model();
    model.saved_config.error = Some("storage_error".to_string());

    let quit = press_with_modifiers(&mut model, KeyCode::Char('c'), KeyModifiers::CONTROL);

    assert!(quit);
    assert!(model.closed);
    assert_eq!(model.saved_config.error.as_deref(), Some("storage_error"));
}

#[test]
fn q_quits_while_saved_config_confirmation_is_open() {
    let mut model = model();
    press(&mut model, KeyCode::Char('s'));

    let quit = press_with_modifiers(&mut model, KeyCode::Char('q'), KeyModifiers::NONE);

    assert!(quit);
    assert!(model.closed);
}

#[test]
fn save_and_apply_keys_open_separate_danger_confirmations() {
    let mut model = model();

    press(&mut model, KeyCode::Char('s'));
    assert!(matches!(
        model.saved_config.confirmation(),
        Some(ConfigConfirmation::Save { .. })
    ));
    press(&mut model, KeyCode::Esc);
    assert!(model.saved_config.confirmation().is_none());

    press(&mut model, KeyCode::Char('a'));
    assert!(matches!(
        model.saved_config.confirmation(),
        Some(ConfigConfirmation::Apply { .. })
    ));
    press(&mut model, KeyCode::Enter);
    assert!(model.saved_config.confirmation().is_none());
    assert_eq!(model.saved_config.busy, Some(ConfigJobKind::Apply));
}

#[test]
fn escape_dismisses_error_without_losing_selection() {
    let mut model = model();
    model.saved_config.error = Some("storage_error".to_string());
    let selected = model.saved_config.selected_ids();

    press(&mut model, KeyCode::Esc);

    assert!(model.saved_config.error.is_none());
    assert_eq!(model.saved_config.selected_ids(), selected);
}

#[test]
fn clear_key_starts_one_clear_job_after_authoritative_get() {
    let mut model = model();

    press(&mut model, KeyCode::Char('x'));

    assert_eq!(model.saved_config.busy, Some(ConfigJobKind::Clear));
}

#[test]
fn status_or_ws_summary_change_requests_authoritative_get() {
    let mut model = TuiModel::new("http://127.0.0.1:0".to_string(), Duration::from_millis(100));
    let summary = PersistentConfigStatus {
        available: true,
        reason: "ready".to_string(),
        saved_count: 1,
        pending_count: 0,
    };

    assert!(model.apply_status_snapshot(WsStatusSnapshot {
        config: Some(summary.clone()),
        ..Default::default()
    }));
    assert!(!model.apply_status_snapshot(WsStatusSnapshot {
        config: Some(summary),
        ..Default::default()
    }));
    assert!(model.apply_status_snapshot(WsStatusSnapshot {
        config: Some(PersistentConfigStatus {
            available: true,
            reason: "ready".to_string(),
            saved_count: 1,
            pending_count: 1,
        }),
        ..Default::default()
    }));
}
