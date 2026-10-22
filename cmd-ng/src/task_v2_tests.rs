use crate::task_api::{
    base64_encode, encode_task_mutation, sha256_hex, TaskApi, TaskMutation,
    TASK_MUTATION_CONTENT_TYPE, TASK_MUTATION_MAX_BYTES, TASK_SOURCE_CONTENT_TYPE,
};
use crate::task_blob::task_ir_section;
use crate::task_command::{TaskCommandIo, TaskRunner};
use crate::task_envelope::TaskList;
use crate::task_test_support::{
    blob, list_output, list_reply, record, FakeReply, FakeTransport, RecordingSleeper,
};
use anyhow::Result;
use reqwest::StatusCode;
use serde_json::{json, Value};
use std::cell::RefCell;
use std::fs;
use std::path::PathBuf;
use std::rc::Rc;

const STORE_ID: &str = "00010203-0405-0607-0809-0a0b0c0d0e0f";
const TEST_STORE_ID: &str = "12345678-1234-4567-890a-123456789abc";
const CATALOG: &str = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
const A_SOURCE_DIGEST: &str = "bfd3006056fd18bce3a2b1db4818f96079030b0976d846b040efe2a72ee9e1f1";
const A_IR_DIGEST: &str = "5c227cbb48ebc9e91d4a4c2fa60f9d5f4490f51e361110763ebc22cf641d4ef0";
const B_IR_DIGEST: &str = "f7633e2af94cbd64a4fc28378a7790b4051f1576604ab287dc621dc13b5363ae";
const FULL_IR_DIGEST: &str = "b2298f2e1f8b31e99a63a7916460941eea303dc34dc032b1b66a7e63021c5d3f";
const GOLDEN_SOURCE: &str = concat!(
    "{\"schema\":\"linkr-test.v1\",\"name\":\"A\",\"version\":\"1.0\",",
    "\"created\":\"2026-10-22T00:00:00.000Z\"}\n",
    "{\"id\":\"s1\",\"params\":{\"rail\":\"5v_out\"},\"type\":\"power_on\"}\n"
);
const GOLDEN_IR: &str = concat!(
    "# linkr-task.v1\n# task A\n",
    "{\"method\":\"PUT\",\"path\":\"/api/v1/power/5v_out\",\"body\":\"{\\\"state\\\":\\\"on\\\"}\"}\n",
    "# task B\n",
    "{\"method\":\"PUT\",\"path\":\"/api/v1/power/5v_out\",\"body\":\"{\\\"state\\\":\\\"on\\\"}\"}\n"
);

/// Strict canonical base64 decode mirroring the firmware mutation parser:
/// standard alphabet, exact zero-pad-bit padding, no whitespace.
fn base64_decode(text: &str) -> Result<Vec<u8>, anyhow::Error> {
    fn value(byte: u8) -> Option<u8> {
        match byte {
            b'A'..=b'Z' => Some(byte - b'A'),
            b'a'..=b'z' => Some(byte - b'a' + 26),
            b'0'..=b'9' => Some(byte - b'0' + 52),
            b'+' => Some(62),
            b'/' => Some(63),
            _ => None,
        }
    }
    let bytes = text.as_bytes();
    anyhow::ensure!(
        bytes.len().is_multiple_of(4),
        "base64 length is not a multiple of 4"
    );
    let pad = bytes
        .iter()
        .rev()
        .take(2)
        .take_while(|&&byte| byte == b'=')
        .count();
    anyhow::ensure!(
        bytes[..bytes.len() - pad]
            .iter()
            .all(|&byte| value(byte).is_some()),
        "base64 carries a non-alphabet byte"
    );
    if pad == 2 {
        anyhow::ensure!(
            value(bytes[bytes.len() - 3]).unwrap() & 0x0f == 0,
            "base64 pad bits are not zero"
        );
    }
    if pad == 1 {
        anyhow::ensure!(
            value(bytes[bytes.len() - 2]).unwrap() & 0x03 == 0,
            "base64 pad bits are not zero"
        );
    }
    let mut out = Vec::new();
    for (index, group) in bytes.chunks(4).enumerate() {
        let last = (index + 1) * 4 == bytes.len();
        let a = value(group[0]).unwrap();
        let b = value(group[1]).unwrap();
        let c = if last && pad == 2 {
            0
        } else {
            value(group[2]).unwrap()
        };
        let d = if last && pad >= 1 {
            0
        } else {
            value(group[3]).unwrap()
        };
        out.push((a << 2) | (b >> 4));
        if !last || pad < 2 {
            out.push((b << 4) | (c >> 2));
        }
        if !last || pad < 1 {
            out.push((c << 6) | d);
        }
    }
    Ok(out)
}

