use crate::task_blob::{build_task_blob, TaskBlob, TaskError, TaskList, TASK_MARKER_VERSION};
use anyhow::{anyhow, Result};
use serde_json::json;

fn request(method: &str, path: &str, wait_ms: u64) -> String {
    request_with_body(method, path, &json!({"state": "off"}).to_string(), wait_ms)
}

fn request_with_body(method: &str, path: &str, body: &str, wait_ms: u64) -> String {
    json!({
        "method": method,
        "path": path,
        "body": body,
        "wait_ms": wait_ms,
    })
    .to_string()
}

fn task_blob_with_record(record: &str) -> String {
    format!("{TASK_MARKER_VERSION}\n# task demo\n{record}\n")
}

#[test]
fn task_blob_builder_wraps_raw_requests_with_the_frozen_marker() -> Result<()> {
    let blob = build_task_blob(&request("PUT", "/api/v1/power/12v_out", 10), "power-cycle")?;

    assert!(blob.starts_with("# linkr-task.v1\n# task power-cycle\n"));
    assert!(blob.ends_with('\n'));
    Ok(())
}

#[test]
fn task_blob_builder_preserves_an_existing_valid_blob() -> Result<()> {
    let blob = format!(
        "{TASK_MARKER_VERSION}\n# task demo\n{}\n",
        request("PUT", "/api/v1/gpio/CON_MAS", 0)
    );

    assert_eq!(build_task_blob(&blob, "ignored")?, blob);
    Ok(())
}

#[test]
fn task_blob_parser_decodes_body_and_wait_into_typed_record() -> Result<()> {
    let blob = format!(
        "{TASK_MARKER_VERSION}\n# task demo\n{}\n",
        request("PUT", "/api/v1/power/12v_out", 42)
    );

    let parsed = TaskBlob::parse(&blob)?;
    let task = parsed.task("demo")?;

    assert_eq!(task.records.len(), 1);
    assert_eq!(task.records[0].body, json!({"state": "off"}));
    assert_eq!(task.records[0].wait.as_millis(), 42);
    Ok(())
}

#[test]
fn task_blob_parser_rejects_duplicate_ids_when_actions_differ() {
    let blob = format!(
        "{TASK_MARKER_VERSION}\n# task demo\n{}\n# task demo\n{}\n",
        request("PUT", "/api/v1/power/12v_out", 0),
        request_with_body(
            "PUT",
            "/api/v1/power/12v_out",
            &json!({"state": "on"}).to_string(),
            0,
        )
    );

    assert!(matches!(
        TaskBlob::parse(&blob),
        Err(TaskError::InvalidBlob { .. })
    ));
}

#[test]
fn task_blob_parser_rejects_disallowed_method_and_path() -> Result<()> {
    let invalid_method = format!(
        "{TASK_MARKER_VERSION}\n# task demo\n{}\n",
        request("POST", "/api/v1/power/12v_out", 0)
    );
    let invalid_path = format!(
        "{TASK_MARKER_VERSION}\n# task demo\n{}\n",
        request("PUT", "/api/v1/serial", 0)
    );

    assert!(matches!(
        TaskBlob::parse(&invalid_method),
        Err(TaskError::InvalidBlob { .. })
    ));
    assert!(matches!(
        TaskBlob::parse(&invalid_path),
        Err(TaskError::InvalidBlob { .. })
    ));
    Ok(())
}

#[test]
fn task_blob_parser_rejects_path_traversal_and_normalization_forms() {
    let invalid_paths = [
        "/api/v1/power/../config",
        "/api/v1/power/%2e%2e",
        "/api/v1/power/12v_out/state",
        "/api/v1/power/12v_out?state=on",
        "/api/v1/power/12v_out#fragment",
        "/api/v1/power/12v\\out",
        "/api/v1/power/",
        "/api/v1/power/12v%2fout",
        "/api/v1/power/12v%25out",
        "/api/v1/power/12v out",
        "/api/v1/power/12v\u{001f}out",
        "/api/v1/power/12v_输出",
    ];

    for path in invalid_paths {
        let blob = task_blob_with_record(&request("PUT", path, 0));
        assert!(TaskBlob::parse(&blob).is_err(), "accepted path {path:?}");
    }
}

