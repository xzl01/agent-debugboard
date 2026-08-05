use super::config_state::{ConfigConfirmation, ConfigRequest, SavedConfigState};
use crate::persistent_config::{ConfigAction, PersistentConfigResponse, PersistentConfigStatus};

const SHOW: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":1},"pending":1,"items":[{"id":"power/alpha","kind":"power","current":{"state":"off"},"saved":{"state":"on"},"selected":false,"requires_confirm":true,"apply_state":"pending"},{"id":"switch/beta","kind":"switch","current":{"route":"pc"},"saved":{"route":"target"},"selected":true,"requires_confirm":false,"apply_state":"applied"}]}"#;
const REFRESHED: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":1},"pending":1,"items":[{"id":"power/alpha","kind":"power","current":{"state":"off"},"saved":{"state":"on"},"selected":false,"requires_confirm":true,"apply_state":"pending"},{"id":"switch/beta","kind":"switch","current":{"route":"pc"},"saved":{"route":"target"},"selected":false,"requires_confirm":false,"apply_state":"applied"}]}"#;

fn show(raw: &str) -> PersistentConfigResponse {
    let response = PersistentConfigResponse::from_raw(raw.to_string()).unwrap();
    response.validate(&ConfigAction::Get, None).unwrap();
    response
}

fn ready_state() -> SavedConfigState {
    state(SHOW, 2, 1)
}

fn state(raw: &str, saved_count: u32, pending_count: u32) -> SavedConfigState {
    let mut state = SavedConfigState::default();
    assert!(state.observe_summary(Some(PersistentConfigStatus {
        available: true,
        reason: "ready".to_string(),
        saved_count,
        pending_count,
    })));
    state.apply_authoritative(show(raw)).unwrap();
    state
}

#[test]
fn preserves_local_selection_when_refresh_retains_firmware_id() {
    let mut state = ready_state();
    state.focus();
    state.toggle_current();

    state.apply_authoritative(show(REFRESHED)).unwrap();

    assert_eq!(state.selected_ids(), ["power/alpha", "switch/beta"]);
}

#[test]
fn dangerous_save_requires_modal_before_confirmed_request() {
    let mut state = ready_state();
    state.focus();
    state.toggle_current();

    let request = state.request_save();

    assert!(request.is_none());
    assert!(matches!(
        state.confirmation(),
        Some(ConfigConfirmation::Save { dangerous, .. })
            if dangerous.iter().map(|id| id.as_str()).collect::<Vec<_>>() == ["power/alpha"]
    ));
    assert!(matches!(
        state.confirm(),
        Some(ConfigRequest::Save { confirm: true, .. })
    ));
}

#[test]
fn safe_save_emits_unconfirmed_request() {
    let mut state = ready_state();

    let request = state.request_save();

    assert!(matches!(
        request,
        Some(ConfigRequest::Save { items, confirm: false })
            if items.iter().map(|id| id.as_str()).collect::<Vec<_>>() == ["switch/beta"]
    ));
}

#[test]
fn dismiss_error_preserves_selection() {
    let mut state = ready_state();
    state.error = Some("storage_error".to_string());
    let selected = state.selected_ids();

    state.dismiss_error();

    assert!(state.error.is_none());
    assert_eq!(state.selected_ids(), selected);
}

#[test]
fn summary_change_requests_refresh_and_missing_summary_marks_unsupported() {
    let mut state = SavedConfigState::default();
    let summary = PersistentConfigStatus {
        available: true,
        reason: "ready".to_string(),
        saved_count: 1,
        pending_count: 0,
    };

    assert!(state.observe_summary(Some(summary.clone())));
    assert!(!state.observe_summary(Some(summary)));
    assert!(state.observe_summary(Some(PersistentConfigStatus {
        available: true,
        reason: "ready".to_string(),
        saved_count: 1,
        pending_count: 1,
    })));
    assert!(!state.observe_summary(None));
    assert!(!state.is_supported());
}

#[test]
fn missing_summary_clears_authoritative_state_and_blocks_mutations() {
    let mut state = ready_state();
    state.focus();
    state.toggle_current();
    assert!(state.request_save().is_none());
    assert!(state.confirmation().is_some());

    state.observe_summary(None);

    assert!(!state.is_supported());
    assert!(!state.loaded);
    assert!(state.items.is_empty());
    assert!(state.selected_ids().is_empty());
    assert_eq!(state.cursor, 0);
    assert!(!state.focused);
    assert!(state.confirmation().is_none());
    assert!(state.request_save().is_none());
    assert!(state.request_clear().is_none());
    assert!(state.confirm().is_none());
    assert!(state.confirmation().is_none());
}

#[test]
fn restored_summary_requires_authoritative_get_before_mutations() {
    let mut state = ready_state();
    state.observe_summary(None);
    state.observe_summary(Some(PersistentConfigStatus {
        available: true,
        reason: "ready".to_string(),
        saved_count: 2,
        pending_count: 1,
    }));

    assert!(state.is_supported());
    assert!(!state.loaded);
    assert!(state.items.is_empty());
    assert!(state.selected_ids().is_empty());
    assert!(!state.focused);
    assert!(state.request_save().is_none());
    assert!(state.request_clear().is_none());
    assert!(state.confirm().is_none());
}

#[test]
fn clear_request_is_available_only_after_get() {
    let mut state = SavedConfigState::default();
    assert!(state.request_clear().is_none());

    state.observe_summary(Some(PersistentConfigStatus {
        available: true,
        reason: "ready".to_string(),
        saved_count: 0,
        pending_count: 0,
    }));
    state.apply_authoritative(show(SHOW)).unwrap();

    assert!(matches!(state.request_clear(), Some(ConfigRequest::Clear)));
}
