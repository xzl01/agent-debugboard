use super::config_state::{ConfigConfirmation, SavedConfigState};
use super::hit::HitRegions;
use super::hit_types::SavedConfigModalTarget;
use super::text_width::sanitize_display;
use crate::persistent_config::ConfigItemId;
use ratatui::layout::{Alignment, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Clear, Paragraph, Wrap};

const CONFIRM_LABEL: &str = "[ Confirm ]";
const CANCEL_LABEL: &str = "[ Cancel ]";
const BUTTON_GAP: usize = 2;

pub(super) fn confirmation_text(state: &SavedConfigState) -> Option<String> {
    let confirmation = state.confirmation()?;
    let join_ids = |dangerous: &[ConfigItemId]| {
        dangerous
            .iter()
            .map(|id| sanitize_display(id.as_str()))
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

pub(super) fn render_confirmation(
    frame: &mut ratatui::Frame,
    state: &SavedConfigState,
    hits: &mut HitRegions<SavedConfigModalTarget>,
) {
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
    let block = Block::default()
        .title(Span::styled("Saved Config Confirmation", emphasis_style()))
        .borders(Borders::ALL)
        .border_style(Style::default().fg(Color::Red));
    let inner = block.inner(area);
    frame.render_widget(block, area);
    frame.render_widget(
        Paragraph::new(Line::from(Span::styled(text, emphasis_style()))).wrap(Wrap { trim: false }),
        Rect::new(
            inner.x,
            inner.y,
            inner.width,
            inner.height.saturating_sub(1),
        ),
    );
    register_buttons(frame, inner, hits);
}

fn register_buttons(
    frame: &mut ratatui::Frame,
    inner: Rect,
    hits: &mut HitRegions<SavedConfigModalTarget>,
) {
    let buttons = format!("{CONFIRM_LABEL}{}{CANCEL_LABEL}", " ".repeat(BUTTON_GAP));
    if inner.height == 0 || (inner.width as usize) < buttons.len() {
        return;
    }
    let x = inner.x + (inner.width - buttons.len() as u16) / 2;
    let y = inner.y + inner.height - 1;
    frame.render_widget(
        Paragraph::new(buttons).alignment(Alignment::Center),
        Rect::new(inner.x, y, inner.width, 1),
    );
    hits.push(
        Rect::new(x, y, CONFIRM_LABEL.len() as u16, 1),
        SavedConfigModalTarget::confirm(),
    );
    hits.push(
        Rect::new(
            x + (CONFIRM_LABEL.len() + BUTTON_GAP) as u16,
            y,
            CANCEL_LABEL.len() as u16,
            1,
        ),
        SavedConfigModalTarget::cancel(),
    );
}

fn emphasis_style() -> Style {
    Style::default()
        .fg(Color::Yellow)
        .add_modifier(Modifier::BOLD)
}
