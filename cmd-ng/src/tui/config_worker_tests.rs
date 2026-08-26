use super::config_io::ConfigWorker;
use super::config_result::ConfigJobResult;
use super::config_state::ConfigRequest;
use super::confirm::{ConfirmableCommand, HardwareConfirmation};
use super::events::KeyOutcome;
use super::gpio_gesture::GpioGestureInput;
use super::handle_key;
use super::mock_board::{mock_server, Reply};
use super::model::TuiModel;
use super::runtime::on_time_tick;
use crate::persistent_config::{
    ConfigAction, ConfigItemId, PersistentConfigResponse, PersistentConfigStatus,
};
use crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
use std::sync::{Arc, Barrier};
use std::time::{Duration, Instant};

const SHOW: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":1},"pending":0,"items":[{"id":"power/alpha","kind":"power","current":{"state":"off"},"saved":{"state":"on"},"selected":true,"requires_confirm":true,"apply_state":"applied"}]}"#;
const CONFIRM: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"save","error":{"code":"confirmation_required","message":"confirm"},"dangerous_items":["power/alpha"]}"#;

fn poll_until(worker: &mut ConfigWorker) -> ConfigJobResult {
    let deadline = Instant::now() + Duration::from_secs(2);
    loop {
        if let Some(result) = worker.poll() {
            return result;
        }
        assert!(Instant::now() < deadline, "worker never produced a result");
        std::thread::sleep(Duration::from_millis(2));
    }
}

fn response(raw: &str, action: ConfigAction, status: u16) -> PersistentConfigResponse {
    let response = PersistentConfigResponse::from_raw(raw.to_string()).unwrap();
    response.validate(&action, Some(status)).unwrap();
    response
}

fn confirmation_result() -> ConfigJobResult {
    ConfigJobResult::mutation(
        ConfigRequest::Save {
            items: vec![ConfigItemId("power/alpha".to_string())],
            confirm: false,
        },
        Ok(response(CONFIRM, ConfigAction::Save, 409)),
        Ok(response(SHOW, ConfigAction::Get, 200)),
    )
}

fn model_with_hardware_confirmation(now: Instant) -> TuiModel {
    let mut model = TuiModel::new("http://127.0.0.1:9".to_string(), Duration::from_secs(1));
    model.last_http_poll = Some(now);
    model.hardware_confirm = Some(HardwareConfirmation {
        command: ConfirmableCommand::SetPower {
            output: "12v_out".to_string(),
            next_state: true,
        },
        started: now,
    });
    model.saved_config.summary = Some(PersistentConfigStatus {
        available: true,
        reason: "ready".to_string(),
        saved_count: 0,
        pending_count: 0,
    });
    let _ = model.gpio_gesture.down(
        GpioGestureInput {
            pin: Some("GP10"),
            column: 0,
            row: 0,
        },
        now,
    );
    model
}

#[test]
fn poll_drains_the_real_result_queued_after_empty_before_starting_the_next_job() {
    let gate = Arc::new(Barrier::new(2));
    let (url, requests) = mock_server(vec![
        Reply::Gated(200, SHOW, gate.clone()),
        Reply::Http(200, SHOW),
    ]);
    let mut worker = ConfigWorker::new();
    assert!(worker.start(url.clone(), Duration::from_secs(1), ConfigRequest::Refresh));

    let hook_gate = gate.clone();
    worker.empty_hook = Some(Box::new(move |worker: &ConfigWorker| {
        hook_gate.wait();
        let deadline = Instant::now() + Duration::from_secs(2);
        while !worker.active_finished() {
            assert!(Instant::now() < deadline, "worker never finished");
            std::thread::sleep(Duration::from_millis(1));
        }
    }));

    let first = poll_until(&mut worker);
    assert!(
        first.refresh.is_ok(),
        "the queued result must win over stopped"
    );
    assert!(requests.recv().unwrap().starts_with("GET /api/v1/config"));
    assert!(
        worker.poll().is_none(),
        "the queued result must be drained once"
    );

    assert!(worker.start(url, Duration::from_secs(1), ConfigRequest::Refresh));
    let second = poll_until(&mut worker);
    assert!(
        second.refresh.is_ok(),
        "no stale result may leak into the next job"
    );
    assert!(requests.recv().unwrap().starts_with("GET /api/v1/config"));
}

#[test]
fn config_confirmation_completion_clears_hardware_confirmation_and_cancels_gesture() {
    let now = Instant::now();
    let mut model = model_with_hardware_confirmation(now);
    model.config_worker.queue_for_test(confirmation_result());

    on_time_tick(&mut model, now).unwrap();

    assert!(model.saved_config.confirmation().is_some());
    assert!(model.hardware_confirm.is_none());
    assert!(!model.gpio_gesture.is_active());
    assert_eq!(model.status, "Saved Config confirmation required");
}

#[test]
fn config_error_completion_clears_hardware_confirmation_and_cancels_gesture() {
    let now = Instant::now();
    let mut model = model_with_hardware_confirmation(now);
    model
        .config_worker
        .queue_for_test(ConfigJobResult::transport(
            ConfigRequest::Clear,
            "config transport failure".to_string(),
        ));

    on_time_tick(&mut model, now).unwrap();

    assert!(model.saved_config.error.is_some());
    assert!(model.hardware_confirm.is_none());
    assert!(!model.gpio_gesture.is_active());
    assert_eq!(model.status, "Saved Config request failed");
}

#[test]
fn consecutive_enter_after_config_confirmation_never_executes_hidden_hardware_action() {
    let now = Instant::now();
    let mut model = model_with_hardware_confirmation(now);
    model.config_worker.queue_for_test(confirmation_result());
    on_time_tick(&mut model, now).unwrap();

    assert_eq!(
        handle_key(
            &mut model,
            KeyEvent::new(KeyCode::Enter, KeyModifiers::NONE),
            now,
        )
        .unwrap(),
        KeyOutcome::Redraw
    );
    assert_eq!(
        handle_key(
            &mut model,
            KeyEvent::new(KeyCode::Enter, KeyModifiers::NONE),
            now,
        )
        .unwrap(),
        KeyOutcome::Continue
    );
    assert!(model.hardware_confirm.is_none());
}
