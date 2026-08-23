use crate::task_catalog_tests::firmware_catalog_output;
use crate::task_command::{TaskCommandIo, TaskRunner};
use crate::task_test_support::{
    blob, list_output, record, FakeReply, FakeTransport, RecordingSleeper,
};
use anyhow::Result;
use reqwest::Method;
use serde_json::{json, Value};
use std::cell::RefCell;
use std::rc::Rc;

#[test]
fn task_list_merges_built_ins_into_the_client_catalog() -> Result<()> {
    // Given: a reachable board with no stored tasks.
    let events = Rc::new(RefCell::new(Vec::new()));
    let transport = FakeTransport::new(
        vec![
            FakeReply::Output(list_output("", "")),
            FakeReply::Output(firmware_catalog_output("builtin/firmware-owned", 1)),
        ],
        Rc::clone(&events),
    );
    let sleeper = RecordingSleeper { events };
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();

    // When: the generic task catalog is listed as JSON.
    let code = TaskRunner::new(&transport, &sleeper).run(
        &["task".to_string(), "list".to_string()],
        TaskCommandIo::new(true, &mut stdout, &mut stderr),
    )?;

    // Then: the firmware-owned built-in is visible without being stored.
    assert_eq!(code, 0);
    assert!(stderr.is_empty());
    let output: Value = serde_json::from_slice(&stdout)?;
    assert_eq!(output["task_count"], 1);
    assert_eq!(output["stored_task_count"], 0);
    assert_eq!(output["tasks"][0]["id"], "builtin/firmware-owned");
    assert_eq!(output["tasks"][0]["source"], "builtin");
    assert_eq!(output["catalog_available"], true);
    assert_eq!(transport.requests.borrow().len(), 2);
    Ok(())
}

#[test]
fn task_list_marks_a_stored_collision_as_shadowed() -> Result<()> {
    // Given: firmware storage containing a task with a reserved built-in ID.
    let events = Rc::new(RefCell::new(Vec::new()));
    let task_id = "builtin/maskrom/5v_out";
    let stored_blob = blob(
        task_id,
        &[record("/api/v1/power/20v_out", json!({"state": "off"}), 0)],
    );
    let transport = FakeTransport::new(
        vec![
            FakeReply::Output(list_output(task_id, &stored_blob)),
            FakeReply::Output(firmware_catalog_output(task_id, 1)),
        ],
        Rc::clone(&events),
    );
    let sleeper = RecordingSleeper { events };
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();

    // When: the generic task catalog is listed.
    let code = TaskRunner::new(&transport, &sleeper).run(
        &["task".to_string(), "list".to_string()],
        TaskCommandIo::new(true, &mut stdout, &mut stderr),
    )?;

    // Then: the immutable built-in wins and reports the hidden stored collision.
    assert_eq!(code, 0);
    let output: Value = serde_json::from_slice(&stdout)?;
    assert_eq!(output["task_count"], 1);
    assert_eq!(output["stored_task_count"], 1);
    assert_eq!(output["tasks"][0]["id"], task_id);
    assert_eq!(output["tasks"][0]["source"], "builtin");
    assert_eq!(output["tasks"][0]["shadowed_stored"], true);
    assert_eq!(
        output["tasks"]
            .as_array()
            .map(|tasks| tasks.iter().filter(|task| task["id"] == task_id).count()),
        Some(1)
    );
    Ok(())
}

#[test]
fn task_list_keeps_stored_tasks_when_catalog_retrieval_fails() -> Result<()> {
    // Given: a readable stored task list and an unavailable firmware catalog.
    let events = Rc::new(RefCell::new(Vec::new()));
    let stored_blob = blob(
        "demo",
        &[record("/api/v1/power/20v_out", json!({"state": "off"}), 0)],
    );
    let transport = FakeTransport::new(
        vec![
            FakeReply::Output(list_output("demo", &stored_blob)),
            FakeReply::Failure("catalog unavailable".to_string()),
        ],
        Rc::clone(&events),
    );
    let sleeper = RecordingSleeper { events };
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();

    // When: task list fetches both independent sources.
    let code = TaskRunner::new(&transport, &sleeper).run(
        &["task".to_string(), "list".to_string()],
        TaskCommandIo::new(true, &mut stdout, &mut stderr),
    )?;

    // Then: stored tasks remain listed with a structured catalog diagnostic.
    assert_eq!(code, 0);
    let output: Value = serde_json::from_slice(&stdout)?;
    assert_eq!(output["tasks"][0]["id"], "demo");
    assert_eq!(output["tasks"][0]["source"], "stored");
    assert_eq!(output["catalog_available"], false);
    assert_eq!(output["catalog_error"]["code"], "catalog_unavailable");
    assert_eq!(transport.requests.borrow().len(), 2);
    Ok(())
}

