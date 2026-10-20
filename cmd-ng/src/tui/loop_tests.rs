use super::actions::{activate_item, confirm_hardware, ControlIntent};
use super::config_result::ConfigJobResult;
use super::config_state::ConfigRequest;
use super::confirm::{ConfirmableCommand, HardwareConfirmation, CONFIRM_TIMEOUT};
use super::controls::ControlItem;
use super::gpio_gesture::GpioGestureInput;
use super::mock_board::{mock_server, Reply};
use super::model::{TuiModel, REBOOT_RECONNECT_TIMEOUT};
use super::runtime::{on_time_tick, poll_pd, process_event, EventDisposition};
use crate::client::DEFAULT_BASE_URL;
use crate::persistent_config::PersistentConfigStatus;
use crate::ws_status::{TuiPdStatus, WsStatusSnapshot};
use crossterm::event::{Event, KeyCode, KeyEvent, KeyModifiers};
use std::sync::{Arc, Barrier};
use std::thread;
use std::time::{Duration, Instant};

const PD_EMPTY: &str = r#"{"ok":true,"available":true,"pd":true,"capabilities_valid":false}"#;
const PD_CAPABILITIES: &str = r#"{"ok":true,"available":true,"pd":true,"capabilities_valid":true,"fixed_profiles":[{"voltage_mv":5000,"max_current_ma":3000}]}"#;
const PD_REPLUGGED: &str = r#"{"ok":true,"available":true,"pd":true,"capabilities_valid":true,"fixed_profiles":[{"voltage_mv":9000,"max_current_ma":2000}]}"#;
const STATUS_EMPTY: &str = r#"{}"#;
const ADC_EMPTY: &str =
    r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"adc","readings":[]}"#;

fn active_pd_snapshot() -> WsStatusSnapshot {
    WsStatusSnapshot {
        pd: Some(serde_json::from_str(PD_EMPTY).unwrap()),
        ..Default::default()
    }
}

fn wait_for_pd_read(model: &TuiModel) {
    let deadline = Instant::now() + Duration::from_secs(2);
    while !model.pd_worker.active_finished() && Instant::now() < deadline {
        thread::sleep(Duration::from_millis(1));
    }
    assert!(
        model.pd_worker.active_finished(),
        "PD worker did not finish"
    );
}

fn accept_normal_reboot(model: &mut TuiModel) -> Instant {
    activate_item(model, ControlItem::Reboot, ControlIntent::Primary).unwrap();
    confirm_hardware(model, Instant::now()).unwrap();
    model.reboot_reconnect.unwrap().accepted_at
}

#[test]
fn accepted_reboot_survives_five_seconds_of_disconnect_and_recovers() {
    let (url, requests) = mock_server(vec![
        Reply::Truncated(202, 128),
        Reply::Disconnect,
        Reply::Http(200, STATUS_EMPTY),
        Reply::Http(200, ADC_EMPTY),
    ]);
    let mut model = TuiModel::new(url, Duration::from_millis(100));
    let accepted_at = accept_normal_reboot(&mut model);
    assert!(requests
        .recv_timeout(Duration::from_secs(1))
        .unwrap()
        .starts_with("POST /api/v1/reboot HTTP/1.1"));

    on_time_tick(&mut model, accepted_at + Duration::from_secs(2)).unwrap();
    assert!(requests
        .recv_timeout(Duration::from_secs(1))
        .unwrap()
        .starts_with("GET /api/v1/status HTTP/1.1"));
    assert!(model.reboot_reconnect.is_some());
    assert!(model.err.is_none());
    assert_eq!(
        model.status,
        "Normal reboot accepted; waiting for reconnect (not ROM BOOTSEL)"
    );

    on_time_tick(&mut model, accepted_at + Duration::from_secs(3)).unwrap();
    assert!(
        requests.recv_timeout(Duration::from_millis(30)).is_err(),
        "reconnect retries must respect the bounded HTTP poll interval"
    );

    on_time_tick(&mut model, accepted_at + Duration::from_secs(5)).unwrap();
    assert!(requests
        .recv_timeout(Duration::from_secs(1))
        .unwrap()
        .starts_with("GET /api/v1/status HTTP/1.1"));
    assert!(requests
        .recv_timeout(Duration::from_secs(1))
        .unwrap()
        .starts_with("GET /api/v1/adc/read HTTP/1.1"));
    assert!(model.reboot_reconnect.is_none());
    assert!(model.err.is_none());
    assert_eq!(model.status, "HTTP mode");
}