#[test]
fn task_blob_parser_rejects_unknown_and_duplicate_envelope_fields() {
    let unknown = r#"{"method":"PUT","path":"/api/v1/power/12v_out","body":"{}","extra":true}"#;
    let duplicate = r#"{"method":"PUT","method":"PUT","path":"/api/v1/power/12v_out","body":"{}"}"#;

    assert!(TaskBlob::parse(&task_blob_with_record(unknown)).is_err());
    assert!(TaskBlob::parse(&task_blob_with_record(duplicate)).is_err());
}

#[test]
fn task_blob_parser_enforces_firmware_body_json_depth_boundary() -> Result<()> {
    let nested_arrays_16 = format!("{}0{}", "[".repeat(16), "]".repeat(16));
    let nested_objects_16 = format!("{}0{}", "{\"v\":".repeat(16), "}".repeat(16));

    for body in [&nested_arrays_16, &nested_objects_16] {
        let valid =
            task_blob_with_record(&request_with_body("PUT", "/api/v1/power/12v_out", body, 0));
        let invalid_body = format!("[{body}]");
        let invalid = task_blob_with_record(&request_with_body(
            "PUT",
            "/api/v1/power/12v_out",
            &invalid_body,
            0,
        ));

        TaskBlob::parse(&valid)?;
        assert!(TaskBlob::parse(&invalid).is_err());
    }
    Ok(())
}

#[test]
fn task_blob_parser_rejects_wait_and_blob_size_above_firmware_limits() -> Result<()> {
    let invalid_wait = format!(
        "{TASK_MARKER_VERSION}\n# task demo\n{}\n",
        request("PUT", "/api/v1/power/12v_out", 60_001)
    );
    let oversized = format!("{TASK_MARKER_VERSION}\n# task demo\n# {}", "x".repeat(4096));

    assert!(TaskBlob::parse(&invalid_wait).is_err());
    assert!(TaskBlob::parse(&oversized).is_err());
    Ok(())
}

#[test]
fn task_blob_parser_rejects_old_or_malformed_marker() {
    assert!(TaskBlob::parse("# linkr-orch.v1\n# task demo\n").is_err());
    assert!(TaskBlob::parse("# linkr-task.v2\n# task demo\n").is_err());
}

#[test]
fn task_blob_parser_rejects_comment_before_version_and_repeated_version_marker() {
    let request = request("PUT", "/api/v1/power/12v_out", 0);
    let comment_before = format!("# padding\n{TASK_MARKER_VERSION}\n# task demo\n{request}\n");
    let repeated =
        format!("{TASK_MARKER_VERSION}\n{TASK_MARKER_VERSION}\n# task demo\n{request}\n");

    assert!(TaskBlob::parse(&comment_before).is_err());
    assert!(TaskBlob::parse(&repeated).is_err());
}

#[test]
fn task_blob_parser_preserves_empty_lines_and_comments_after_exact_version() -> Result<()> {
    let blob = format!(
        "\n\r\n{TASK_MARKER_VERSION}\n# padding\n# task demo\n{}\n",
        request("PUT", "/api/v1/power/12v_out", 0)
    );

    TaskBlob::parse(&blob)?;
    Ok(())
}

#[test]
fn task_list_accepts_empty_blob_but_requires_frozen_envelope() -> Result<()> {
    let valid = json!({
        "schema": "radxa-linkr-debugger.v1",
        "ok": true,
        "command": "task",
        "action": "list",
        "tasks": [],
        "blob": "",
    })
    .to_string();
    let wrong_action = valid.replace("\"list\"", "\"store\"");

    let list = TaskList::from_raw(valid)?;
    assert!(list.tasks.is_empty());
    assert!(TaskBlob::parse(&list.blob).is_ok());
    let error = TaskList::from_raw(wrong_action)
        .err()
        .ok_or_else(|| anyhow!("wrong task action unexpectedly accepted"))?;
    assert_eq!(error.code(), "invalid_response");
    Ok(())
}

#[test]
fn task_blob_schema_constant_uses_task_terminology() {
    assert_eq!(TASK_MARKER_VERSION, "# linkr-task.v1");
}
