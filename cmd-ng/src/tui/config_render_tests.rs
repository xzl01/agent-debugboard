use super::config_render::{append_saved_config_lines, render_confirmation};
use super::config_state::SavedConfigState;
use crate::persistent_config::{ConfigAction, PersistentConfigResponse, PersistentConfigStatus};
use ratatui::backend::TestBackend;
use ratatui::text::Text;
use ratatui::Terminal;

const SHOW: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":1},"pending":1,"items":[{"id":"power/alpha","kind":"power","current":{"state":"off"},"saved":{"state":"on"},"selected":true,"requires_confirm":true,"apply_state":"pending"}]}"#;

fn ready_state() -> SavedConfigState {
    let response = PersistentConfigResponse::from_raw(SHOW.to_string()).unwrap();
    response.validate(&ConfigAction::Get, None).unwrap();
    let mut state = SavedConfigState::default();
    state.observe_summary(Some(PersistentConfigStatus {
        available: true,
        reason: "ready".to_string(),
        saved_count: 1,
        pending_count: 1,
    }));
    state.apply_authoritative(response).unwrap();
    state.focus();
    state
}

#[test]
fn rendered_row_exposes_firmware_values_selection_risk_apply_and_badges() {
    let mut state = ready_state();
    state.error = Some("storage_error".to_string());
    let mut lines = Vec::new();

    append_saved_config_lines(&mut lines, &state, 120);

    let rendered = Text::from(lines).to_string();
    for token in [
        "power/alpha",
        "current=off",
        "saved=on",
        "[x]",
        "risk=confirm",
        "apply=pending",
        "[pending:1]",
        "[error]",
    ] {
        assert!(
            rendered.contains(token),
            "missing token {token}: {rendered}"
        );
    }
}

#[test]
fn old_firmware_renders_unsupported_without_items() {
    let state = SavedConfigState::default();
    let mut lines = Vec::new();

    append_saved_config_lines(&mut lines, &state, 32);

    let rendered = Text::from(lines).to_string();
    assert!(rendered.contains("[unsupported]"));
    assert!(!rendered.contains("power/"));
}

#[test]
fn first_refresh_disconnect_renders_error_detail_instead_of_loading() {
    let mut state = SavedConfigState::default();
    state.observe_summary(Some(PersistentConfigStatus {
        available: true,
        reason: "ready".to_string(),
        saved_count: 0,
        pending_count: 0,
    }));
    state.error = Some("disconnect".to_string());
    let mut lines = Vec::new();

    append_saved_config_lines(&mut lines, &state, 48);

    let rendered = Text::from(lines).to_string();
    assert!(rendered.contains("[error]"));
    assert!(rendered.contains("error=disconnect"));
    assert!(!rendered.contains("[loading]"));
    assert!(!rendered.contains("[unavailable]"));
}

#[test]
fn compact_terminal_clips_confirmation_without_panicking() {
    let mut state = ready_state();
    assert!(state.request_save().is_none());
    let backend = TestBackend::new(20, 7);
    let mut terminal = Terminal::new(backend).unwrap();

    terminal
        .draw(|frame| render_confirmation(frame, &state))
        .unwrap();

    assert_eq!(terminal.backend().buffer().area.width, 20);
    assert_eq!(terminal.backend().buffer().area.height, 7);
}

#[test]
fn save_confirmation_names_the_dangerous_firmware_ids() {
    let mut save = ready_state();
    assert!(save.request_save().is_none());
    let save_text = super::config_render::confirmation_text(&save).unwrap();

    assert!(save_text.contains("SAVE"));
    assert!(save_text.contains("power/alpha"));
}