#[test]
fn task_store_recovery_is_not_a_generic_task_command() -> Result<()> {
    // Given: the removed recovery-specific storage syntax.
    let events = Rc::new(RefCell::new(Vec::new()));
    let transport = FakeTransport::new(Vec::new(), Rc::clone(&events));
    let sleeper = RecordingSleeper { events };
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();
    let arguments = vec![
        "task".to_string(),
        "store-recovery".to_string(),
        "rockchip-maskrom".to_string(),
        "12v_out".to_string(),
    ];

    // When: it is submitted to the generic task parser.
    let code = TaskRunner::new(&transport, &sleeper).run(
        &arguments,
        TaskCommandIo::new(true, &mut stdout, &mut stderr),
    )?;

    // Then: usage fails before any firmware request is made.
    assert_eq!(code, 2);
    assert!(transport.requests.borrow().is_empty());
    let output: Value = serde_json::from_slice(&stdout)?;
    assert_eq!(output["error"]["code"], "usage");
    Ok(())
}

#[test]
fn task_clear_uses_delete_on_the_task_endpoint() -> Result<()> {
    let events = Rc::new(RefCell::new(Vec::new()));
    let transport = FakeTransport::new(
        vec![FakeReply::Output(
            json!({
                "schema": "radxa-linkr-debugger.v1",
                "ok": true,
                "command": "task",
                "action": "clear",
            })
            .to_string(),
        )],
        Rc::clone(&events),
    );
    let sleeper = RecordingSleeper { events };
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();

    let code = TaskRunner::new(&transport, &sleeper).run(
        &["task".to_string(), "clear".to_string()],
        TaskCommandIo::new(false, &mut stdout, &mut stderr),
    )?;

    assert_eq!(code, 0);
    let requests = transport.requests.borrow();
    assert_eq!(requests.len(), 1);
    assert_eq!(requests[0].method, Method::DELETE);
    assert_eq!(requests[0].path, "/api/v1/tasks");
    Ok(())
}

#[test]
fn task_run_human_error_names_failed_record_and_path() -> Result<()> {
    let events = Rc::new(RefCell::new(Vec::new()));
    let saved_blob = blob(
        "demo",
        &[record("/api/v1/power/12v_out", json!({"state": "off"}), 5)],
    );
    let transport = FakeTransport::new(
        vec![
            FakeReply::Output(firmware_catalog_output("builtin/catalog-only", 1)),
            FakeReply::Output(list_output("demo", &saved_blob)),
            FakeReply::Failure("HTTP 409 Conflict: busy".to_string()),
        ],
        Rc::clone(&events),
    );
    let sleeper = RecordingSleeper { events };
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();

    let code = TaskRunner::new(&transport, &sleeper).run(
        &[
            "task".to_string(),
            "run".to_string(),
            "demo".to_string(),
            "--confirm".to_string(),
        ],
        TaskCommandIo::new(false, &mut stdout, &mut stderr),
    )?;

    assert_eq!(code, 1);
    let error = String::from_utf8(stderr)?;
    assert!(error.contains("record 1"));
    assert!(error.contains("/api/v1/power/12v_out"));
    Ok(())
}

#[test]
fn task_run_requires_explicit_confirmation_before_any_transport_request() -> Result<()> {
    // Given: an otherwise valid Task run without the hardware confirmation flag.
    let events = Rc::new(RefCell::new(Vec::new()));
    let transport = FakeTransport::new(Vec::new(), Rc::clone(&events));
    let sleeper = RecordingSleeper { events };
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();

    // When: the unconfirmed run is submitted.
    let code = TaskRunner::new(&transport, &sleeper).run(
        &["task".to_string(), "run".to_string(), "demo".to_string()],
        TaskCommandIo::new(true, &mut stdout, &mut stderr),
    )?;

    // Then: confirmation fails closed before even a read-only catalog request.
    assert_eq!(code, 2);
    assert!(transport.requests.borrow().is_empty());
    let output: Value = serde_json::from_slice(&stdout)?;
    assert_eq!(output["error"]["code"], "confirmation_required");
    Ok(())
}
