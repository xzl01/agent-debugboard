use super::model::TuiModel;
use super::text_width::{clip_display, display_width, sanitize_display};
use ratatui::layout::Rect;
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::Paragraph;

const APP_TITLE: &str = "Radxa Linkr Debugger TUI";

fn title_style() -> Style {
    Style::default().add_modifier(Modifier::BOLD)
}

fn error_style() -> Style {
    Style::default().fg(Color::Red)
}

fn right_state(model: &TuiModel) -> String {
    let mut parts = String::new();
    if model.status.ends_with("mode") {
        parts.push_str(&model.status);
    }
    if model.paused {
        if !parts.is_empty() {
            parts.push(' ');
        }
        parts.push_str("paused");
    }
    parts
}

fn status_row_one(model: &TuiModel, width: usize) -> Line<'static> {
    let right = right_state(model);
    let url = sanitize_display(&format!(" url={}", model.base_url));
    let title_width = display_width(APP_TITLE);
    let url_width = display_width(&url);
    let right_width = display_width(&right);

    let mut left = format!("{APP_TITLE}{url}");
    let mut right_text = sanitize_display(&right);
    if title_width + url_width + 1 + right_width > width {
        right_text = String::new();
    }
    if right_text.is_empty() && title_width + url_width > width {
        left = APP_TITLE.to_string();
    }
    let left_width = display_width(&left);
    let right_width = display_width(&right_text);

    if !right_text.is_empty() && left_width + 1 + right_width <= width {
        let gap = width - left_width - right_width;
        return Line::from(vec![
            Span::styled(left, title_style()),
            Span::raw(" ".repeat(gap)),
            Span::raw(right_text),
        ]);
    }
    let clipped = clip_display(&left, width);
    let clipped_width = display_width(&clipped);
    Line::from(vec![
        Span::styled(clipped, title_style()),
        Span::raw(" ".repeat(width.saturating_sub(clipped_width))),
    ])
}

fn status_row_two(model: &TuiModel, width: usize) -> Line<'static> {
    let (text, style) = match &model.err {
        Some(err) => (err.clone(), error_style()),
        None => (model.status.clone(), Style::default()),
    };
    Line::from(Span::styled(clip_display(&text, width), style))
}

pub(super) fn render_status_band(frame: &mut ratatui::Frame, area: Rect, model: &TuiModel) {
    let width = area.width as usize;
    let lines = [status_row_one(model, width), status_row_two(model, width)];
    let rows: Vec<Line<'static>> = lines.into_iter().take(area.height as usize).collect();
    frame.render_widget(Paragraph::new(rows), area);
}