fn golden_metadata() -> String {
    format!(
        concat!(
            "{{\"schema\":\"radxa-linkr-debugger.v1\",\"ok\":true,",
            "\"command\":\"tasks\",\"action\":\"list\",",
            "\"task_store_id\":\"{}\",\"revision\":\"9007199254740993\",",
            "\"catalog_fingerprint\":\"{}\",\"full_ir_bytes\":184,",
            "\"full_ir_digest\":\"{}\",\"tasks\":[",
            "{{\"slot\":0,\"id\":\"A\",\"request_count\":1,",
            "\"source_state\":\"present\",\"source_bytes\":148,",
            "\"source_digest\":\"{}\",\"ir_bytes\":100,",
            "\"task_ir_digest\":\"{}\"}},",
            "{{\"slot\":3,\"id\":\"B\",\"request_count\":1,",
            "\"source_state\":\"absent\",\"source_bytes\":0,",
            "\"source_digest\":null,\"ir_bytes\":100,",
            "\"task_ir_digest\":\"{}\"}}]}}\n"
        ),
        STORE_ID, CATALOG, FULL_IR_DIGEST, A_SOURCE_DIGEST, A_IR_DIGEST, B_IR_DIGEST
    )
}

fn mutation_success(action: &str, task: Value) -> String {
    json!({
        "schema": "radxa-linkr-debugger.v1",
        "ok": true,
        "command": "tasks",
        "action": action,
        "task_store_id": TEST_STORE_ID,
        "revision": "9007199254740994",
        "catalog_fingerprint": CATALOG,
        "task": task,
    })
    .to_string()
}

fn absent_summary(id: &str, slot: u8, ir: &str) -> Value {
    let section = task_ir_section(ir, id).expect("valid task fixture");
    json!({
        "slot": slot,
        "id": id,
        "request_count": 1,
        "source_state": "absent",
        "source_bytes": 0,
        "source_digest": null,
        "ir_bytes": section.len(),
        "task_ir_digest": sha256_hex(section.as_bytes()),
    })
}

struct TestFile(PathBuf);

impl TestFile {
    fn create(contents: &str) -> Result<Self> {
        let path = std::env::temp_dir().join(format!("linkr-task-v2-{}", uuid::Uuid::new_v4()));
        fs::write(&path, contents)?;
        Ok(Self(path))
    }
}

impl Drop for TestFile {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.0);
    }
}

