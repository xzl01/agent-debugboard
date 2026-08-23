use crate::task_catalog_tests::firmware_catalog_output;
use crate::task_test_support::{
    blob, list_output, record, run_task, FakeReply, FakeTransport, RecordingSleeper,
};
use anyhow::Result;
use serde_json::{json, Value};
use std::cell::RefCell;
use std::rc::Rc;

#[test]
fn task_run_retrieves_and_executes_records_in_order_with_client_waits() -> Result<()> {
    let events = Rc::new(RefCell::new(Vec::new()));
    let saved_blob = blob(
        "demo",
        &[
            record("/api/v1/power/12v_out", json!({"state": "off"}), 25),
            record("/api/v1/gpio/CON_MAS", json!({"direction": "input"}), 10),
        ],
    );
    let transport = FakeTransport::new(
        vec![
            FakeReply::Output(firmware_catalog_output("builtin/catalog-only", 1)),
            FakeReply::Output(list_output("demo", &saved_blob)),
            FakeReply::Output("{}".to_string()),
            FakeReply::Output("{}".to_string()),
        ],
        Rc::clone(&events),
    );
    let sleeper = RecordingSleeper {
        events: Rc::clone(&events),
    };

    let (code, stdout, stderr) = run_task(&transport, &sleeper, "demo")?;

    assert_eq!(code, 0);
    assert!(stderr.is_empty());
    assert_eq!(
        events.borrow().as_slice(),
        [
            "send:GET /api/v1/tasks/catalog",
            "send:GET /api/v1/tasks",
            "send:PUT /api/v1/power/12v_out",
            "sleep:25",
            "send:PUT /api/v1/gpio/CON_MAS",
            "sleep:10",
        ]
    );
    let requests = transport.requests.borrow();
    assert_eq!(requests[2].body, Some(json!({"state": "off"})));
    assert_eq!(requests[3].body, Some(json!({"direction": "input"})));
    let output: Value = serde_json::from_str(&stdout)?;
    assert_eq!(output["command"], "task");
    assert_eq!(output["action"], "run");
    assert_eq!(output["requests_executed"], 2);
    Ok(())
}

#[test]
fn task_run_stops_at_first_failed_record_without_sleeping_after_it() -> Result<()> {
    let events = Rc::new(RefCell::new(Vec::new()));
    let saved_blob = blob(
        "demo",
        &[
            record("/api/v1/power/12v_out", json!({"state": "off"}), 5),
            record("/api/v1/power/12v_out", json!({"state": "on"}), 20),
            record("/api/v1/gpio/CON_MAS", json!({"direction": "input"}), 30),
        ],
    );
    let transport = FakeTransport::new(
        vec![
            FakeReply::Output(firmware_catalog_output("builtin/catalog-only", 1)),
            FakeReply::Output(list_output("demo", &saved_blob)),
            FakeReply::Output("{}".to_string()),
            FakeReply::Failure("HTTP 409 Conflict: busy".to_string()),
        ],
        Rc::clone(&events),
    );
    let sleeper = RecordingSleeper {
        events: Rc::clone(&events),
    };

    let (code, stdout, _) = run_task(&transport, &sleeper, "demo")?;

    assert_eq!(code, 1);
    assert_eq!(transport.requests.borrow().len(), 4);
    assert_eq!(
        events.borrow().as_slice(),
        [
            "send:GET /api/v1/tasks/catalog",
            "send:GET /api/v1/tasks",
            "send:PUT /api/v1/power/12v_out",
            "sleep:5",
            "send:PUT /api/v1/power/12v_out",
        ]
    );
    let output: Value = serde_json::from_str(&stdout)?;
    assert_eq!(output["action"], "run");
    assert_eq!(output["error"]["code"], "request_failed");
    assert_eq!(output["error"]["record_index"], 2);
    assert_eq!(output["error"]["path"], "/api/v1/power/12v_out");
    assert!(output.get("cleanup").is_none());
    Ok(())
}

