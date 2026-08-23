use crate::client::{BoardBinaryUpload, BoardRawJsonRequest, BoardRequest, BoardTransport};
use crate::task_catalog_tests::firmware_catalog_output;
use crate::task_command::{TaskCommandIo, TaskRunner};
use crate::task_execution::TaskCancellation;
use crate::task_test_support::{
    blob, list_output, record, FakeReply, FakeTransport, RecordingSleeper,
};
use anyhow::Result;
use serde_json::{json, Value};
use std::cell::{Cell, RefCell};
use std::rc::Rc;

struct FlagCancellation {
    cancelled: Rc<Cell<bool>>,
}

impl TaskCancellation for FlagCancellation {
    fn is_cancelled(&self) -> bool {
        self.cancelled.get()
    }
}

struct CancellingTransport {
    inner: FakeTransport,
    cancelled: Rc<Cell<bool>>,
    cancel_after: usize,
}

impl BoardTransport for CancellingTransport {
    fn send_text(&self, request: BoardRequest) -> Result<String> {
        let result = self.inner.send_text(request);
        if result.is_ok() && self.inner.requests.borrow().len() == self.cancel_after {
            self.cancelled.set(true);
        }
        result
    }

    fn send_raw_json(&self, request: BoardRawJsonRequest) -> Result<String> {
        self.inner.send_raw_json(request)
    }

    fn upload_binary(&self, request: BoardBinaryUpload) -> Result<String> {
        self.inner.upload_binary(request)
    }

    fn base_url(&self) -> &str {
        self.inner.base_url()
    }
}

fn confirmed_args(task_id: &str) -> Vec<String> {
    vec![
        "task".to_string(),
        "run".to_string(),
        task_id.to_string(),
        "--confirm".to_string(),
    ]
}

#[test]
fn built_in_cancellation_boundaries_cleanup_only_partial_runs() -> Result<()> {
    // Given: cancellation arriving after each possible successful ordinary request.
    for completed in 1..=5 {
        let events = Rc::new(RefCell::new(Vec::new()));
        let cancelled = Rc::new(Cell::new(false));
        let transport = CancellingTransport {
            inner: FakeTransport::new(
                std::iter::once(FakeReply::Output(firmware_catalog_output(
                    "builtin/firmware-owned",
                    5,
                )))
                .chain((0..6).map(|_| FakeReply::Output("{}".to_string())))
                .collect(),
                Rc::clone(&events),
            ),
            cancelled: Rc::clone(&cancelled),
            cancel_after: completed + 1,
        };
        let sleeper = RecordingSleeper { events };
        let cancellation = FlagCancellation { cancelled };
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();

        // When: the confirmed run reaches that cancellation boundary.
        let code = TaskRunner::with_cancellation(&transport, &sleeper, &cancellation).run(
            &confirmed_args("builtin/firmware-owned"),
            TaskCommandIo::new(true, &mut stdout, &mut stderr),
        )?;

        // Then: partial runs clean up once, while all five successful requests complete normally.
        let output: Value = serde_json::from_slice(&stdout)?;
        if completed < 5 {
            assert_eq!(code, 1);
            assert_eq!(output["error"]["code"], "cancelled");
            assert_eq!(output["error"]["requests_completed"], completed);
            assert_eq!(output["cleanup"]["ok"], true);
            assert_eq!(transport.inner.requests.borrow().len(), completed + 2);
        } else {
            assert_eq!(code, 0);
            assert_eq!(output["requests_executed"], 5);
            assert_eq!(transport.inner.requests.borrow().len(), 6);
            assert!(output.get("cleanup").is_none());
        }
    }
    Ok(())
}

#[test]
fn cancellation_before_first_dispatch_runs_no_cleanup_or_catalog_request() -> Result<()> {
    // Given: a confirmed built-in whose cancellation token is already set.
    let events = Rc::new(RefCell::new(Vec::new()));
    let transport = FakeTransport::new(Vec::new(), Rc::clone(&events));
    let sleeper = RecordingSleeper { events };
    let cancellation = FlagCancellation {
        cancelled: Rc::new(Cell::new(true)),
    };
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();

    // When: execution begins.
    let code = TaskRunner::with_cancellation(&transport, &sleeper, &cancellation).run(
        &confirmed_args("builtin/maskrom/5v_out"),
        TaskCommandIo::new(true, &mut stdout, &mut stderr),
    )?;

    // Then: no request or cleanup occurs.
    assert_eq!(code, 1);
    assert!(transport.requests.borrow().is_empty());
    let output: Value = serde_json::from_slice(&stdout)?;
    assert_eq!(output["error"]["code"], "cancelled");
    assert_eq!(output["error"]["requests_completed"], 0);
    assert!(output.get("cleanup").is_none());
    Ok(())
}

#[test]
fn stored_task_cancellation_never_infers_cleanup() -> Result<()> {
    // Given: a stored two-request task cancelled after its first control request.
    let events = Rc::new(RefCell::new(Vec::new()));
    let cancelled = Rc::new(Cell::new(false));
    let stored_blob = blob(
        "demo",
        &[
            record("/api/v1/power/12v_out", json!({"state": "off"}), 0),
            record("/api/v1/gpio/CON_MAS", json!({"direction": "input"}), 0),
        ],
    );
    let transport = CancellingTransport {
        inner: FakeTransport::new(
            vec![
                FakeReply::Output(firmware_catalog_output("builtin/catalog-only", 1)),
                FakeReply::Output(list_output("demo", &stored_blob)),
                FakeReply::Output("{}".to_string()),
            ],
            Rc::clone(&events),
        ),
        cancelled: Rc::clone(&cancelled),
        cancel_after: 3,
    };
    let sleeper = RecordingSleeper { events };
    let cancellation = FlagCancellation { cancelled };
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();

    // When: the confirmed stored task reaches its next request boundary.
    let code = TaskRunner::with_cancellation(&transport, &sleeper, &cancellation).run(
        &confirmed_args("demo"),
        TaskCommandIo::new(true, &mut stdout, &mut stderr),
    )?;

    // Then: execution stops without an inferred CON_MAS cleanup.
    assert_eq!(code, 1);
    let output: Value = serde_json::from_slice(&stdout)?;
    assert_eq!(output["error"]["code"], "cancelled");
    assert_eq!(output["error"]["requests_completed"], 1);
    assert!(output.get("cleanup").is_none());
    assert_eq!(transport.inner.requests.borrow().len(), 3);
    Ok(())
}
