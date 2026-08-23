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
fn gpio_primary_from_input_always_drives_output_high() {
    for level in [false, true] {
        let model = gpio_model(true, level);
        let activation = resolve_activation(
            &model,
            &ControlItem::Gpio("GP13".to_string()),
            ControlIntent::Primary,
        );
        assert_eq!(
            activation,
            Activation::Immediate(ControlCommand::SetGpioOutput {
                name: "GP13".to_string(),
                value: true,
            }),
            "input gpio with level={level} must drive output HIGH"
        );
    }
}

#[test]
fn gpio_primary_toggles_output_level_like_the_web_ui() {
    let low = gpio_model(false, false);
    assert_eq!(
        resolve_activation(
            &low,
            &ControlItem::Gpio("GP13".to_string()),
            ControlIntent::Primary
        ),
        Activation::Immediate(ControlCommand::SetGpioOutput {
            name: "GP13".to_string(),
            value: true,
        })
    );

    let high = gpio_model(false, true);
    assert_eq!(
        resolve_activation(
            &high,
            &ControlItem::Gpio("GP13".to_string()),
            ControlIntent::Primary
        ),
        Activation::Immediate(ControlCommand::SetGpioOutput {
            name: "GP13".to_string(),
            value: false,
        })
    );
}

#[test]
fn gpio_restore_input_matches_web_ui_secondary_action() {
    let model = gpio_model(false, true);
    assert_eq!(
        resolve_activation(
            &model,
            &ControlItem::Gpio("GP13".to_string()),
            ControlIntent::RestoreInput
        ),
        Activation::Immediate(ControlCommand::SetGpioInput {
            name: "GP13".to_string(),
        })
    );
}

#[test]
fn restore_input_is_ignored_for_non_gpio_items() {
    let model = model();
    assert_eq!(
        resolve_activation(
            &model,
            &ControlItem::Power("12v_out".to_string()),
            ControlIntent::RestoreInput
        ),
        Activation::Ignored
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
fn switch_without_confirm_routes_immediately() {
    let model = switch_model("sd", "target", &["target", "usb-reader"], false);
    assert_eq!(
        resolve_activation(
            &model,
            &ControlItem::Switch("sd".to_string()),
            ControlIntent::Primary
        ),
        Activation::Immediate(ControlCommand::RouteSwitch {
            name: "sd".to_string(),
            route: "usb-reader".to_string(),
        })
    );
}

#[test]
fn switch_with_firmware_confirm_flag_requires_confirmation() {
    let model = switch_model("vin", "3.3v", &["3.3v", "1.8v"], true);
    assert_eq!(
        resolve_activation(
            &model,
            &ControlItem::Switch("vin".to_string()),
            ControlIntent::Primary
        ),
        Activation::Confirm(ConfirmableCommand::RouteSwitch {
            name: "vin".to_string(),
            route: "1.8v".to_string(),
        })
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
