use super::config_result::ConfigJobResult;
use super::config_state::ConfigRequest;
use super::confirm::{ConfirmableCommand, HardwareConfirmation, CONFIRM_TIMEOUT};
use super::gpio_gesture::GpioGestureInput;
use super::model::TuiModel;
use super::runtime::{on_time_tick, process_event, EventDisposition};
use crate::client::DEFAULT_BASE_URL;
use crate::persistent_config::PersistentConfigStatus;
use crate::ws_status::WsStatusSnapshot;
use crossterm::event::{Event, KeyCode, KeyEvent, KeyModifiers};
use std::time::{Duration, Instant};

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
            name: "vin".to_string(),
            route: "1.8v".to_string(),
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
