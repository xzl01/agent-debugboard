use super::board_io::{perform_control_action, set_switch_route};
use super::confirm::{ConfirmableCommand, HardwareConfirmation};
use super::controls::{next_switch_route, ControlItem};
use super::gpio_io::{GpioAction, GpioJob};
use super::model::TuiModel;
use anyhow::Result;
use std::time::{Duration, Instant};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum ControlIntent {
    Primary,
    DriveLow,
    DriveHigh,
    SetInput,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) enum ControlCommand {
    SetGpioOutput { name: String, value: bool },
    SetGpioInput { name: String },
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
            Activation::Confirm(ConfirmableCommand::RouteSwitch {
                name: name.clone(),
                route: next_route,
            })
        }
        (ControlItem::Gpio(_), ControlIntent::Primary) => Activation::Ignored,
        (ControlItem::Gpio(name), ControlIntent::DriveLow) => {
            Activation::Immediate(ControlCommand::SetGpioOutput {
                name: name.clone(),
                value: false,
            })
        }
        (ControlItem::Gpio(name), ControlIntent::DriveHigh) => {
            Activation::Immediate(ControlCommand::SetGpioOutput {
                name: name.clone(),
                value: true,
            })
        }
        (ControlItem::Gpio(name), ControlIntent::SetInput) => {
            Activation::Immediate(ControlCommand::SetGpioInput { name: name.clone() })
        }
        (_, ControlIntent::DriveLow | ControlIntent::DriveHigh | ControlIntent::SetInput) => {
            Activation::Ignored
        }
    }
}

pub(super) fn activate_item(
    model: &mut TuiModel,
    item: ControlItem,
    intent: ControlIntent,
) -> Result<()> {
    if model.gpio_pending.is_some()
        && matches!(
            (&item, intent),
            (
                ControlItem::Power(_) | ControlItem::Switch(_),
                ControlIntent::Primary
            )
        )
    {
        return Ok(());
    }
    match resolve_activation(model, &item, intent) {
        Activation::Immediate(command) => execute_immediate(model, command),
        Activation::Confirm(command) => {
            model.gpio_gesture.cancel();
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

pub(super) fn confirm_hardware(model: &mut TuiModel, now: Instant) -> Result<()> {
    if expire_hardware_confirmation(model, now) {
        return Ok(());
    }
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

pub(super) fn expire_hardware_confirmation(model: &mut TuiModel, now: Instant) -> bool {
    match model.hardware_confirm.take() {
        Some(confirm) if confirm.expired_at(now) => {
            model.status = confirm.command.timeout_message();
            model.last_http_poll = Some(now);
            true
        }
        Some(confirm) => {
            model.hardware_confirm = Some(confirm);
            false
        }
        None => false,
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
            let action = if value {
                GpioAction::DriveHigh
            } else {
                GpioAction::DriveLow
            };
            start_gpio_job(
                model,
                GpioJob {
                    action,
                    target: name,
                },
            );
        }
        ControlCommand::SetGpioInput { name } => {
            start_gpio_job(
                model,
                GpioJob {
                    action: GpioAction::SetInput,
                    target: name,
                },
            );
        }
    }
    Ok(())
}

fn start_gpio_job(model: &mut TuiModel, job: GpioJob) {
    model.gpio_gesture.cancel();
    if let Some(pending) = &model.gpio_pending {
        model.status = format!(
            "{} dropped: {} in flight",
            job.action.status_text(&job.target),
            pending.action.status_text(&pending.target),
        );
        return;
    }
    if model
        .gpio_worker
        .start(model.base_url.clone(), model.timeout, job.clone())
    {
        model.status = format!("{}…", job.action.status_text(&job.target));
        model.gpio_pending = Some(job);
    } else {
        model.status = format!(
            "{} dropped: worker busy",
            job.action.status_text(&job.target)
        );
    }
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