#[test]
fn accepted_reboot_currently_accepts_first_online_snapshot_without_uptime_validation() {
    let (url, requests) = mock_server(vec![
        Reply::Truncated(202, 128),
        Reply::Http(200, STATUS_EMPTY),
        Reply::Http(200, ADC_EMPTY),
    ]);
    let mut model = TuiModel::new(url, Duration::from_millis(100));
    let accepted_at = accept_normal_reboot(&mut model);
    requests.recv_timeout(Duration::from_secs(1)).unwrap();

    on_time_tick(&mut model, accepted_at + Duration::from_secs(2)).unwrap();

    let status = requests.recv_timeout(Duration::from_secs(1)).unwrap();
    let adc = requests.recv_timeout(Duration::from_secs(1)).unwrap();
    assert!(status.starts_with("GET /api/v1/status HTTP/1.1"));
    assert!(adc.starts_with("GET /api/v1/adc/read HTTP/1.1"));
    assert!(model.reboot_reconnect.is_none());
    assert_eq!(model.status, "HTTP mode");
}

#[test]
fn accepted_reboot_disconnect_becomes_fatal_at_reconnect_deadline() {
    let (url, requests) = mock_server(vec![
        Reply::Truncated(202, 128),
        Reply::Disconnect,
        Reply::Disconnect,
    ]);
    let mut model = TuiModel::new(url, Duration::from_millis(100));
    let accepted_at = accept_normal_reboot(&mut model);
    requests.recv_timeout(Duration::from_secs(1)).unwrap();

    on_time_tick(&mut model, accepted_at + Duration::from_secs(2)).unwrap();
    requests.recv_timeout(Duration::from_secs(1)).unwrap();
    let error = on_time_tick(&mut model, accepted_at + REBOOT_RECONNECT_TIMEOUT)
        .unwrap_err()
        .to_string();

    assert!(requests
        .recv_timeout(Duration::from_secs(1))
        .unwrap()
        .starts_with("GET /api/v1/status HTTP/1.1"));
    assert!(error.contains("Normal reboot reconnect timed out after 30s"));
    assert!(model.reboot_reconnect.is_none());
    assert_eq!(model.err.as_deref(), Some(error.as_str()));
    assert_eq!(model.status, error);
}

#[test]
fn ordinary_http_poll_failure_without_reboot_remains_fatal() {
    let (url, requests) = mock_server(vec![Reply::Disconnect]);
    let mut model = TuiModel::new(url, Duration::from_millis(100));

    let error = on_time_tick(&mut model, Instant::now()).unwrap_err();

    assert!(requests
        .recv_timeout(Duration::from_secs(1))
        .unwrap()
        .starts_with("GET /api/v1/status HTTP/1.1"));
    assert!(!error.to_string().contains("reboot reconnect"));
    assert!(model.reboot_reconnect.is_none());
}

