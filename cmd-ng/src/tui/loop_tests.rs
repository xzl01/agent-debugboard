use super::confirm::{ConfirmableCommand, HardwareConfirmation, CONFIRM_TIMEOUT};
use super::model::TuiModel;
use super::on_time_tick;
use crate::client::DEFAULT_BASE_URL;
use std::time::{Duration, Instant};

#[test]
fn time_tick_sets_last_http_poll_when_due() {
    let mut model = TuiModel::new("http://127.0.0.1:9".to_string(), Duration::from_secs(2));
    let _ = on_time_tick(&mut model);
    assert!(model.last_http_poll.is_some());
}

#[test]
fn time_tick_does_nothing_when_paused() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.paused = true;
    on_time_tick(&mut model).unwrap();
    assert!(model.last_http_poll.is_none());
}

#[test]
fn time_tick_expires_stale_hardware_confirmation() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.last_http_poll = Some(Instant::now());
    model.hardware_confirm = Some(HardwareConfirmation {
        command: ConfirmableCommand::SetPower {
            output: "12v_out".to_string(),
            next_state: true,
        },
        started: Instant::now() - CONFIRM_TIMEOUT - Duration::from_millis(1),
    });

    on_time_tick(&mut model).unwrap();

    assert!(model.hardware_confirm.is_none());
    assert_eq!(model.status, "Power confirmation timed out");
}

#[test]
fn time_tick_keeps_fresh_hardware_confirmation() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.last_http_poll = Some(Instant::now());
    model.hardware_confirm = Some(HardwareConfirmation::new(ConfirmableCommand::RouteSwitch {
        name: "vin".to_string(),
        route: "1.8v".to_string(),
    }));

    on_time_tick(&mut model).unwrap();

    assert!(model.hardware_confirm.is_some());
}

#[test]
fn time_tick_expires_stale_hardware_confirmation_while_paused() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.paused = true;
    model.hardware_confirm = Some(HardwareConfirmation {
        command: ConfirmableCommand::SetPower {
            output: "5v_out".to_string(),
            next_state: false,
        },
        started: Instant::now() - CONFIRM_TIMEOUT - Duration::from_millis(1),
    });

    on_time_tick(&mut model).unwrap();

    assert!(model.hardware_confirm.is_none());
    assert_eq!(model.status, "Power confirmation timed out");
}