#[test]
fn task_run_rejects_stale_blob_marker_before_dispatch() -> Result<()> {
    let events = Rc::new(RefCell::new(Vec::new()));
    let transport = FakeTransport::new(
        vec![
            FakeReply::Output(firmware_catalog_output("builtin/catalog-only", 1)),
            FakeReply::Output(list_output("demo", "# linkr-orch.v1\n# task demo\n")),
        ],
        Rc::clone(&events),
    );
    let sleeper = RecordingSleeper { events };

    let (code, stdout, _) = run_task(&transport, &sleeper, "demo")?;

    assert_eq!(code, 1);
    assert_eq!(transport.requests.borrow().len(), 2);
    let output: Value = serde_json::from_str(&stdout)?;
    assert_eq!(output["error"]["code"], "invalid_blob");
    Ok(())
}

#[test]
fn task_run_rejects_duplicate_stored_ids_before_dispatch() -> Result<()> {
    let events = Rc::new(RefCell::new(Vec::new()));
    let saved_blob = format!(
        "# linkr-task.v1\n# task demo\n{}\n# task demo\n{}\n",
        record("/api/v1/power/12v_out", json!({"state": "off"}), 0),
        record("/api/v1/power/12v_out", json!({"state": "on"}), 0),
    );
    let transport = FakeTransport::new(
        vec![
            FakeReply::Output(firmware_catalog_output("builtin/catalog-only", 1)),
            FakeReply::Output(list_output("demo", &saved_blob)),
        ],
        Rc::clone(&events),
    );
    let sleeper = RecordingSleeper { events };

    let (code, stdout, _) = run_task(&transport, &sleeper, "demo")?;

    assert_eq!(code, 1);
    assert_eq!(transport.requests.borrow().len(), 2);
    let output: Value = serde_json::from_str(&stdout)?;
    assert_eq!(output["error"]["code"], "invalid_blob");
    Ok(())
}

#[test]
fn task_run_reports_empty_store_for_valid_empty_state() -> Result<()> {
    let events = Rc::new(RefCell::new(Vec::new()));
    let transport = FakeTransport::new(
        vec![
            FakeReply::Output(firmware_catalog_output("builtin/catalog-only", 1)),
            FakeReply::Output(list_output("", "")),
        ],
        Rc::clone(&events),
    );
    let sleeper = RecordingSleeper { events };

    let (code, stdout, _) = run_task(&transport, &sleeper, "missing")?;

    assert_eq!(code, 1);
    let output: Value = serde_json::from_str(&stdout)?;
    assert_eq!(output["error"]["code"], "task_store_empty");
    Ok(())
}

#[test]
fn task_run_reports_unknown_id_for_nonempty_store() -> Result<()> {
    let events = Rc::new(RefCell::new(Vec::new()));
    let saved_blob = blob(
        "existing",
        &[record("/api/v1/power/12v_out", json!({"state": "off"}), 0)],
    );
    let transport = FakeTransport::new(
        vec![
            FakeReply::Output(firmware_catalog_output("builtin/catalog-only", 1)),
            FakeReply::Output(list_output("existing", &saved_blob)),
        ],
        Rc::clone(&events),
    );
    let sleeper = RecordingSleeper { events };

    let (code, stdout, _) = run_task(&transport, &sleeper, "missing")?;

    assert_eq!(code, 1);
    let output: Value = serde_json::from_str(&stdout)?;
    assert_eq!(output["error"]["code"], "task_not_found");
    Ok(())
}

#[test]
fn task_run_reports_retrieval_error_before_blob_parsing() -> Result<()> {
    let events = Rc::new(RefCell::new(Vec::new()));
    let transport = FakeTransport::new(
        vec![
            FakeReply::Output(firmware_catalog_output("builtin/catalog-only", 1)),
            FakeReply::Failure("connection refused".to_string()),
        ],
        Rc::clone(&events),
    );
    let sleeper = RecordingSleeper { events };

    let (code, stdout, _) = run_task(&transport, &sleeper, "demo")?;

    assert_eq!(code, 1);
    let output: Value = serde_json::from_str(&stdout)?;
    assert_eq!(output["error"]["code"], "retrieval_failed");
    Ok(())
}
