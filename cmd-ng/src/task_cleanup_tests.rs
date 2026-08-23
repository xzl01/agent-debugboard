use crate::task_catalog_tests::firmware_catalog_output;
use crate::task_command::{TaskCommandIo, TaskRunner};
use crate::task_test_support::{run_task, FakeReply, FakeTransport, RecordingSleeper};
use anyhow::Result;
use serde_json::{json, Value};
use std::cell::RefCell;
use std::rc::Rc;

fn confirmed_args(task_id: &str) -> Vec<String> {
    vec![
        "task".to_string(),
        "run".to_string(),
        task_id.to_string(),
        "--confirm".to_string(),
    ]
}

#[test]
fn firmware_catalog_task_run_uses_only_catalog_requests() -> Result<()> {
    // Given: a firmware catalog task and successful atomic API responses.
    let events = Rc::new(RefCell::new(Vec::new()));
    let transport = FakeTransport::new(
        std::iter::once(FakeReply::Output(firmware_catalog_output(
            "builtin/firmware-owned",
            5,
        )))
        .chain((0..5).map(|_| FakeReply::Output("{}".to_string())))
        .collect(),
        Rc::clone(&events),
    );
    let sleeper = RecordingSleeper {
        events: Rc::clone(&events),
    };

    // When: it is run through the generic task command.
    let (code, stdout, stderr) = run_task(&transport, &sleeper, "builtin/firmware-owned")?;

    // Then: no task-storage request occurs and the firmware sequence is preserved.
    assert_eq!(code, 0);
    assert!(stderr.is_empty());
    assert_eq!(
        events.borrow().as_slice(),
        [
            "send:GET /api/v1/tasks/catalog",
            "send:PUT /api/v1/power/firmware-owned-0",
            "sleep:1",
            "send:PUT /api/v1/power/firmware-owned-1",
            "sleep:2",
            "send:PUT /api/v1/power/firmware-owned-2",
            "sleep:3",
            "send:PUT /api/v1/power/firmware-owned-3",
            "sleep:4",
            "send:PUT /api/v1/power/firmware-owned-4",
        ]
    );
    let requests = transport.requests.borrow();
    assert_eq!(requests[3].body, Some(json!({"state": "off", "index": 2})));
    assert!(requests
        .iter()
        .all(|request| request.path != "/api/v1/tasks"));
    let output: Value = serde_json::from_str(&stdout)?;
    assert_eq!(output["requests_executed"], 5);
    Ok(())
}

#[test]
fn built_in_failures_at_every_record_run_exactly_one_cleanup() -> Result<()> {
    // Given: each possible ordinary built-in request is the primary failure in turn.
    for failed_index in 1..=5 {
        let events = Rc::new(RefCell::new(Vec::new()));
        let mut replies = (1..failed_index)
            .map(|_| FakeReply::Output("{}".to_string()))
            .collect::<Vec<_>>();
        replies.push(FakeReply::Failure(format!(
            "primary failure {failed_index}"
        )));
        replies.push(FakeReply::Output("{}".to_string()));
        replies.insert(
            0,
            FakeReply::Output(firmware_catalog_output("builtin/firmware-owned", 5)),
        );
        let transport = FakeTransport::new(replies, Rc::clone(&events));
        let sleeper = RecordingSleeper { events };

        // When: the confirmed built-in runs.
        let (code, stdout, _) = run_task(&transport, &sleeper, "builtin/firmware-owned")?;

        // Then: the primary index remains authoritative and cleanup is exactly one final PUT.
        assert_eq!(code, 1);
        let output: Value = serde_json::from_str(&stdout)?;
        assert_eq!(output["error"]["record_index"], failed_index);
        assert!(output["error"]["message"]
            .as_str()
            .is_some_and(|message| message.contains("primary failure")));
        assert_eq!(output["cleanup"]["attempted"], true);
        assert_eq!(output["cleanup"]["ok"], true);
        let requests = transport.requests.borrow();
        assert_eq!(requests.len(), failed_index + 2);
        assert_eq!(
            requests.last().map(|request| request.path.as_str()),
            Some("/api/v1/gpio/firmware-cleanup")
        );
        assert_eq!(
            requests.last().and_then(|request| request.body.as_ref()),
            Some(&json!({"direction": "input"}))
        );
    }
    Ok(())
}

#[test]
fn built_in_cleanup_failure_is_secondary_to_the_primary_request_failure() -> Result<()> {
    // Given: the second ordinary request and the independent cleanup both fail.
    let events = Rc::new(RefCell::new(Vec::new()));
    let transport = FakeTransport::new(
        vec![
            FakeReply::Output(firmware_catalog_output("builtin/firmware-owned", 5)),
            FakeReply::Output("{}".to_string()),
            FakeReply::Failure("rail failure".to_string()),
            FakeReply::Failure("cleanup failure".to_string()),
        ],
        Rc::clone(&events),
    );
    let sleeper = RecordingSleeper { events };

    // When: the confirmed built-in runs.
    let (code, stdout, _) = run_task(&transport, &sleeper, "builtin/firmware-owned")?;

    // Then: the rail failure stays primary and cleanup is a separate diagnostic.
    assert_eq!(code, 1);
    let output: Value = serde_json::from_str(&stdout)?;
    assert_eq!(output["error"]["record_index"], 2);
    assert!(output["error"]["message"]
        .as_str()
        .is_some_and(|message| message.contains("rail failure")));
    assert_eq!(output["cleanup"]["ok"], false);
    assert!(output["cleanup"]["error"]
        .as_str()
        .is_some_and(|message| message.contains("cleanup failure")));
    Ok(())
}

#[test]
fn built_in_human_failure_appends_cleanup_diagnostic() -> Result<()> {
    // Given: both the first ordinary request and cleanup fail.
    let events = Rc::new(RefCell::new(Vec::new()));
    let transport = FakeTransport::new(
        vec![
            FakeReply::Output(firmware_catalog_output("builtin/firmware-owned", 5)),
            FakeReply::Failure("primary failure".to_string()),
            FakeReply::Failure("cleanup failure".to_string()),
        ],
        Rc::clone(&events),
    );
    let sleeper = RecordingSleeper { events };
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();

    // When: human-readable execution reports the failure.
    let code = TaskRunner::new(&transport, &sleeper).run(
        &confirmed_args("builtin/firmware-owned"),
        TaskCommandIo::new(false, &mut stdout, &mut stderr),
    )?;

    // Then: the primary diagnostic remains first and cleanup failure is appended.
    assert_eq!(code, 1);
    let error = String::from_utf8(stderr)?;
    assert!(error.starts_with("task record 1"));
    assert!(error.contains("primary failure"));
    assert!(error.contains("cleanup failed: cleanup failure"));
    Ok(())
}
