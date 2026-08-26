use super::actions::{resolve_activation, Activation, ControlCommand, ControlIntent};
use super::confirm::ConfirmableCommand;
use super::controls::ControlItem;
use super::model::TuiModel;
use crate::client::DEFAULT_BASE_URL;
use crate::ws_status::{TuiStatusSwitchInfo, WsStatusSnapshot};
use std::time::Duration;

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
            ControlItem::Switch("sd".to_string()),
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