#[test]
fn firmware_golden_json_raw_digests_and_json_mutation_contract_match() -> Result<()> {
    let firmware_source = include_str!(
        "../../apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_http_net.c"
    );
    assert!(firmware_source.contains(FULL_IR_DIGEST));
    assert!(firmware_source.contains(A_SOURCE_DIGEST));

    let metadata = TaskList::from_raw(golden_metadata())?;
    assert_eq!(metadata.revision_text, "9007199254740993");
    assert_eq!(metadata.task_store_id, STORE_ID);
    assert_eq!(metadata.tasks[0].slot, 0);
    assert_eq!(metadata.tasks[1].slot, 3);
    assert_eq!(sha256_hex(GOLDEN_IR.as_bytes()), FULL_IR_DIGEST);
    assert_eq!(sha256_hex(GOLDEN_SOURCE.as_bytes()), A_SOURCE_DIGEST);
    assert_eq!(
        sha256_hex(task_ir_section(GOLDEN_IR, "A")?.as_bytes()),
        A_IR_DIGEST
    );
    assert_eq!(
        sha256_hex(task_ir_section(GOLDEN_IR, "B")?.as_bytes()),
        B_IR_DIGEST
    );

    let mutation = encode_task_mutation(&metadata, TaskMutation::Delete { id: "A" })?;
    assert_eq!(
        std::str::from_utf8(&mutation)?,
        format!(
            concat!(
                "{{\"schema\":\"radxa-linkr-debugger.v1\",\"action\":\"delete\",",
                "\"expected_revision\":\"9007199254740993\",",
                "\"catalog_fingerprint\":\"{}\",\"id\":\"A\"}}"
            ),
            CATALOG
        )
    );
    let a_ir = task_ir_section(GOLDEN_IR, "A")?;
    let upsert = encode_task_mutation(
        &metadata,
        TaskMutation::Upsert {
            id: "A",
            source: Some(GOLDEN_SOURCE.as_bytes()),
            ir: &a_ir,
        },
    )?;
    let parsed: Value = serde_json::from_slice(&upsert)?;
    assert_eq!(parsed["schema"], "radxa-linkr-debugger.v1");
    assert_eq!(parsed["action"], "upsert");
    assert_eq!(parsed["expected_revision"], "9007199254740993");
    assert_eq!(parsed["catalog_fingerprint"], CATALOG);
    assert_eq!(parsed["id"], "A");
    assert_eq!(
        parsed["source_base64"],
        "eyJzY2hlbWEiOiJsaW5rci10ZXN0LnYxIiwibmFtZSI6IkEiLCJ2ZXJzaW9uIjoiMS4wIiwiY3JlYXRlZCI6IjIwMjYtMTAtMjJUMDA6MDA6MDAuMDAwWiJ9CnsiaWQiOiJzMSIsInBhcmFtcyI6eyJyYWlsIjoiNXZfb3V0In0sInR5cGUiOiJwb3dlcl9vbiJ9Cg=="
    );
    assert_eq!(
        parsed["source_base64"],
        base64_encode(GOLDEN_SOURCE.as_bytes())
    );
    assert_eq!(parsed["ir_base64"], base64_encode(a_ir.as_bytes()));
    assert_eq!(
        base64_decode(parsed["source_base64"].as_str().unwrap())?,
        GOLDEN_SOURCE.as_bytes()
    );
    assert_eq!(
        base64_decode(parsed["ir_base64"].as_str().unwrap())?,
        a_ir.as_bytes()
    );
    // Stable field order before the optional fields.
    let text = std::str::from_utf8(&upsert)?;
    let schema = text.find("\"schema\"").unwrap();
    let action = text.find("\"action\"").unwrap();
    let revision = text.find("\"expected_revision\"").unwrap();
    let catalog = text.find("\"catalog_fingerprint\"").unwrap();
    let id = text.find("\"id\"").unwrap();
    let source = text.find("\"source_base64\"").unwrap();
    let ir = text.find("\"ir_base64\"").unwrap();
    assert!(schema < action && action < revision && revision < catalog);
    assert!(catalog < id && id < source && source < ir);
    Ok(())
}

#[test]
fn worst_case_source_and_full_ir_fit_the_bounded_base64_body() -> Result<()> {
    let metadata = TaskList::from_raw(golden_metadata())?;
    // Old JSON-escaping worst case: quote, backslash and multi-byte UTF-8
    // in one 8-byte unit, repeated to exactly 4000 bytes.
    let unit = [b'\\', b'"', 0xe4, 0xb8, 0xad, 0xe6, 0x96, 0x87];
    let source: Vec<u8> = unit.iter().copied().cycle().take(4000).collect();
    let mut ir = String::from(
        "# linkr-task.v1\n# task A\n{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP7\",\"body\":\"{}\"}\n",
    );
    while ir.len() + 5 <= 4000 {
        ir.push_str("#pad\n");
    }
    while ir.len() < 4000 {
        ir.push('\n');
    }
    assert_eq!(ir.len(), 4000);
    let body = encode_task_mutation(
        &metadata,
        TaskMutation::Upsert {
            id: "A",
            source: Some(&source),
            ir: &ir,
        },
    )?;
    assert!(body.len() > 2 * 5336);
    assert!(body.len() <= TASK_MUTATION_MAX_BYTES);
    let parsed: Value = serde_json::from_slice(&body)?;
    assert_eq!(
        base64_decode(parsed["source_base64"].as_str().unwrap())?,
        source
    );
    assert_eq!(
        base64_decode(parsed["ir_base64"].as_str().unwrap())?,
        ir.as_bytes()
    );
    // Random binary-like source can travel in base64 but violates the
    // strict UTF-8/control source policy and never leaves the client.
    let binary: Vec<u8> = (0..64u32).map(|value| (value * 37 + 11) as u8).collect();
    let error = encode_task_mutation(
        &metadata,
        TaskMutation::Upsert {
            id: "A",
            source: Some(&binary),
            ir: &ir,
        },
    )
    .unwrap_err();
    assert!(matches!(
        &error,
        crate::task_error::TaskError::Command {
            code: "invalid_task",
            ..
        }
    ));
    // Oversize source is rejected as body_too_large before any transport.
    let oversize = vec![b'x'; 4001];
    let error = encode_task_mutation(
        &metadata,
        TaskMutation::Upsert {
            id: "A",
            source: Some(&oversize),
            ir: &ir,
        },
    )
    .unwrap_err();
    assert!(matches!(
        &error,
        crate::task_error::TaskError::Command {
            code: "body_too_large",
            ..
        }
    ));
    Ok(())
}

