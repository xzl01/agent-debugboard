use super::model::{TuiModel, TuiSwitchState};
use super::text_width::{clip_display, display_width};
use crate::monitoring::format_monitoring_summary;
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};

const NAME_WIDTH: usize = 10;
const DESIRED_WIDTH: usize = 12;
const ACTUAL_WIDTH: usize = 12;
const STATE_WIDTH: usize = 9;
const COLUMN_GAP: usize = 2;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum SwitchState {
    Ready,
    Pending,
    Mismatch,
}

impl SwitchState {
    fn derive(state: &TuiSwitchState) -> Self {
        if state.pending_route.is_some() {
            Self::Pending
        } else if state.desired_route != state.actual_route {
            Self::Mismatch
        } else {
            Self::Ready
        }
    }

    const fn text(self) -> &'static str {
        match self {
            Self::Ready => "ready",
            Self::Pending => "pending",
            Self::Mismatch => "mismatch",
        }
    }

    fn style(self) -> Style {
        match self {
            Self::Ready => Style::default().fg(Color::Cyan),
            Self::Pending => Style::default().fg(Color::Yellow),
            Self::Mismatch => Style::default().fg(Color::Red),
        }
    }
}

fn header_style() -> Style {
    Style::default()
        .fg(Color::DarkGray)
        .add_modifier(Modifier::BOLD)
}

pub(super) fn status_header_line(width: usize) -> Line<'static> {
    let plan = [
        ("SWITCH", NAME_WIDTH),
        ("DESIRED", DESIRED_WIDTH),
        ("ACTUAL", ACTUAL_WIDTH),
        ("STATE", STATE_WIDTH),
    ];
    let mut spans = Vec::new();
    let mut used = 0usize;
    for (index, (title, column_width)) in plan.iter().enumerate() {
        if index > 0 {
            spans.push(Span::styled("  ", header_style()));
            used += COLUMN_GAP;
        }
        let available = (*column_width).min(width.saturating_sub(used));
        let clipped = clip_display(title, available);
        let padding = available.saturating_sub(display_width(&clipped));
        spans.push(Span::styled(
            format!("{clipped}{:padding$}", "", padding = padding),
            header_style(),
        ));
        used += available;
    }
    Line::from(spans)
}

pub(super) fn status_lines(model: &TuiModel, width: usize) -> Vec<Line<'static>> {
    let mut lines = Vec::new();
    for state in model.switches.values() {
        lines.push(switch_line(state, width));
    }
    for pair in monitoring_pairs(model) {
        lines.push(Line::from(clip_display(&pair, width)));
    }
    if let Some(err) = &model.err {
        lines.push(Line::from(Span::styled(
            clip_display(&format!("error: {err}"), width),
            Style::default().fg(Color::Red),
        )));
    }
    lines
}

fn switch_line(state: &TuiSwitchState, width: usize) -> Line<'static> {
    let derived = SwitchState::derive(state);
    let columns = [
        (state.name.as_str(), NAME_WIDTH),
        (state.desired_route.as_str(), DESIRED_WIDTH),
        (state.actual_route.as_str(), ACTUAL_WIDTH),
    ];
    let mut spans = Vec::new();
    let mut used = 0usize;
    for (text, column_width) in columns {
        if used > 0 {
            spans.push(Span::raw("  "));
            used += COLUMN_GAP;
        }
        let available = column_width.min(width.saturating_sub(used));
        let clipped = clip_display(text, available);
        let clipped_width = display_width(&clipped);
        used += clipped_width;
        let padding = available.saturating_sub(clipped_width);
        spans.push(Span::raw(format!(
            "{clipped}{:padding$}",
            "",
            padding = padding
        )));
        used += padding;
    }
    if used + COLUMN_GAP < width {
        spans.push(Span::raw("  "));
        spans.push(Span::styled(
            clip_display(derived.text(), width - used - COLUMN_GAP),
            derived.style(),
        ));
    }
    Line::from(spans)
}

fn monitoring_pairs(model: &TuiModel) -> Vec<String> {
    let summary = format_monitoring_summary(&model.monitoring);
    let summary = summary.strip_prefix("board: ").unwrap_or(&summary);
    summary
        .split(" · ")
        .map(|field| match field.split_once(' ') {
            Some((key, value)) => format!("{key}: {value}"),
            None => field.to_string(),
        })
        .collect()
}
