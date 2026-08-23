use super::model::{TuiModel, TuiSwitchState};

pub(super) fn control_targets(model: &TuiModel) -> &[String] {
    &model.power_names
}

pub(super) fn next_switch_route(state: &TuiSwitchState) -> Option<String> {
    if state.routes.is_empty() {
        return None;
    }
    if let Some(index) = state
        .routes
        .iter()
        .position(|route| route == &state.desired_route)
    {
        return state.routes.get((index + 1) % state.routes.len()).cloned();
    }
    state.routes.first().cloned()
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) enum ControlItem {
    Power(String),
    Switch(String),
    Gpio(String),
}

pub(super) fn control_items(model: &TuiModel) -> Vec<ControlItem> {
    let mut items = Vec::new();
    for output in control_targets(model) {
        items.push(ControlItem::Power(output.clone()));
    }
    for name in model.switches.keys() {
        items.push(ControlItem::Switch(name.clone()));
    }
    for gpio in &model.gpio_names {
        items.push(ControlItem::Gpio(gpio.clone()));
    }
    items
}

pub(super) fn first_gpio_index(model: &TuiModel) -> Option<usize> {
    let lines = super::gpio_projection::projected_lines(model);
    lines
        .get(super::gpio_projection::gpio_line_start(model))
        .and_then(|line| line.first().copied())
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum Nav {
    Up,
    Down,
    Left,
    Right,
    PageUp,
    PageDown,
}

pub(super) fn navigate(model: &TuiModel, nav: Nav) -> usize {
    let lines = super::gpio_projection::projected_lines(model);
    if lines.is_empty() {
        return 0;
    }
    let last_index = lines
        .last()
        .and_then(|line| line.last().copied())
        .unwrap_or(0);
    let current = model.control_idx.min(last_index);
    let (line, side) = locate(&lines, current);
    match nav {
        Nav::Up => step_line(model, &lines, line, side, -1),
        Nav::Down => step_line(model, &lines, line, side, 1),
        Nav::PageUp => step_line(model, &lines, line, side, -3),
        Nav::PageDown => step_line(model, &lines, line, side, 3),
        Nav::Left | Nav::Right => {
            if lines[line].len() == 2 {
                lines[line][1 - side]
            } else {
                current
            }
        }
    }
}

fn locate(lines: &[Vec<usize>], current: usize) -> (usize, usize) {
    for (line_index, line) in lines.iter().enumerate() {
        for (side, item) in line.iter().enumerate() {
            if *item == current {
                return (line_index, side);
            }
        }
    }
    (lines.len() - 1, lines[lines.len() - 1].len() - 1)
}

fn step_line(
    model: &TuiModel,
    lines: &[Vec<usize>],
    line: usize,
    side: usize,
    delta: isize,
) -> usize {
    let target = (line as isize + delta).clamp(0, lines.len() as isize - 1) as usize;
    let gpio_start = super::gpio_projection::gpio_line_start(model);
    let keep_side = side == 1 && line >= gpio_start && target >= gpio_start;
    let target_side = if keep_side && lines[target].len() == 2 {
        1
    } else {
        0
    };
    lines[target][target_side]
}
