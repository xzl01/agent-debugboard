use super::controls::{control_items, ControlItem};
use super::model::{current_milliamp_estimate, TuiModel};
use crate::adc::AdcReading;
use ratatui::style::{Color, Modifier, Style};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum RowTone {
    PowerOn,
    PowerOff,
    SwitchReady,
    SwitchPending,
    SwitchMismatch,
    GpioHigh,
    GpioLow,
}

impl RowTone {
    pub(super) fn state_style(self) -> Style {
        match self {
            Self::PowerOn => Style::default().fg(Color::Green),
            Self::PowerOff => Style::default().fg(Color::DarkGray),
            Self::SwitchReady => Style::default().fg(Color::Cyan),
            Self::SwitchPending => Style::default().fg(Color::Yellow),
            Self::SwitchMismatch => Style::default().fg(Color::Red),
            Self::GpioHigh => Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),
            Self::GpioLow => Style::default().fg(Color::DarkGray),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct ControlRow {
    pub(super) item: ControlItem,
    pub(super) kind: &'static str,
    pub(super) name: String,
    pub(super) state_route: String,
    pub(super) live: String,
    pub(super) mode: String,
    pub(super) description: String,
    pub(super) tone: RowTone,
}

pub(super) fn control_rows(model: &TuiModel) -> Vec<ControlRow> {
    control_items(model)
        .iter()
        .map(|item| build_row(model, item))
        .collect()
}

fn build_row(model: &TuiModel, item: &ControlItem) -> ControlRow {
    match item {
        ControlItem::Power(output) => power_row(model, item, output),
        ControlItem::Switch(name) => switch_row(model, item, name),
        ControlItem::Gpio(gpio) => gpio_row(model, item, gpio),
    }
}

fn power_row(model: &TuiModel, item: &ControlItem, output: &str) -> ControlRow {
    let enabled = *model.power_states.get(output).unwrap_or(&false);
    let live = model
        .latest
        .get(output)
        .map_or_else(|| "-".to_string(), live_current_text);
    let channel = model.channel_ids.iter().find(|id| id.as_str() == output);
    ControlRow {
        item: item.clone(),
        kind: "power",
        name: output.to_string(),
        state_route: if enabled { "on" } else { "off" }.to_string(),
        live,
        mode: "-".to_string(),
        description: channel.cloned().unwrap_or_else(|| "-".to_string()),
        tone: if enabled {
            RowTone::PowerOn
        } else {
            RowTone::PowerOff
        },
    }
}

fn live_current_text(reading: &AdcReading) -> String {
    if let Some(current_ua) = reading.current_ua {
        let prefix = if current_ua < 0 { "-" } else { "" };
        let abs = (current_ua as i64).abs();
        return format!("{prefix}{:.6}A", abs as f64 / 1_000_000.0);
    }
    format!(
        "{:>5.2}A",
        current_milliamp_estimate(reading) as f64 / 1000.0
    )
}

fn switch_row(model: &TuiModel, item: &ControlItem, name: &str) -> ControlRow {
    let state = model.switches.get(name);
    let pending = state.is_some_and(|state| state.pending_route.is_some());
    let mismatch = state.is_some_and(|state| state.desired_route != state.actual_route);
    let mut state_route = state
        .map_or("", |state| state.desired_route.as_str())
        .to_string();
    if mismatch {
        state_route = format!(
            "{}(->{})",
            state.map_or("", |state| state.desired_route.as_str()),
            state.map_or("", |state| state.actual_route.as_str())
        );
    }
    if pending {
        state_route.push_str(" (pending)");
    }
    ControlRow {
        item: item.clone(),
        kind: "switch",
        name: name.to_string(),
        state_route,
        live: "-".to_string(),
        mode: if state.is_some_and(|state| state.requires_confirm) {
            "confirm"
        } else {
            "auto"
        }
        .to_string(),
        description: state.map_or_else(String::new, |state| state.routes.join("/")),
        tone: if pending {
            RowTone::SwitchPending
        } else if mismatch {
            RowTone::SwitchMismatch
        } else {
            RowTone::SwitchReady
        },
    }
}

fn gpio_row(model: &TuiModel, item: &ControlItem, gpio: &str) -> ControlRow {
    let level = *model.gpio_levels.get(gpio).unwrap_or(&false);
    let is_input = *model.gpio_is_input.get(gpio).unwrap_or(&true);
    ControlRow {
        item: item.clone(),
        kind: "gpio",
        name: gpio.to_string(),
        state_route: if level { "1" } else { "0" }.to_string(),
        live: "-".to_string(),
        mode: if is_input { "in" } else { "out" }.to_string(),
        description: model.gpio_notes.get(gpio).cloned().unwrap_or_default(),
        tone: if level {
            RowTone::GpioHigh
        } else {
            RowTone::GpioLow
        },
    }
}
