use crate::task_catalog_tests::firmware_catalog_output;
use crate::task_command::{TaskCommandIo, TaskRunner};
use crate::task_test_support::{
    blob, list_output, record, run_task, FakeReply, FakeTransport, RecordingSleeper,
};
use anyhow::Result;
use reqwest::Method;
use serde_json::{json, Value};
use std::cell::RefCell;
use std::rc::Rc;

#[test]
fn built_in_id_does_not_fall_back_to_shadowed_stored_task_when_catalog_fails() -> Result<()> {
    let events = Rc::new(RefCell::new(Vec::new()));
    let task_id = "builtin/firmware-owned";
    let stored_blob = blob(
        task_id,
        &[record("/api/v1/power/stored", json!({"state": "on"}), 0)],
    );
    let transport = FakeTransport::new(
        vec![
            FakeReply::Failure("catalog unavailable".to_string()),
            FakeReply::Output(list_output(task_id, &stored_blob)),
            FakeReply::Output("{}".to_string()),
        ],
        Rc::clone(&events),
    );
    let sleeper = RecordingSleeper { events };

    let (code, stdout, stderr) = run_task(&transport, &sleeper, task_id)?;

    assert_eq!(code, 1);
    assert!(stderr.is_empty());
    let output: Value = serde_json::from_str(&stdout)?;
    assert_eq!(output["error"]["code"], "catalog_unavailable");
    assert_eq!(transport.requests.borrow().len(), 1);
    assert_eq!(transport.requests.borrow()[0].path, "/api/v1/tasks/catalog");
    Ok(())
}

#[test]
fn built_in_id_missing_from_available_catalog_does_not_run_stored_task() -> Result<()> {
    // Given: a stored rogue built-in ID and a successful catalog without that ID.
    let events = Rc::new(RefCell::new(Vec::new()));
    let task_id = "builtin/rogue";
    let stored_blob = blob(
        task_id,
        &[record("/api/v1/power/stored", json!({"state": "on"}), 0)],
    );
    let transport = FakeTransport::new(
        vec![
            FakeReply::Output(firmware_catalog_output("builtin/firmware-owned", 1)),
            FakeReply::Output(list_output(task_id, &stored_blob)),
            FakeReply::Output("{}".to_string()),
        ],
        Rc::clone(&events),
    );
    let sleeper = RecordingSleeper { events };

    // When: the missing reserved ID is run.
    let (code, stdout, stderr) = run_task(&transport, &sleeper, task_id)?;

    // Then: it fails closed before a stored fetch or control request.
    assert_eq!(code, 1);
    assert!(stderr.is_empty());
    let output: Value = serde_json::from_str(&stdout)?;
    assert_eq!(output["error"]["code"], "catalog_unavailable");
    let requests = transport.requests.borrow();
    assert_eq!(requests.len(), 1);
    assert_eq!(requests[0].path, "/api/v1/tasks/catalog");
    assert_eq!(
        requests
            .iter()
            .filter(|request| request.method == Method::PUT)
            .count(),
        0
    );
    Ok(())
}

#[test]
fn task_list_hides_rogue_reserved_stored_task_when_catalog_is_available() -> Result<()> {
    // Given: storage holds a reserved ID that the available catalog does not own.
    let events = Rc::new(RefCell::new(Vec::new()));
    let transport = FakeTransport::new(
        vec![
            FakeReply::Output(list_output("builtin/rogue", "")),
            FakeReply::Output(firmware_catalog_output("builtin/firmware-owned", 1)),
        ],
        Rc::clone(&events),
    );
    let sleeper = RecordingSleeper { events };
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();

    // When: the catalog and stored tasks are listed.
    let code = TaskRunner::new(&transport, &sleeper).run(
        &["task".to_string(), "list".to_string()],
        TaskCommandIo::new(true, &mut stdout, &mut stderr),
    )?;

    // Then: only the real built-in is exposed.
    assert_eq!(code, 0);
    assert!(stderr.is_empty());
    let output: Value = serde_json::from_slice(&stdout)?;
    assert_eq!(output["task_count"], 1);
    assert_eq!(output["tasks"][0]["id"], "builtin/firmware-owned");
    assert!(output["tasks"]
        .as_array()
        .is_some_and(|tasks| tasks.iter().all(|task| task["id"] != "builtin/rogue")));
    Ok(())
}

#[test]
fn task_list_hides_rogue_reserved_stored_task_when_catalog_is_unavailable() -> Result<()> {
    // Given: storage holds a reserved ID and the catalog cannot be retrieved.
    let events = Rc::new(RefCell::new(Vec::new()));
    let transport = FakeTransport::new(
        vec![
            FakeReply::Output(list_output("builtin/rogue", "")),
            FakeReply::Failure("catalog unavailable".to_string()),
        ],
        Rc::clone(&events),
    );
    let sleeper = RecordingSleeper { events };
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();

    // When: the catalog and stored tasks are listed.
    let code = TaskRunner::new(&transport, &sleeper).run(
        &["task".to_string(), "list".to_string()],
        TaskCommandIo::new(true, &mut stdout, &mut stderr),
    )?;

    // Then: the reserved stored ID remains hidden despite catalog failure.
    assert_eq!(code, 0);
    assert!(stderr.is_empty());
    let output: Value = serde_json::from_slice(&stdout)?;
    assert_eq!(output["task_count"], 0);
    assert_eq!(output["catalog_available"], false);
    assert!(output["tasks"]
        .as_array()
        .is_some_and(|tasks| tasks.iter().all(|task| task["id"] != "builtin/rogue")));
    Ok(())
}

#[test]
fn catalog_record_precedes_a_same_id_stored_task() -> Result<()> {
    let events = Rc::new(RefCell::new(Vec::new()));
    let task_id = "builtin/firmware-owned";
    let stored_blob = blob(
        task_id,
        &[record("/api/v1/power/stored", json!({"state": "on"}), 0)],
    );
    let transport = FakeTransport::new(
        vec![
            FakeReply::Output(firmware_catalog_output(task_id, 1)),
            FakeReply::Output("{}".to_string()),
            FakeReply::Output(list_output(task_id, &stored_blob)),
        ],
        Rc::clone(&events),
    );
    let sleeper = RecordingSleeper { events };

    let (code, _, stderr) = run_task(&transport, &sleeper, task_id)?;

    assert_eq!(code, 0);
    assert!(stderr.is_empty());
    assert_eq!(transport.requests.borrow().len(), 2);
    assert_eq!(
        transport.requests.borrow()[1].path,
        "/api/v1/power/firmware-owned-0"
    );
    Ok(())
}
