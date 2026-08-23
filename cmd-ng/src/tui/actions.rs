use super::board_io::{perform_control_action, set_gpio_input, set_gpio_output, set_switch_route};
use super::confirm::{ConfirmableCommand, HardwareConfirmation};
use super::controls::{next_switch_route, ControlItem};
use super::model::TuiModel;
use anyhow::Result;
use std::time::{Duration, Instant};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum ControlIntent {
    Primary,
    RestoreInput,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) enum ControlCommand {
    SetGpioOutput { name: String, value: bool },
    SetGpioInput { name: String },
    RouteSwitch { name: String, route: String },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) enum Activation {
    Immediate(ControlCommand),
    Confirm(ConfirmableCommand),
    Rejected(String),
    Ignored,
}

pub(super) fn resolve_activation(
    model: &TuiModel,
    item: &ControlItem,
    intent: ControlIntent,
) -> Activation {
    match (item, intent) {
        (ControlItem::Power(output), ControlIntent::Primary) => {
            let current_state = *model.power_states.get(output).unwrap_or(&false);
            Activation::Confirm(ConfirmableCommand::SetPower {
                output: output.clone(),
                next_state: !current_state,
            })
        }
        (ControlItem::Switch(name), ControlIntent::Primary) => {
            let Some(state) = model.switches.get(name) else {
                return Activation::Ignored;
            };
            let Some(next_route) = next_switch_route(state) else {
                return Activation::Rejected(format!("switch {name} has no advertised routes"));
            };
            if state.requires_confirm {
                Activation::Confirm(ConfirmableCommand::RouteSwitch {
                    name: name.clone(),
                    route: next_route,
                })
            } else {
                Activation::Immediate(ControlCommand::RouteSwitch {
                    name: name.clone(),
                    route: next_route,
                })
            }
        }
        (ControlItem::Gpio(name), ControlIntent::Primary) => {
            let level = *model.gpio_levels.get(name).unwrap_or(&false);
            let is_input = *model.gpio_is_input.get(name).unwrap_or(&true);
            let next = if is_input { true } else { !level };
            Activation::Immediate(ControlCommand::SetGpioOutput {
                name: name.clone(),
                value: next,
            })
        }
        (ControlItem::Gpio(name), ControlIntent::RestoreInput) => {
            Activation::Immediate(ControlCommand::SetGpioInput { name: name.clone() })
        }
        (_, ControlIntent::RestoreInput) => Activation::Ignored,
    }
}

pub(super) fn activate_item(
    model: &mut TuiModel,
    item: ControlItem,
    intent: ControlIntent,
) -> Result<()> {
    match resolve_activation(model, &item, intent) {
        Activation::Immediate(command) => execute_immediate(model, command),
        Activation::Confirm(command) => {
            model.status = format!("Confirm {} within 3s", command.target_text());
            model.hardware_confirm = Some(HardwareConfirmation::new(command));
            Ok(())
        }
        Activation::Rejected(message) => {
            model.status = message;
            Ok(())
        }
        Activation::Ignored => Ok(()),
    }
}

pub(super) fn confirm_hardware(model: &mut TuiModel) -> Result<()> {
    let Some(confirm) = model.hardware_confirm.take() else {
        return Ok(());
    };
    match confirm.command {
        ConfirmableCommand::SetPower { output, next_state } => {
            toggle_power(model, &output, next_state)
        }
        ConfirmableCommand::RouteSwitch { name, route } => route_switch(model, &name, &route),
    }
}

pub(super) fn cancel_hardware(model: &mut TuiModel) {
    if let Some(confirm) = model.hardware_confirm.take() {
        model.status = confirm.command.cancel_message();
    }
}

fn execute_immediate(model: &mut TuiModel, command: ControlCommand) -> Result<()> {
    match command {
        ControlCommand::SetGpioOutput { name, value } => {
            let action = set_gpio_output(&model.base_url, model.timeout, &name, value)?;
            model.gpio_levels.insert(name.clone(), value);
            model.gpio_is_input.insert(name, false);
            model.apply_action_msg(action);
        }
        ControlCommand::SetGpioInput { name } => {
            let action = set_gpio_input(&model.base_url, model.timeout, &name)?;
            model.gpio_is_input.insert(name, true);
            model.apply_action_msg(action);
        }
        ControlCommand::RouteSwitch { name, route } => route_switch(model, &name, &route)?,
    }
    Ok(())
}

fn toggle_power(model: &mut TuiModel, output: &str, next_state: bool) -> Result<()> {
    let action = perform_control_action(&model.base_url, model.timeout, output, !next_state)?;
    model.power_states.insert(output.to_string(), next_state);
    model.apply_action_msg(action);
    Ok(())
}

fn route_switch(model: &mut TuiModel, name: &str, route: &str) -> Result<()> {
    let action = set_switch_route(&model.base_url, model.timeout, name, route)?;
    if let Some(state) = model.switches.get_mut(name) {
        state.desired_route = route.to_string();
        state.pending_route = Some(route.to_string());
        state.pending_until = Some(Instant::now() + Duration::from_secs(2));
        state.route_intent_active = true;
    }
    model.apply_action_msg(action);
    Ok(())
}
