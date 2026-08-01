use crate::persistent_config::{ConfigAction, PersistentConfigResponse};

#[test]
fn known_item_values_reject_kind_mismatches_and_invalid_primitives() {
    let cases = [
        r#"{"id":"power/12v_out","kind":"power","current":{"route":"target"}}"#,
        r#"{"id":"power/12v_out","kind":"power","current":{"state":"standby"}}"#,
        r#"{"id":"gpio/GP7","kind":"gpio","current":{"direction":"high_z","value":0}}"#,
        r#"{"id":"gpio/GP7","kind":"gpio","current":{"direction":"output","value":2}}"#,
    ];

    for item in cases {
        let response = format!(
            r#"{{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","items":[{item}]}}"#
        );
        assert!(
            PersistentConfigResponse::from_raw(response).is_err(),
            "{item}"
        );
    }
}

#[test]
fn config_action_and_http_status_must_match_the_operation() {
    for (body, status) in [
        (
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"save"}"#,
            500,
        ),
        (
            r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"apply","error":{"code":"busy","message":"blocked"}}"#,
            200,
        ),
        (
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config"}"#,
            200,
        ),
        (
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"save"}"#,
            200,
        ),
    ] {
        let response = PersistentConfigResponse::from_raw(body.to_string()).unwrap();
        assert!(response
            .validate(&ConfigAction::Apply, Some(status))
            .is_err());
    }
}

#[test]
fn config_success_responses_require_every_action_field() {
    let cases = [
        (
            ConfigAction::Get,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","snapshot":{"present":false,"version":null},"pending":0,"items":[]}"#,
        ),
        (
            ConfigAction::Get,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"pending":0,"items":[]}"#,
        ),
        (
            ConfigAction::Get,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":false},"pending":0,"items":[]}"#,
        ),
        (
            ConfigAction::Get,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":false,"version":null},"items":[]}"#,
        ),
        (
            ConfigAction::Get,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":false,"version":null},"pending":0}"#,
        ),
        (
            ConfigAction::Save,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"save","confirmation_items":[],"snapshot":{"present":true,"version":1},"pending":0}"#,
        ),
        (
            ConfigAction::Save,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"save","saved_items":[],"snapshot":{"present":true,"version":1},"pending":0}"#,
        ),
        (
            ConfigAction::Save,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"save","saved_items":[],"confirmation_items":[],"pending":0}"#,
        ),
        (
            ConfigAction::Save,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"save","saved_items":[],"confirmation_items":[],"snapshot":{"present":true},"pending":0}"#,
        ),
        (
            ConfigAction::Save,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"save","saved_items":[],"confirmation_items":[],"snapshot":{"present":true,"version":1}}"#,
        ),
        (
            ConfigAction::Apply,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"apply","applied_items":[],"failed_item":null,"pending_items":[]}"#,
        ),
        (
            ConfigAction::Apply,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"apply","noop":true,"failed_item":null,"pending_items":[]}"#,
        ),
        (
            ConfigAction::Apply,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"apply","noop":true,"applied_items":[],"pending_items":[]}"#,
        ),
        (
            ConfigAction::Apply,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"apply","noop":true,"applied_items":[],"failed_item":null}"#,
        ),
        (
            ConfigAction::Clear,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"clear","snapshot":{"present":false,"version":null},"pending":0}"#,
        ),
        (
            ConfigAction::Clear,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"clear","noop":true,"pending":0}"#,
        ),
        (
            ConfigAction::Clear,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"clear","noop":true,"snapshot":{"present":false},"pending":0}"#,
        ),
        (
            ConfigAction::Clear,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"clear","noop":true,"snapshot":{"present":false,"version":null}}"#,
        ),
    ];

    for (action, body) in cases {
        let response = PersistentConfigResponse::from_raw(body.to_string()).unwrap();
        assert!(response.validate(&action, Some(200)).is_err(), "{body}");
    }
}

#[test]
fn config_get_requires_every_frozen_catalog_row_field() {
    let rows = [
        r#"{"kind":"power","current":null,"saved":null,"selected":false,"requires_confirm":null,"apply_state":"not_saved"}"#,
        r#"{"id":"power/test","current":null,"saved":null,"selected":false,"requires_confirm":null,"apply_state":"not_saved"}"#,
        r#"{"id":"power/test","kind":"power","saved":null,"selected":false,"requires_confirm":null,"apply_state":"not_saved"}"#,
        r#"{"id":"power/test","kind":"power","current":null,"selected":false,"requires_confirm":null,"apply_state":"not_saved"}"#,
        r#"{"id":"power/test","kind":"power","current":null,"saved":null,"requires_confirm":null,"apply_state":"not_saved"}"#,
        r#"{"id":"power/test","kind":"power","current":null,"saved":null,"selected":false,"apply_state":"not_saved"}"#,
        r#"{"id":"power/test","kind":"power","current":null,"saved":null,"selected":false,"requires_confirm":null}"#,
    ];

    for row in rows {
        let body = format!(
            r#"{{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{{"available":true,"reason":"ready"}},"snapshot":{{"present":false,"version":null}},"pending":0,"items":[{row}]}}"#
        );
        let result = PersistentConfigResponse::from_raw(body)
            .and_then(|response| response.validate(&ConfigAction::Get, Some(200)));
        assert!(result.is_err(), "{row}");
    }
}

#[test]
fn config_known_errors_require_their_typed_details() {
    let cases = [
        (
            ConfigAction::Clear,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"clear"}"#,
        ),
        (
            ConfigAction::Clear,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"clear","error":{"code":"future_code","message":""}}"#,
        ),
        (
            ConfigAction::Save,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"save","error":{"code":"confirmation_required","message":"confirm"}}"#,
        ),
        (
            ConfigAction::Clear,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"clear","error":{"code":"busy","message":"blocked"}}"#,
        ),
        (
            ConfigAction::Clear,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"clear","error":{"code":"busy","message":"blocked"},"activity":"future"}"#,
        ),
        (
            ConfigAction::Apply,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"apply","error":{"code":"apply_failed","message":"failed"},"failed_item":"switch/sd","pending_items":[]}"#,
        ),
        (
            ConfigAction::Apply,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"apply","error":{"code":"apply_failed","message":"failed"},"applied_items":[],"pending_items":[]}"#,
        ),
        (
            ConfigAction::Apply,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"apply","error":{"code":"apply_failed","message":"failed"},"applied_items":[],"failed_item":"switch/sd"}"#,
        ),
    ];

    for (action, body) in cases {
        let response = PersistentConfigResponse::from_raw(body.to_string()).unwrap();
        assert!(response.validate(&action, Some(500)).is_err(), "{body}");
    }

    let future = r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"clear","error":{"code":"future_code","message":"future"}}"#;
    let response = PersistentConfigResponse::from_raw(future.to_string()).unwrap();
    assert!(response.validate(&ConfigAction::Clear, Some(500)).is_ok());

    for (action, body) in [
        (
            ConfigAction::Save,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"save","error":{"code":"confirmation_required","message":"confirm"},"dangerous_items":[]}"#,
        ),
        (
            ConfigAction::Clear,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"clear","error":{"code":"busy","message":"blocked"},"activity":"capture"}"#,
        ),
        (
            ConfigAction::Apply,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"apply","error":{"code":"apply_failed","message":"failed"},"applied_items":[],"failed_item":null,"pending_items":[]}"#,
        ),
    ] {
        let response = PersistentConfigResponse::from_raw(body.to_string()).unwrap();
        assert!(response.validate(&action, Some(500)).is_ok(), "{body}");
    }
}
