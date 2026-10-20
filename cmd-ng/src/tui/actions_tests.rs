use super::actions::{
    activate_item, cancel_hardware, confirm_hardware, expire_hardware_confirmation,
    resolve_activation, Activation, ControlCommand, ControlIntent,
};
use super::confirm::{ConfirmableCommand, CONFIRM_TIMEOUT};
use super::controls::ControlItem;
use super::mock_board::{mock_server, Reply};
use super::model::{TuiModel, REBOOT_RECONNECT_TIMEOUT};
use crate::client::DEFAULT_BASE_URL;
use crate::ws_status::{TuiPdFixedProfile, TuiPdStatus, TuiStatusSwitchInfo, WsStatusSnapshot};
use std::time::{Duration, Instant};

fn model() -> TuiModel {
    TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2))
}

fn gpio_model(direction_input: bool, level: bool) -> TuiModel {
    let mut model = model();
    model.gpio_names = vec!["GP13".to_string()];
    model
        .gpio_is_input
        .insert("GP13".to_string(), direction_input);
    model.gpio_levels.insert("GP13".to_string(), level);
    model
}

fn switch_model(name: &str, route: &str, routes: &[&str], requires_confirm: bool) -> TuiModel {
    let mut model = model();
    let mut snapshot = WsStatusSnapshot::default();
    snapshot.switches.insert(
        name.to_string(),
        TuiStatusSwitchInfo {
            route: route.to_string(),
            routes: routes.iter().map(|route| (*route).to_string()).collect(),
            requires_confirm,
        },
    );
    model.apply_status_snapshot(snapshot);
    model
}

#[test]
fn gpio_primary_is_ignored_while_drive_low_drives_output_low() {
    for direction_input in [false, true] {
        for level in [false, true] {
            let model = gpio_model(direction_input, level);
            assert_eq!(
                resolve_activation(
                    &model,
                    &ControlItem::Gpio("GP13".to_string()),
                    ControlIntent::Primary
                ),
                Activation::Ignored
            );
            assert_eq!(
                resolve_activation(
                    &model,
                    &ControlItem::Gpio("GP13".to_string()),
                    ControlIntent::DriveLow
                ),
                Activation::Immediate(ControlCommand::SetGpioOutput {
                    name: "GP13".to_string(),
                    value: false,
                }),
                "gpio with input={direction_input} level={level} must drive LOW"
            );
        }
    }
}

#[test]
fn gpio_drive_high_always_drives_output_high() {
    for direction_input in [false, true] {
        for level in [false, true] {
            let model = gpio_model(direction_input, level);
            assert_eq!(
                resolve_activation(
                    &model,
                    &ControlItem::Gpio("GP13".to_string()),
                    ControlIntent::DriveHigh
                ),
                Activation::Immediate(ControlCommand::SetGpioOutput {
                    name: "GP13".to_string(),
                    value: true,
                }),
                "gpio with input={direction_input} level={level} must drive HIGH"
            );
        }
    }
}

#[test]
fn gpio_set_input_resolves_the_input_command() {
    let model = gpio_model(false, true);
    assert_eq!(
        resolve_activation(
            &model,
            &ControlItem::Gpio("GP13".to_string()),
            ControlIntent::SetInput
        ),
        Activation::Immediate(ControlCommand::SetGpioInput {
            name: "GP13".to_string(),
        })
    );
}

#[test]
fn direct_gpio_intents_are_ignored_for_non_gpio_items() {
    let model = model();
    for intent in [
        ControlIntent::DriveLow,
        ControlIntent::DriveHigh,
        ControlIntent::SetInput,
    ] {
        for item in [
            ControlItem::Power("12v_out".to_string()),
            ControlItem::Pd,
            ControlItem::Switch("sd".to_string()),
            ControlItem::Reboot,
        ] {
            assert_eq!(
                resolve_activation(&model, &item, intent),
                Activation::Ignored,
                "{item:?} with {intent:?} must stay inert"
            );
        }
    }
}

#[test]
fn reboot_primary_requires_confirmation() {
    assert_eq!(
        resolve_activation(&model(), &ControlItem::Reboot, ControlIntent::Primary),
        Activation::Confirm(ConfirmableCommand::Reboot)
    );
}