#[test]
fn runtime_discovers_pd_capabilities_asynchronously_with_one_request_in_flight() {
    let gate = Arc::new(Barrier::new(2));
    let (url, requests) = mock_server(vec![
        Reply::Http(
            200,
            r#"{"pd":{"available":true,"pd":true,"capabilities_valid":false}}"#,
        ),
        Reply::Http(
            200,
            r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"adc","readings":[]}"#,
        ),
        Reply::Gated(200, PD_CAPABILITIES, gate.clone()),
    ]);
    let mut model = TuiModel::new(url, Duration::from_secs(2));
    let now = Instant::now();
    on_time_tick(&mut model, now).unwrap();
    let status = requests.recv_timeout(Duration::from_secs(2)).unwrap();
    let adc = requests.recv_timeout(Duration::from_secs(2)).unwrap();
    let pd = requests.recv_timeout(Duration::from_secs(2)).unwrap();
    poll_pd(&mut model, now + Duration::from_secs(10));
    let still_in_flight = !model.pd_worker.active_finished();
    let no_duplicate = requests.try_recv().is_err();
    gate.wait();

    assert!(status.starts_with("GET /api/v1/status HTTP/1.1"));
    assert!(adc.starts_with("GET /api/v1/adc/read HTTP/1.1"));
    assert!(pd.starts_with("GET /api/v1/pd HTTP/1.1"));
    assert!(
        still_in_flight,
        "tick must return before the PD response is released"
    );
    assert!(no_duplicate);
    wait_for_pd_read(&model);
    poll_pd(&mut model, now + Duration::from_secs(10));
    assert!(model.pd.as_ref().unwrap().capabilities_valid);
    assert_eq!(
        model.pd.as_ref().unwrap().fixed_profiles[0].voltage_mv,
        5000
    );
    poll_pd(&mut model, now + Duration::from_secs(20));
    assert!(requests.try_recv().is_err());
}

#[test]
fn pd_discovery_backs_off_at_least_five_seconds_after_failure_or_empty_capabilities() {
    let (url, requests) = mock_server(vec![
        Reply::Http(503, r#"{"ok":false}"#),
        Reply::Http(200, PD_EMPTY),
        Reply::Http(200, PD_CAPABILITIES),
    ]);
    let mut model = TuiModel::new(url, Duration::from_secs(2));
    model.apply_status_snapshot(active_pd_snapshot());
    let now = Instant::now();
    for attempt in 0..3 {
        let started = now + Duration::from_secs(attempt * 7);
        let finished = started + Duration::from_secs(2);
        poll_pd(&mut model, started);
        assert!(requests
            .recv_timeout(Duration::from_secs(2))
            .unwrap()
            .starts_with("GET /api/v1/pd HTTP/1.1"));
        wait_for_pd_read(&model);
        poll_pd(&mut model, finished);
        if attempt == 0 {
            assert!(model.err.is_some());
        }
        poll_pd(&mut model, finished + Duration::from_millis(4999));
        assert!(
            requests.try_recv().is_err(),
            "attempt {attempt} retried before 5s"
        );
    }
    assert!(model.pd.as_ref().unwrap().capabilities_valid);
}

#[test]
fn pd_disconnect_clears_capabilities_and_backoff_for_replug() {
    let (url, requests) = mock_server(vec![
        Reply::Http(200, PD_CAPABILITIES),
        Reply::Http(200, PD_REPLUGGED),
    ]);
    let mut model = TuiModel::new(url, Duration::from_secs(2));
    model.apply_status_snapshot(active_pd_snapshot());
    let now = Instant::now();
    poll_pd(&mut model, now);
    requests.recv_timeout(Duration::from_secs(2)).unwrap();
    wait_for_pd_read(&model);
    poll_pd(&mut model, now);
    let mut inactive = model.pd.clone().unwrap();
    inactive.pd = false;
    inactive.pps_ranges.push(Default::default());
    inactive.avs_ranges.push(Default::default());
    model.apply_status_snapshot(WsStatusSnapshot {
        pd: Some(inactive),
        ..Default::default()
    });
    let pd = model.pd.as_ref().unwrap();
    assert!(!pd.capabilities_valid);
    assert!(pd.fixed_profiles.is_empty() && pd.pps_ranges.is_empty() && pd.avs_ranges.is_empty());
    poll_pd(&mut model, now + Duration::from_millis(100));
    assert!(requests.try_recv().is_err());

    model.apply_status_snapshot(active_pd_snapshot());
    poll_pd(&mut model, now + Duration::from_secs(1));
    requests.recv_timeout(Duration::from_secs(2)).unwrap();
    wait_for_pd_read(&model);
    poll_pd(&mut model, now + Duration::from_secs(1));
    assert_eq!(
        model.pd.as_ref().unwrap().fixed_profiles[0].voltage_mv,
        9000
    );
}

#[test]
fn pd_replug_discards_old_in_flight_capabilities_without_overlapping_reads() {
    let gate = Arc::new(Barrier::new(2));
    let (url, requests) = mock_server(vec![
        Reply::Gated(200, PD_CAPABILITIES, gate.clone()),
        Reply::Http(200, PD_REPLUGGED),
    ]);
    let mut model = TuiModel::new(url, Duration::from_secs(2));
    model.apply_status_snapshot(active_pd_snapshot());
    let now = Instant::now();
    poll_pd(&mut model, now);
    requests.recv_timeout(Duration::from_secs(2)).unwrap();
    model.apply_status_snapshot(WsStatusSnapshot {
        pd: Some(TuiPdStatus::default()),
        ..Default::default()
    });
    model.apply_status_snapshot(active_pd_snapshot());
    poll_pd(&mut model, now + Duration::from_secs(1));
    let no_duplicate = requests.try_recv().is_err();
    gate.wait();
    assert!(no_duplicate);

    wait_for_pd_read(&model);
    poll_pd(&mut model, now + Duration::from_secs(1));
    assert!(!model.pd.as_ref().unwrap().capabilities_valid);
    requests.recv_timeout(Duration::from_secs(2)).unwrap();
    wait_for_pd_read(&model);
    poll_pd(&mut model, now + Duration::from_secs(1));
    assert_eq!(
        model.pd.as_ref().unwrap().fixed_profiles[0].voltage_mv,
        9000
    );
}

#[test]
fn time_tick_sets_last_http_poll_when_due() {
    let mut model = TuiModel::new("http://127.0.0.1:9".to_string(), Duration::from_secs(2));
    let _ = on_time_tick(&mut model, Instant::now());
    assert!(model.last_http_poll.is_some());
}

#[test]
fn time_tick_does_nothing_when_paused() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.paused = true;
    on_time_tick(&mut model, Instant::now()).unwrap();
    assert!(model.last_http_poll.is_none());
}

