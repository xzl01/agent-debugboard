use super::text_width::sanitize_display;
use std::time::{Duration, Instant};

pub(super) const CONFIRM_TIMEOUT: Duration = Duration::from_secs(3);

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) enum ConfirmableCommand {
    SetPower { output: String, next_state: bool },
    RouteSwitch { name: String, route: String },
}

impl ConfirmableCommand {
    pub(super) fn title(&self) -> String {
        match self {
            Self::SetPower { .. } => "Confirm Power Toggle".to_string(),
            Self::RouteSwitch { .. } => "Confirm Switch Route".to_string(),
        }
    }

    pub(super) fn target_text(&self) -> String {
        match self {
            Self::SetPower { output, next_state } => {
                let current = if *next_state { "off" } else { "on" };
                let next = if *next_state { "on" } else { "off" };
                sanitize_display(&format!("power {output}: {current} -> {next}"))
            }
            Self::RouteSwitch { name, route } => {
                sanitize_display(&format!("switch {name} -> {route}"))
            }
        }
    }

    pub(super) fn cancel_message(&self) -> String {
        match self {
            Self::SetPower { .. } => "Power toggle cancelled".to_string(),
            Self::RouteSwitch { .. } => "Switch cancelled".to_string(),
        }
    }

    pub(super) fn timeout_message(&self) -> String {
        match self {
            Self::SetPower { .. } => "Power confirmation timed out".to_string(),
            Self::RouteSwitch { .. } => "Switch confirmation timed out".to_string(),
        }
    }
}

#[derive(Debug, Clone)]
pub(super) struct HardwareConfirmation {
    pub(super) command: ConfirmableCommand,
    pub(super) started: Instant,
}

impl HardwareConfirmation {
    pub(super) fn new(command: ConfirmableCommand) -> Self {
        Self {
            command,
            started: Instant::now(),
        }
    }

    pub(super) fn expired_at(&self, now: Instant) -> bool {
        now >= self.started + CONFIRM_TIMEOUT
    }

    pub(super) fn remains_advertised(
        &self,
        power_names: &[String],
        switches: &std::collections::BTreeMap<String, super::model::TuiSwitchState>,
    ) -> bool {
        match &self.command {
            ConfirmableCommand::SetPower { output, .. } => {
                power_names.iter().any(|name| name == output)
            }
            ConfirmableCommand::RouteSwitch { name, route } => switches
                .get(name)
                .is_some_and(|state| state.routes.iter().any(|advertised| advertised == route)),
        }
    }
}