#[test]
fn golden_a_b_snapshot_and_exact_source_use_raw_digest_contract() -> Result<()> {
    let events = Rc::new(RefCell::new(Vec::new()));
    let transport = FakeTransport::new(
        vec![
            FakeReply::TaskList {
                metadata: golden_metadata(),
                ir: GOLDEN_IR.as_bytes().to_vec(),
            },
            FakeReply::TaskList {
                metadata: golden_metadata(),
                ir: GOLDEN_IR.as_bytes().to_vec(),
            },
            FakeReply::Bytes {
                status: StatusCode::OK,
                content_type: TASK_SOURCE_CONTENT_TYPE,
                body: GOLDEN_SOURCE.as_bytes().to_vec(),
            },
        ],
        events,
    );
    let api = TaskApi::new(&transport);

    let snapshot = api.snapshot()?;
    assert_eq!(snapshot.ir, GOLDEN_IR);
    assert_eq!(
        snapshot
            .metadata
            .tasks
            .iter()
            .map(|task| task.id.as_str())
            .collect::<Vec<_>>(),
        ["A", "B"]
    );
    let source = api.source("A")?;
    assert_eq!(source.source, GOLDEN_SOURCE.as_bytes());
    assert_eq!(
        transport.byte_requests.borrow()[3].path,
        "/api/v1/tasks/slots/0/source"
    );
    Ok(())
}

#[test]
fn raw_ir_digest_race_relists_once_and_content_type_is_strict() -> Result<()> {
    let events = Rc::new(RefCell::new(Vec::new()));
    let transport = FakeTransport::new(
        vec![
            FakeReply::Bytes {
                status: StatusCode::OK,
                content_type: "application/json",
                body: golden_metadata().into_bytes(),
            },
            FakeReply::Bytes {
                status: StatusCode::OK,
                content_type: crate::task_api::TASK_IR_CONTENT_TYPE,
                body: b"changed".to_vec(),
            },
            FakeReply::Bytes {
                status: StatusCode::OK,
                content_type: "application/json",
                body: golden_metadata().into_bytes(),
            },
            FakeReply::Bytes {
                status: StatusCode::OK,
                content_type: crate::task_api::TASK_IR_CONTENT_TYPE,
                body: GOLDEN_IR.as_bytes().to_vec(),
            },
        ],
        events,
    );
    assert_eq!(TaskApi::new(&transport).snapshot()?.ir, GOLDEN_IR);
    assert_eq!(transport.byte_requests.borrow().len(), 4);

    let events = Rc::new(RefCell::new(Vec::new()));
    let wrong_type = FakeTransport::new(
        vec![
            FakeReply::Bytes {
                status: StatusCode::OK,
                content_type: "application/json",
                body: list_output("", "").into_bytes(),
            },
            FakeReply::Bytes {
                status: StatusCode::OK,
                content_type: "text/plain",
                body: Vec::new(),
            },
        ],
        events,
    );
    let failure = TaskApi::new(&wrong_type)
        .snapshot()
        .expect_err("wrong raw IR content type must fail");
    assert_eq!(failure.code(), "invalid_response");
    Ok(())
}

