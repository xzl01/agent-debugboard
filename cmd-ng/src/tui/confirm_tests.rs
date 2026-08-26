use super::confirm::{ConfirmableCommand, HardwareConfirmation, CONFIRM_TIMEOUT};
use std::time::{Duration, Instant};

#[test]
fn confirmation_expires_at_the_exact_timeout_boundary() {
    let now = Instant::now();
    let command = ConfirmableCommand::SetPower {
        output: "12v_out".to_string(),
        next_state: true,
    };
    let confirmation = HardwareConfirmation {
        command,
        started: now,
    };
    assert!(!confirmation.expired_at(now + CONFIRM_TIMEOUT - Duration::from_nanos(1)));
    assert!(confirmation.expired_at(now + CONFIRM_TIMEOUT));
}

#[test]
fn power_confirmation_texts_name_the_exact_target() {
    let command = ConfirmableCommand::SetPower {
        output: "12v_out".to_string(),
        next_state: true,
    };
    assert_eq!(command.title(), "Confirm Power Toggle");
    assert_eq!(command.target_text(), "power 12v_out: off -> on");
    assert_eq!(command.cancel_message(), "Power toggle cancelled");
    assert_eq!(command.timeout_message(), "Power confirmation timed out");
}

#[test]
fn switch_confirmation_texts_name_the_exact_route() {
    let command = ConfirmableCommand::RouteSwitch {
        name: "vin".to_string(),
        route: "1.8v".to_string(),
    };
    assert_eq!(command.title(), "Confirm Switch Route");
    assert_eq!(command.target_text(), "switch vin -> 1.8v");
    assert_eq!(command.cancel_message(), "Switch cancelled");
    assert_eq!(command.timeout_message(), "Switch confirmation timed out");
}