#[test]
fn confirmed_reboot_posts_once_and_defers_the_next_status_poll() {
    let (base_url, requests) = mock_server(vec![
        Reply::Truncated(202, 128),
        Reply::Http(200, r#"{"ok":true}"#),
    ]);
    let mut model = TuiModel::new(base_url, Duration::from_secs(1));
    activate_item(&mut model, ControlItem::Reboot, ControlIntent::Primary).unwrap();
    let now = Instant::now();

    confirm_hardware(&mut model, now).unwrap();
    let completed = Instant::now();

    let request = requests.recv_timeout(Duration::from_secs(1)).unwrap();
    assert!(request.starts_with("POST /api/v1/reboot HTTP/1.1"));
    assert!(
        requests.recv_timeout(Duration::from_millis(30)).is_err(),
        "reboot must only be posted once and must not trigger an immediate refresh"
    );
    assert!(model.hardware_confirm.is_none());
    let reconnect = model.reboot_reconnect.unwrap();
    assert!(now <= reconnect.accepted_at && reconnect.accepted_at <= completed);
    assert_eq!(
        reconnect.deadline,
        reconnect.accepted_at + REBOOT_RECONNECT_TIMEOUT
    );
    assert_eq!(model.last_http_poll, Some(reconnect.accepted_at));
    assert_eq!(
        model.status,
        "Normal reboot accepted; waiting for reconnect (not ROM BOOTSEL)"
    );
}

#[test]
fn cancelled_or_expired_reboot_confirmation_sends_no_request() {
    for expire in [false, true] {
        let (base_url, requests) = mock_server(vec![Reply::Http(200, r#"{"ok":true}"#)]);
        let mut model = TuiModel::new(base_url, Duration::from_millis(30));
        activate_item(&mut model, ControlItem::Reboot, ControlIntent::Primary).unwrap();
        let started = model.hardware_confirm.as_ref().unwrap().started;
        if expire {
            assert!(expire_hardware_confirmation(
                &mut model,
                started + CONFIRM_TIMEOUT
            ));
            assert_eq!(model.status, "Normal reboot confirmation timed out");
        } else {
            cancel_hardware(&mut model);
            assert_eq!(model.status, "Normal reboot cancelled");
        }
        assert!(model.hardware_confirm.is_none());
        assert_eq!(
            model.last_http_poll,
            if expire {
                Some(started + CONFIRM_TIMEOUT)
            } else {
                None
            }
        );
        assert!(
            requests.recv_timeout(Duration::from_millis(30)).is_err(),
            "expire={expire} must issue zero requests"
        );
    }
}

#[test]
fn pd_primary_cycles_fixed_voltage_with_confirmation() {
    let mut model = model();
    model.pd = Some(TuiPdStatus {
        available: true,
        voltage_mv: 9000,
        fixed_profiles: vec![
            TuiPdFixedProfile {
                voltage_mv: 5000,
                max_current_ma: 3000,
            },
            TuiPdFixedProfile {
                voltage_mv: 9000,
                max_current_ma: 3000,
            },
            TuiPdFixedProfile {
                voltage_mv: 12000,
                max_current_ma: 2500,
            },
        ],
        ..Default::default()
    });
    assert_eq!(
        resolve_activation(&model, &ControlItem::Pd, ControlIntent::Primary),
        Activation::Confirm(ConfirmableCommand::SetPd { voltage_mv: 12000 })
    );
}

#[test]
fn power_primary_always_requires_confirmation_with_computed_next_state() {
    let mut model = model();
    assert_eq!(
        resolve_activation(
            &model,
            &ControlItem::Power("12v_out".to_string()),
            ControlIntent::Primary
        ),
        Activation::Confirm(ConfirmableCommand::SetPower {
            output: "12v_out".to_string(),
            next_state: true,
        })
    );

    model.power_states.insert("12v_out".to_string(), true);
    assert_eq!(
        resolve_activation(
            &model,
            &ControlItem::Power("12v_out".to_string()),
            ControlIntent::Primary
        ),
        Activation::Confirm(ConfirmableCommand::SetPower {
            output: "12v_out".to_string(),
            next_state: false,
        })
    );
}

#[test]
fn every_advertised_switch_requires_confirmation_in_firmware_route_order() {
    for (name, route, routes, requires_confirm, next_route) in [
        (
            "sd",
            "target",
            ["target", "usb-reader", "pc"],
            false,
            "usb-reader",
        ),
        (
            "future_mux",
            "route-b",
            ["route-a", "route-b", "route-c"],
            true,
            "route-c",
        ),
    ] {
        let model = switch_model(name, route, &routes, requires_confirm);
        assert_eq!(
            resolve_activation(
                &model,
                &ControlItem::Switch(name.to_string()),
                ControlIntent::Primary
            ),
            Activation::Confirm(ConfirmableCommand::RouteSwitch {
                name: name.to_string(),
                route: next_route.to_string(),
            })
        );
    }
}

#[test]
fn switch_missing_from_current_state_is_ignored() {
    assert_eq!(
        resolve_activation(
            &model(),
            &ControlItem::Switch("missing".to_string()),
            ControlIntent::Primary
        ),
        Activation::Ignored
    );
}

#[test]
fn switch_without_advertised_routes_is_rejected() {
    let model = switch_model("sd", "", &[], false);
    assert_eq!(
        resolve_activation(
            &model,
            &ControlItem::Switch("sd".to_string()),
            ControlIntent::Primary
        ),
        Activation::Rejected("switch sd has no advertised routes".to_string())
    );
}