#[test]
fn storing_b_after_a_is_one_ir_only_atomic_upsert_not_a_full_blob_overwrite() -> Result<()> {
    let a = blob(
        "A",
        &[record("/api/v1/power/5v_out", json!({"state": "on"}), 0)],
    );
    let b = blob(
        "B",
        &[record("/api/v1/power/5v_out", json!({"state": "off"}), 0)],
    );
    let file = TestFile::create(&b)?;
    let events = Rc::new(RefCell::new(Vec::new()));
    let transport = FakeTransport::new(
        vec![
            list_reply("A", &a),
            FakeReply::Output(mutation_success("upsert", absent_summary("B", 1, &b))),
        ],
        Rc::clone(&events),
    );
    let sleeper = RecordingSleeper { events };
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();

    let code = TaskRunner::new(&transport, &sleeper).run(
        &[
            "task".to_string(),
            "store".to_string(),
            file.0.to_string_lossy().into_owned(),
        ],
        TaskCommandIo::new(true, &mut stdout, &mut stderr),
    )?;

    assert_eq!(code, 0);
    assert!(stderr.is_empty());
    let requests = transport.byte_requests.borrow();
    assert_eq!(requests.len(), 2);
    assert_eq!(requests[1].path, "/api/v1/tasks/mutations");
    assert_eq!(requests[1].content_type, Some(TASK_MUTATION_CONTENT_TYPE));
    let parsed: Value = serde_json::from_slice(&requests[1].body)?;
    assert_eq!(parsed["action"], "upsert");
    assert_eq!(parsed["id"], "B");
    assert_eq!(parsed["expected_revision"], "9007199254740993");
    assert!(parsed.get("source_base64").is_none());
    assert_eq!(parsed["ir_base64"], base64_encode(b.as_bytes()));
    assert_ne!(parsed["ir_base64"], base64_encode(a.as_bytes()));
    Ok(())
}

#[test]
fn raw_ir_export_is_exact_and_full_import_round_trips_bytes() -> Result<()> {
    let destination = TestFile::create("")?;
    let export_events = Rc::new(RefCell::new(Vec::new()));
    let export_transport = FakeTransport::new(
        vec![FakeReply::TaskList {
            metadata: golden_metadata(),
            ir: GOLDEN_IR.as_bytes().to_vec(),
        }],
        Rc::clone(&export_events),
    );
    let export_sleeper = RecordingSleeper {
        events: export_events,
    };
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();
    let export_code = TaskRunner::new(&export_transport, &export_sleeper).run(
        &[
            "task".to_string(),
            "export".to_string(),
            destination.0.to_string_lossy().into_owned(),
        ],
        TaskCommandIo::new(true, &mut stdout, &mut stderr),
    )?;
    assert_eq!(export_code, 0);
    assert_eq!(fs::read(&destination.0)?, GOLDEN_IR.as_bytes());

    let import_events = Rc::new(RefCell::new(Vec::new()));
    let import_transport = FakeTransport::new(
        vec![
            list_reply("", ""),
            FakeReply::Output(mutation_success("import", Value::Null)),
        ],
        Rc::clone(&import_events),
    );
    let import_sleeper = RecordingSleeper {
        events: import_events,
    };
    stdout.clear();
    stderr.clear();
    let import_code = TaskRunner::new(&import_transport, &import_sleeper).run(
        &[
            "task".to_string(),
            "import".to_string(),
            destination.0.to_string_lossy().into_owned(),
        ],
        TaskCommandIo::new(true, &mut stdout, &mut stderr),
    )?;
    assert_eq!(import_code, 0);
    let requests = import_transport.byte_requests.borrow();
    assert_eq!(requests.len(), 2);
    let parsed: Value = serde_json::from_slice(&requests[1].body)?;
    assert_eq!(parsed["action"], "import");
    assert_eq!(parsed["ir_base64"], base64_encode(GOLDEN_IR.as_bytes()));
    assert_eq!(
        base64_decode(parsed["ir_base64"].as_str().unwrap())?,
        GOLDEN_IR.as_bytes()
    );
    assert!(parsed.get("id").is_none());
    assert!(parsed.get("source_base64").is_none());
    Ok(())
}