#[test]
fn paused_tick_cancels_a_pending_gpio_gesture_without_dispatching() {
    let start = Instant::now();
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.paused = true;
    let _ = model.gpio_gesture.down(
        GpioGestureInput {
            pin: Some("GP10"),
            column: 0,
            row: 0,
        },
        start,
    );
    model.gpio_gesture.up(
        GpioGestureInput {
            pin: Some("GP10"),
            column: 0,
            row: 0,
        },
        start,
    );
    on_time_tick(&mut model, start + Duration::from_millis(220)).unwrap();
    assert_eq!(model.gpio_pending, None);
}

#[test]
fn saved_config_error_cancels_a_pending_gpio_gesture_without_dispatching() {
    let start = Instant::now();
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.last_http_poll = Some(Instant::now());
    model.saved_config.error = Some("failure".to_string());
    let _ = model.gpio_gesture.down(
        GpioGestureInput {
            pin: Some("GP10"),
            column: 0,
            row: 0,
        },
        start,
    );
    model.gpio_gesture.up(
        GpioGestureInput {
            pin: Some("GP10"),
            column: 0,
            row: 0,
        },
        start,
    );
    on_time_tick(&mut model, start + Duration::from_millis(220)).unwrap();
    assert_eq!(model.gpio_pending, None);
}

#[test]
fn config_error_created_in_same_tick_blocks_due_gpio_gesture() {
    let start = Instant::now();
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.last_http_poll = Some(start);
    model.saved_config.summary = Some(PersistentConfigStatus {
        available: true,
        reason: "ready".to_string(),
        saved_count: 0,
        pending_count: 0,
    });
    model.saved_config.loaded = true;
    model.gpio_names = vec!["GP10".to_string()];
    let _ = model.gpio_gesture.down(
        GpioGestureInput {
            pin: Some("GP10"),
            column: 0,
            row: 0,
        },
        start,
    );
    model.gpio_gesture.up(
        GpioGestureInput {
            pin: Some("GP10"),
            column: 0,
            row: 0,
        },
        start,
    );
    model
        .config_worker
        .queue_for_test(ConfigJobResult::mutation(
            ConfigRequest::Clear,
            Err("same-tick config mutation failure".to_string()),
            Err("same-tick config refresh failure".to_string()),
        ));
    on_time_tick(&mut model, start + Duration::from_millis(220)).unwrap();
    assert!(model.saved_config.error.is_some());
    assert_eq!(model.gpio_pending, None);
    assert!(!model.gpio_gesture.is_active());
}

