use super::config_result::{ConfigJobKind, ConfigJobResult, ConfigOutcome};
use super::config_state::{ConfigConfirmation, ConfigRequest, SavedConfigState};
use crate::persistent_config::{ConfigAction, ConfigItemId, PersistentConfigResponse};

const SHOW: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":1},"pending":0,"items":[{"id":"power/alpha","kind":"power","current":{"state":"off"},"saved":{"state":"on"},"selected":true,"requires_confirm":true,"apply_state":"applied"}]}"#;
const SAVE: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"save","saved_items":["power/alpha"],"confirmation_items":["power/alpha"],"applied_items":["power/alpha"],"snapshot":{"present":true,"version":1},"pending":0}"#;
const CONFIRM: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"save","error":{"code":"confirmation_required","message":"confirm"},"dangerous_items":["power/alpha"]}"#;
const BUSY: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"save","error":{"code":"busy","message":"blocked"},"activity":"capture"}"#;
const STORAGE: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"clear","error":{"code":"storage_error","message":"flash"}}"#;

fn response(raw: &str, action: ConfigAction, status: u16) -> PersistentConfigResponse {
    let response = PersistentConfigResponse::from_raw(raw.to_string()).unwrap();
    response.validate(&action, Some(status)).unwrap();
    response
}

fn loaded_state() -> SavedConfigState {
    let mut state = SavedConfigState::default();
    state
        .apply_authoritative(response(SHOW, ConfigAction::Get, 200))
        .unwrap();
    state
}

#[test]
fn successful_mutation_becomes_visible_only_after_authoritative_get() {
    let mut state = loaded_state();
    state.pending = 1;
    let request = ConfigRequest::Save {
        items: vec![ConfigItemId("power/alpha".to_string())],
        confirm: true,
    };
    state.start(ConfigJobKind::Save);

    let outcome = state.finish(ConfigJobResult::mutation(
        request,
        Ok(response(SAVE, ConfigAction::Save, 200)),
        Ok(response(SHOW, ConfigAction::Get, 200)),
    ));

    assert_eq!(outcome, ConfigOutcome::Saved);
    assert_eq!(state.pending, 0);
    assert!(state.error.is_none());
    assert!(state.busy.is_none());
}

#[test]
fn confirmation_error_uses_server_dangerous_ids_without_retrying() {
    let mut state = loaded_state();
    let request = ConfigRequest::Save {
        items: vec![ConfigItemId("power/alpha".to_string())],
        confirm: false,
    };
    state.start(ConfigJobKind::Save);

    let outcome = state.finish(ConfigJobResult::mutation(
        request,
        Ok(response(CONFIRM, ConfigAction::Save, 409)),
        Ok(response(SHOW, ConfigAction::Get, 200)),
    ));

    assert_eq!(outcome, ConfigOutcome::AwaitingConfirmation);
    assert!(matches!(
        state.confirmation(),
        Some(ConfigConfirmation::Save { dangerous, .. })
            if dangerous.iter().map(|id| id.as_str()).collect::<Vec<_>>() == ["power/alpha"]
    ));
    assert!(state.error.is_none());
}

#[test]
fn busy_and_storage_errors_keep_refreshed_selection_and_are_dismissible() {
    for (request, raw, action, status, code) in [
        (
            ConfigRequest::Save {
                items: vec![ConfigItemId("power/alpha".to_string())],
                confirm: true,
            },
            BUSY,
            ConfigAction::Save,
            409,
            "busy",
        ),
        (
            ConfigRequest::Clear,
            STORAGE,
            ConfigAction::Clear,
            500,
            "storage_error",
        ),
    ] {
        let mut state = loaded_state();
        state.start(request.kind());

        let outcome = state.finish(ConfigJobResult::mutation(
            request,
            Ok(response(raw, action, status)),
            Ok(response(SHOW, ConfigAction::Get, 200)),
        ));

        assert_eq!(outcome, ConfigOutcome::Failed);
        assert!(state
            .error
            .as_deref()
            .is_some_and(|error| error.contains(code)));
        assert_eq!(state.selected_ids(), ["power/alpha"]);
        state.dismiss_error();
        assert!(state.error.is_none());
    }
}

#[test]
fn successful_mutation_with_failed_get_is_not_marked_successful() {
    let mut state = loaded_state();
    state.pending = 1;
    let request = ConfigRequest::Save {
        items: vec![ConfigItemId("power/alpha".to_string())],
        confirm: true,
    };
    state.start(ConfigJobKind::Save);

    let outcome = state.finish(ConfigJobResult::mutation(
        request,
        Ok(response(SAVE, ConfigAction::Save, 200)),
        Err("disconnect".to_string()),
    ));

    assert_eq!(outcome, ConfigOutcome::Failed);
    assert_eq!(state.pending, 1);
    assert!(state
        .error
        .as_deref()
        .is_some_and(|error| error.contains("disconnect")));
}
