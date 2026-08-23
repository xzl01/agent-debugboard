use super::model::TuiModel;
use ratatui::layout::{Alignment, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Clear, Paragraph};

const MODAL_MAX_WIDTH: u16 = 64;
const MODAL_HEIGHT: u16 = 7;
const CONFIRM_LABEL: &str = "[ Confirm ]";
const CANCEL_LABEL: &str = "[ Cancel ]";
const BUTTON_GAP: usize = 2;

pub(super) fn modal_area(frame_area: Rect) -> Option<Rect> {
    if frame_area.width < 4 || frame_area.height < 4 {
        return None;
    }
    let width = frame_area.width.saturating_sub(2).clamp(1, MODAL_MAX_WIDTH);
    let height = MODAL_HEIGHT.clamp(1, frame_area.height.saturating_sub(2));
    Some(Rect::new(
        frame_area.x + (frame_area.width - width) / 2,
        frame_area.y + (frame_area.height - height) / 2,
        width,
        height,
    ))
}

pub(super) fn render_hardware_confirmation(frame: &mut ratatui::Frame, model: &mut TuiModel) {
    let Some(confirm) = &model.hardware_confirm else {
        return;
    };
    let Some(area) = modal_area(frame.area()) else {
        return;
    };
    frame.render_widget(Clear, area);

    let emphasis = Style::default()
        .fg(Color::Yellow)
        .add_modifier(Modifier::BOLD);
    let block = Block::default()
        .title(Span::styled(
            format!(" {} ", confirm.command.title()),
            emphasis,
        ))
        .borders(Borders::ALL)
        .border_style(Style::default().fg(Color::Red));
    let inner = block.inner(area);

    let buttons = format!("{CONFIRM_LABEL}{}{CANCEL_LABEL}", " ".repeat(BUTTON_GAP));
    let lines = vec![
        Line::from(Span::styled(
            "Hardware action requires confirmation",
            emphasis,
        )),
        Line::from(confirm.command.target_text()),
        Line::from(""),
        Line::from(buttons.clone()),
    ];
    let paragraph = Paragraph::new(lines)
        .block(block)
        .alignment(Alignment::Center);
    frame.render_widget(paragraph, area);

    register_button_hits(model, inner, buttons.len());
}

fn register_button_hits(model: &mut TuiModel, inner: Rect, buttons_len: usize) {
    let buttons_line = 3usize;
    if inner.height as usize <= buttons_line || (inner.width as usize) < buttons_len {
        return;
    }
    let pad = ((inner.width as usize) - buttons_len) / 2;
    let y = inner.y + buttons_line as u16;
    let confirm_x = inner.x + pad as u16;
    model.hit_map.confirm_button = Some(Rect::new(confirm_x, y, CONFIRM_LABEL.len() as u16, 1));
    model.hit_map.cancel_button = Some(Rect::new(
        confirm_x + (CONFIRM_LABEL.len() + BUTTON_GAP) as u16,
        y,
        CANCEL_LABEL.len() as u16,
        1,
    ));
}
