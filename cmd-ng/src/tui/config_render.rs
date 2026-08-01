use super::config_state::{ConfigConfirmation, SavedConfigState};
use crate::persistent_config::{
    ConfigApplyState, ConfigItemKind, ConfigValue, GpioDirection, GpioLevel, PowerState,
};
use ratatui::layout::Rect;
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Clear, Paragraph, Wrap};

pub(super) const SAVED_CONFIG_TITLE: &str = "Saved Config";
const COMPACT_WIDTH: usize = 72;

pub(super) fn append_saved_config_lines(
    lines: &mut Vec<Line<'static>>,
    state: &SavedConfigState,
    width: usize,
) {
    let mut heading = vec![Span::styled(SAVED_CONFIG_TITLE, heading_style())];
    if !state.is_supported() {
        heading.push(badge(" [unsupported]"));
        lines.push(Line::from(heading));
        lines.push(Line::from("  firmware config summary unavailable"));
        lines.push(Line::from(""));
        return;
    }
    if !state.loaded {
        if state.error.is_none() {
            heading.push(badge(" [loading]"));
        }
    } else if !state.backend_available {
        heading.push(badge(" [unavailable]"));
    } else {
        heading.push(badge(" [ready]"));
    }
    if state.pending != 0 {
        heading.push(badge(format!(" [pending:{}]", state.pending)));
    }
    if let Some(busy) = state.busy {
        heading.push(badge(format!(" [busy:{}]", busy.as_str())));
    }
    if state.error.is_some() {
        heading.push(badge(" [error]"));
    }
    lines.push(Line::from(heading));
    lines.push(Line::from(
        "  c focus · ↑/↓ item · Space select · s save · a apply · x clear · Esc dismiss",
    ));
    if !state.loaded {
        append_error(lines, state);
        lines.push(Line::from(""));
        return;
    }
    if !state.backend_available {
        lines.push(Line::from(format!("  reason={}", state.backend_reason)));
    } else if state.items.is_empty() {
        lines.push(Line::from("  (none)"));
    } else {
        append_items(lines, state, width);
    }
    append_error(lines, state);
    lines.push(Line::from(""));
}

fn append_error(lines: &mut Vec<Line<'static>>, state: &SavedConfigState) {
    if let Some(error) = &state.error {
        lines.push(Line::from(Span::styled(
            format!("  error={error} [Esc]"),
            badge_style(),
        )));
    }
}

fn append_items(lines: &mut Vec<Line<'static>>, state: &SavedConfigState, width: usize) {
    for (index, item) in state.items.iter().enumerate() {
        let marker = if state.is_selected(&item.id) {
            "[x]"
        } else {
            "[ ]"
        };
        let current = value_text(item.current.as_ref());
        let saved = value_text(item.saved.as_ref());
        let risk = risk_text(item.requires_confirm);
        let apply = apply_text(item.apply_state.as_ref());
        let style = if state.focused && index == state.cursor {
            selected_style()
        } else {
            Style::default()
        };
        if width < COMPACT_WIDTH {
            lines.push(Line::from(Span::styled(
                format!(
                    "  {marker} {} kind={}",
                    item.id.as_str(),
                    kind_text(&item.kind)
                ),
                style,
            )));
            lines.push(Line::from(format!("      current={current} saved={saved}")));
            lines.push(Line::from(format!("      risk={risk} apply={apply}")));
        } else {
            lines.push(Line::from(Span::styled(
                format!(
                    "  {marker} {} kind={} current={current} saved={saved} risk={risk} apply={apply}",
                    item.id.as_str(),
                    kind_text(&item.kind)
                ),
                style,
            )));
        }
    }
}

pub(super) fn confirmation_text(state: &SavedConfigState) -> Option<String> {
    let confirmation = state.confirmation()?;
    let (action, dangerous) = match confirmation {
        ConfigConfirmation::Save { dangerous, .. } => ("SAVE", dangerous),
        ConfigConfirmation::Apply { dangerous } => ("APPLY", dangerous),
    };
    Some(format!(
        "{action} dangerous={}",
        dangerous
            .iter()
            .map(|id| id.as_str())
            .collect::<Vec<_>>()
            .join(",")
    ))
}

pub(super) fn render_confirmation(frame: &mut ratatui::Frame, state: &SavedConfigState) {
    let Some(text) = confirmation_text(state) else {
        return;
    };
    let frame_area = frame.area();
    if frame_area.width == 0 || frame_area.height == 0 {
        return;
    }
    let width = frame_area.width.saturating_sub(2).clamp(1, 72);
    let height = frame_area.height.saturating_sub(2).clamp(1, 7);
    let area = Rect::new(
        frame_area.x + (frame_area.width - width) / 2,
        frame_area.y + (frame_area.height - height) / 2,
        width,
        height,
    );
    frame.render_widget(Clear, area);
    frame.render_widget(
        Paragraph::new(vec![
            Line::from(Span::styled(text, badge_style())),
            Line::from(""),
            Line::from("Enter confirm · Esc cancel"),
        ])
        .block(
            Block::default()
                .title("Saved Config Confirmation")
                .borders(Borders::ALL),
        )
        .wrap(Wrap { trim: false }),
        area,
    );
}

fn heading_style() -> Style {
    Style::default().add_modifier(Modifier::BOLD | Modifier::UNDERLINED)
}

fn badge_style() -> Style {
    Style::default().add_modifier(Modifier::BOLD)
}

fn selected_style() -> Style {
    Style::default()
        .fg(Color::Black)
        .bg(Color::White)
        .add_modifier(Modifier::BOLD)
}

fn badge(text: impl Into<String>) -> Span<'static> {
    Span::styled(text.into(), badge_style())
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
        Some(true) => "confirm",
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
