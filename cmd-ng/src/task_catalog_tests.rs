use crate::task_catalog::FirmwareTaskCatalog;
use anyhow::Result;
use serde_json::json;

fn catalog_output(tasks: serde_json::Value) -> String {
    json!({
        "schema": "radxa-linkr-debugger.v1",
        "ok": true,
        "command": "task",
        "action": "catalog",
        "version": 1,
        "tasks": tasks,
    })
    .to_string()
}

fn task(id: &str) -> serde_json::Value {
    json!({
        "id": id,
        "name": "firmware-owned task",
        "requests": [{
            "method": "PUT",
            "path": "/api/v1/power/firmware-owned",
            "body": {"state": "off"},
            "wait_ms": 7,
        }],
        "cleanup": {
            "method": "PUT",
            "path": "/api/v1/gpio/firmware-cleanup",
            "body": {"direction": "input"},
            "wait_ms": 0,
        },
    })
}

fn catalog_tasks() -> Vec<serde_json::Value> {
    (0..6)
        .map(|index| task(&format!("builtin/firmware-owned/{index}")))
        .collect()
}

pub(crate) fn firmware_catalog_output(task_id: &str, request_count: usize) -> String {
    let requests = (0..request_count)
        .map(|index| {
            json!({
                "method": "PUT",
                "path": format!("/api/v1/power/firmware-owned-{index}"),
                "body": {"state": "off", "index": index},
                "wait_ms": if index + 1 == request_count { 0 } else { index + 1 },
            })
        })
        .collect::<Vec<_>>();
    catalog_output(json!([{
        "id": task_id,
        "name": "firmware-owned task",
        "requests": requests,
        "cleanup": {
            "method": "PUT",
            "path": "/api/v1/gpio/firmware-cleanup",
            "body": {"direction": "input"},
            "wait_ms": 0,
        },
    }]))
}

#[test]
fn firmware_catalog_rejects_unknown_request_fields() -> Result<()> {
    // Given: a catalog request with an unrecognized field.
    let mut tasks = catalog_tasks();
    let task = tasks
        .first_mut()
        .ok_or_else(|| anyhow::anyhow!("catalog test fixture must contain a task"))?;
    task["requests"][0]["unexpected"] = json!(true);

    // When: the catalog crosses the firmware boundary.
    let error = FirmwareTaskCatalog::from_raw(catalog_output(json!(tasks)))
        .expect_err("unknown catalog fields must fail closed");

    // Then: the failure remains a structured catalog error.
    assert_eq!(error.code(), "catalog_invalid");
    Ok(())
}

#[test]
fn firmware_catalog_rejects_duplicate_task_ids() -> Result<()> {
    // Given: two firmware catalog records with the same identifier.
    let mut tasks = catalog_tasks();
    tasks[5] = task("builtin/firmware-owned/0");

    // When: the catalog is decoded.
    let error = FirmwareTaskCatalog::from_raw(catalog_output(json!(tasks)))
        .expect_err("duplicate firmware task identifiers must fail closed");

    // Then: no ambiguous built-in task can be selected.
    assert_eq!(error.code(), "catalog_invalid");
    Ok(())
}
