use super::config_columns::{clip, column_plan, ColumnKind};
use super::config_state::SavedConfigState;
use super::text_width::clip_display;
use crate::persistent_config::{
    ConfigApplyState, ConfigItemKind, ConfigValue, GpioDirection, GpioLevel, PowerState,
};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};

const COLUMN_GAP: usize = 2;

fn badge_style() -> Style {
    Style::default().add_modifier(Modifier::BOLD)
}

fn cursor_style() -> Style {
    Style::default()
        .fg(Color::Black)
        .bg(Color::White)
        .add_modifier(Modifier::BOLD)
}

fn error_style() -> Style {
    Style::default().fg(Color::Red)
}

pub(super) struct SavedConfigContent {
    pub(super) lines: Vec<Line<'static>>,
    pub(super) item_anchors: Vec<usize>,
}

pub(super) fn build_saved_config_content(
    state: &SavedConfigState,
    width: usize,
) -> SavedConfigContent {
    let mut content = SavedConfigContent {
        lines: Vec::new(),
        item_anchors: Vec::new(),
    };
    if !state.is_supported() {
        content.lines.push(Line::from(vec![
            Span::styled("[unsupported]", badge_style()),
            Span::raw(" firmware config summary unavailable"),
        ]));
        return content;
    }
    let mut badges = Vec::new();
    if !state.loaded {
        if state.error.is_none() {
            badges.push("[loading]".to_string());
        }
    } else if !state.backend_available {
        badges.push("[unavailable]".to_string());
    } else if !state.items.is_empty() {
        // The ready badge is implicit once rows are listed.
    } else {
        badges.push("[ready]".to_string());
    }
    if state.pending != 0 {
        badges.push(format!("[pending:{}]", state.pending));
    }
    if let Some(busy) = state.busy {
        badges.push(format!("[busy:{}]", busy.as_str()));
    }
    if state.error.is_some() {
        badges.push("[error]".to_string());
    }
    if !badges.is_empty() {
        content
            .lines
            .push(Line::from(Span::styled(badges.join(" "), badge_style())));
    }
    if !state.loaded {
        append_error(&mut content.lines, state, width);
        return content;
    }
    if !state.backend_available {
        content.lines.push(Line::from(clip_display(
            &format!("reason={}", state.backend_reason),
            width,
        )));
    } else if state.items.is_empty() {
        content.lines.push(Line::from("(none)"));
    } else {
        append_items(&mut content, state, width);
    }
    append_error(&mut content.lines, state, width);
    content
}

fn append_error(lines: &mut Vec<Line<'static>>, state: &SavedConfigState, width: usize) {
    if let Some(error) = &state.error {
        lines.push(Line::from(Span::styled(
            clip_display(&format!("error={error} [Esc]"), width),
            error_style(),
        )));
    }
}

fn append_items(content: &mut SavedConfigContent, state: &SavedConfigState, width: usize) {
    let plan = column_plan(width);
    for (index, item) in state.items.iter().enumerate() {
        content.item_anchors.push(content.lines.len());
        let focused = state.focused && index == state.cursor;
        let style = if focused {
            cursor_style()
        } else {
            Style::default()
        };
        let mut spans = Vec::new();
        let mut used = 0usize;
        for (column_index, column) in plan.iter().enumerate() {
            if column_index > 0 {
                spans.push(Span::styled("  ", style));
                used += COLUMN_GAP;
            }
            let text = match column.kind {
                ColumnKind::Sel => {
                    if state.is_selected(&item.id) {
                        "[x]"
                    } else {
                        "[ ]"
                    }
                }
                .to_string(),
                ColumnKind::Id => item.id.as_str().to_string(),
                ColumnKind::Kind => kind_text(&item.kind).to_string(),
                ColumnKind::Current => value_text(item.current.as_ref()),
                ColumnKind::Saved => value_text(item.saved.as_ref()),
                ColumnKind::Risk => risk_text(item.requires_confirm).to_string(),
                ColumnKind::Apply => apply_text(item.apply_state.as_ref()).to_string(),
            };
            used += column.width;
            spans.push(Span::styled(clip(&text, column.width), style));
        }
        if focused && used < width {
            spans.push(Span::styled(" ".repeat(width - used), style));
        }
        content.lines.push(Line::from(spans));
    }
}

fn kind_text(kind: &ConfigItemKind) -> &str {
    match kind {
        ConfigItemKind::Power => "power",
        ConfigItemKind::Switch => "switch",
        ConfigItemKind::Gpio => "gpio",
        ConfigItemKind::Unknown(value) => value,
    }
}

fn value_text(value: Option<&ConfigValue>) -> String {
    match value {
        Some(ConfigValue::Power(value)) => match value.state {
            PowerState::On => "on".to_string(),
            PowerState::Off => "off".to_string(),
        },
        Some(ConfigValue::Switch(value)) => value.route.clone(),
        Some(ConfigValue::Gpio(value)) => {
            let direction = match value.direction {
                GpioDirection::Input => "in",
                GpioDirection::Output => "out",
            };
            let level = match value.value {
                GpioLevel::Low => "0",
                GpioLevel::High => "1",
            };
            format!("{direction}:{level}")
        }
        Some(ConfigValue::Unknown(_)) => "unknown".to_string(),
        None => "none".to_string(),
    }
}

fn risk_text(requires_confirm: Option<bool>) -> &'static str {
    match requires_confirm {
        Some(true) => "danger",
        Some(false) => "safe",
        None => "unknown",
    }
}

fn apply_text(state: Option<&ConfigApplyState>) -> &str {
    match state {
        Some(ConfigApplyState::NotSaved) => "not_saved",
        Some(ConfigApplyState::Applied) => "applied",
        Some(ConfigApplyState::Pending) => "pending",
        Some(ConfigApplyState::Failed) => "failed",
        Some(ConfigApplyState::Unknown(value)) => value,
        None => "unknown",
    }
}
