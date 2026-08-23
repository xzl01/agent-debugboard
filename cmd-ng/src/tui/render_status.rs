use super::model::TuiModel;
use ratatui::layout::Rect;
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::Paragraph;

const APP_TITLE: &str = "Radxa Linkr Debugger TUI";

fn clip(text: &str, width: usize) -> String {
    text.chars().take(width).collect()
}

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
    let url = format!(" url={}", model.base_url);
    let title_len = APP_TITLE.chars().count();
    let url_len = url.chars().count();
    let right_len = right.chars().count();

    let mut left = format!("{APP_TITLE}{url}");
    let mut right_text = right.clone();
    if title_len + url_len + 1 + right_len > width {
        right_text = String::new();
    }
    if right_text.is_empty() && title_len + url_len > width {
        left = APP_TITLE.to_string();
    }
    let left_len = left.chars().count();
    let right_len = right_text.chars().count();

    if !right_text.is_empty() && left_len + 1 + right_len <= width {
        let gap = width - left_len - right_len;
        return Line::from(vec![
            Span::styled(left, title_style()),
            Span::raw(" ".repeat(gap)),
            Span::raw(right_text),
        ]);
    }
    Line::from(vec![
        Span::styled(clip(&left, width), title_style()),
        Span::raw(" ".repeat(width.saturating_sub(left_len.min(width)))),
    ])
}

fn status_row_two(model: &TuiModel, width: usize) -> Line<'static> {
    let (text, style) = match &model.err {
        Some(err) => (err.clone(), error_style()),
        None => (model.status.clone(), Style::default()),
    };
    Line::from(Span::styled(clip(&text, width), style))
}

pub(super) fn render_status_band(frame: &mut ratatui::Frame, area: Rect, model: &TuiModel) {
    let width = area.width as usize;
    let lines = [status_row_one(model, width), status_row_two(model, width)];
    let rows: Vec<Line<'static>> = lines.into_iter().take(area.height as usize).collect();
    frame.render_widget(Paragraph::new(rows), area);
}
