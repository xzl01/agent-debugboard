use super::config_state::{ConfigConfirmation, SavedConfigState};
use crate::persistent_config::ConfigItemId;
use ratatui::layout::Rect;
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Clear, Paragraph, Wrap};

pub(super) fn confirmation_text(state: &SavedConfigState) -> Option<String> {
    let confirmation = state.confirmation()?;
    let join_ids = |dangerous: &[ConfigItemId]| {
        dangerous
            .iter()
            .map(|id| id.as_str())
            .collect::<Vec<_>>()
            .join(",")
    };
    Some(match confirmation {
        ConfigConfirmation::Save { dangerous, .. } => format!(
            "SAVE dangerous={} · auto-restores on every normal boot until replaced or cleared",
            join_ids(dangerous)
        ),
    })
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
            Line::from(Span::styled(text, emphasis_style())),
            Line::from(""),
            Line::from("Enter confirm · Esc cancel"),
        ])
        .block(
            Block::default()
                .title(Span::styled("Saved Config Confirmation", emphasis_style()))
                .borders(Borders::ALL)
                .border_style(Style::default().fg(Color::Red)),
        )
        .wrap(Wrap { trim: false }),
        area,
    );
}

fn emphasis_style() -> Style {
    Style::default()
        .fg(Color::Yellow)
        .add_modifier(Modifier::BOLD)
}