#[test]
fn source_path_comes_only_from_numeric_slot_even_for_percent_like_task_id() -> Result<()> {
    let task_id = "name%2Fwith/slash";
    let source = b"{\"schema\":\"linkr-test.v1\"}\n";
    let metadata = json!({
        "schema": "radxa-linkr-debugger.v1",
        "ok": true,
        "command": "tasks",
        "action": "list",
        "task_store_id": STORE_ID,
        "revision": "18446744073709551615",
        "catalog_fingerprint": CATALOG,
        "full_ir_bytes": 0,
        "full_ir_digest": sha256_hex(b""),
        "tasks": [{
            "slot": 3,
            "id": task_id,
            "request_count": 0,
            "source_state": "present",
            "source_bytes": source.len(),
            "source_digest": sha256_hex(source),
            "ir_bytes": 0,
            "task_ir_digest": sha256_hex(b""),
        }],
    })
    .to_string();
    let events = Rc::new(RefCell::new(Vec::new()));
    let transport = FakeTransport::new(
        vec![
            FakeReply::TaskList {
                metadata,
                ir: Vec::new(),
            },
            FakeReply::Bytes {
                status: StatusCode::OK,
                content_type: TASK_SOURCE_CONTENT_TYPE,
                body: source.to_vec(),
            },
        ],
        Rc::clone(&events),
    );
    let sleeper = RecordingSleeper { events };
    let destination = TestFile::create("")?;
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();
    let code = TaskRunner::new(&transport, &sleeper).run(
        &[
            "task".to_string(),
            "source".to_string(),
            task_id.to_string(),
            destination.0.to_string_lossy().into_owned(),
        ],
        TaskCommandIo::new(true, &mut stdout, &mut stderr),
    )?;
    assert_eq!(code, 0);
    assert!(stderr.is_empty());
    assert_eq!(fs::read(&destination.0)?, source);
    let output: Value = serde_json::from_slice(&stdout)?;
    assert_eq!(output["lossless_source"], true);
    assert_eq!(output["editor_opened"], false);
    let requests = transport.byte_requests.borrow();
    assert_eq!(requests[1].path, "/api/v1/tasks/slots/3/source");
    assert!(!requests[1].path.contains('%'));
    assert!(!requests[1].path.contains(task_id));
    Ok(())
}

#[test]
fn stable_error_codes_are_status_checked_and_never_retry_mutations() -> Result<()> {
    let cases = [
        ("revision_conflict", StatusCode::CONFLICT),
        ("catalog_conflict", StatusCode::CONFLICT),
        ("invalid_mutation", StatusCode::BAD_REQUEST),
        ("invalid_task", StatusCode::BAD_REQUEST),
        ("body_too_large", StatusCode::BAD_REQUEST),
        ("task_source_absent", StatusCode::NOT_FOUND),
        ("busy", StatusCode::CONFLICT),
        ("storage_full", StatusCode::INSUFFICIENT_STORAGE),
        ("readback_failed", StatusCode::INTERNAL_SERVER_ERROR),
        ("storage_error", StatusCode::INTERNAL_SERVER_ERROR),
    ];
    for (code, status) in cases {
        let mut error = json!({
            "schema": "radxa-linkr-debugger.v1",
            "ok": false,
            "command": "tasks",
            "error": {"code": code, "message": code},
        });
        if matches!(code, "revision_conflict" | "catalog_conflict") {
            error["task_store_id"] = json!(STORE_ID);
            error["revision"] = json!("9007199254740994");
            error["catalog_fingerprint"] = json!(CATALOG);
        }
        let events = Rc::new(RefCell::new(Vec::new()));
        let transport = FakeTransport::new(
            vec![
                list_reply("", ""),
                FakeReply::Bytes {
                    status,
                    content_type: "application/json",
                    body: error.to_string().into_bytes(),
                },
            ],
            events,
        );

        let failure = TaskApi::new(&transport)
            .mutate_latest(TaskMutation::Clear)
            .expect_err("firmware error must fail");
        assert_eq!(failure.code(), code);
        assert_eq!(transport.byte_requests.borrow().len(), 2);
        if matches!(code, "revision_conflict" | "catalog_conflict") {
            assert!(failure.to_string().contains("mutation was not retried"));
            assert!(failure.to_string().contains("9007199254740994"));
        }
    }
    Ok(())
}