#[test]
fn queued_escape_cancels_before_an_overdue_gesture_tick() {
    let start = Instant::now();
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.last_http_poll = Some(start);
    model.gpio_names = vec!["GP10".to_string()];
    let _ = model.gpio_gesture.down(
        GpioGestureInput {
            pin: Some("GP10"),
            column: 0,
            row: 0,
        },
        start,
    );
    let disposition = process_event(
        &mut model,
        Event::Key(KeyEvent::new(KeyCode::Esc, KeyModifiers::NONE)),
        start + Duration::from_millis(600),
    )
    .unwrap();
    assert_eq!(disposition, EventDisposition::Continue);
    on_time_tick(&mut model, start + Duration::from_millis(600)).unwrap();
    assert_eq!(model.gpio_pending, None);
}

#[test]
fn snapshot_pin_removal_cancels_down_and_await_second_gestures() {
    let start = Instant::now();
    for release in [false, true] {
        let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
        model.last_http_poll = Some(start);
        model.gpio_names = vec!["GP10".to_string()];
        let _ = model.gpio_gesture.down(
            GpioGestureInput {
                pin: Some("GP10"),
                column: 0,
                row: 0,
            },
            start,
        );
        if release {
            model.gpio_gesture.up(
                GpioGestureInput {
                    pin: Some("GP10"),
                    column: 0,
                    row: 0,
                },
                start,
            );
        }
        model.apply_status_snapshot(WsStatusSnapshot::default());
        on_time_tick(&mut model, start + Duration::from_millis(600)).unwrap();
        assert_eq!(model.gpio_pending, None, "release={release}");
    }
}

#[test]
fn time_tick_expires_hardware_confirmation_at_the_timeout_boundary() {
    let now = Instant::now();
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.last_http_poll = Some(now - Duration::from_secs(1));
    model.hardware_confirm = Some(HardwareConfirmation {
        command: ConfirmableCommand::SetPower {
            output: "12v_out".to_string(),
            next_state: true,
        },
        started: now - CONFIRM_TIMEOUT,
    });

    on_time_tick(&mut model, now).unwrap();

    assert!(model.hardware_confirm.is_none());
    assert_eq!(model.status, "Power confirmation timed out");
    assert_eq!(model.last_http_poll, Some(now));
}

#[test]
fn time_tick_keeps_fresh_hardware_confirmation() {
    let now = Instant::now();
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.last_http_poll = Some(now);
    model.hardware_confirm = Some(HardwareConfirmation {
        command: ConfirmableCommand::RouteSwitch {
            name: "tf_wp".to_string(),
            route: "protected".to_string(),
        },
        started: now - CONFIRM_TIMEOUT + Duration::from_nanos(1),
    });

    on_time_tick(&mut model, now).unwrap();

    assert!(model.hardware_confirm.is_some());
}

#[test]
fn time_tick_expires_stale_hardware_confirmation_while_paused() {
    let now = Instant::now();
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.paused = true;
    model.hardware_confirm = Some(HardwareConfirmation {
        command: ConfirmableCommand::SetPower {
            output: "5v_out".to_string(),
            next_state: false,
        },
        started: now - CONFIRM_TIMEOUT,
    });

    on_time_tick(&mut model, now).unwrap();

    assert!(model.hardware_confirm.is_none());
    assert_eq!(model.status, "Power confirmation timed out");
    assert_eq!(model.last_http_poll, Some(now));
}
