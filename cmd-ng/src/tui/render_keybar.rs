use super::controls::{control_items, ControlItem};
use super::model::TuiModel;
use super::pages::ActivePage;
use super::text_width::display_width;
use ratatui::layout::Rect;
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::Paragraph;

const SEGMENT_GAP: usize = 2;

struct Segment {
    key: &'static str,
    label: String,
}

impl Segment {
    fn new(key: &'static str, label: impl Into<String>) -> Self {
        Self {
            key,
            label: label.into(),
        }
    }

    fn unit_len(&self) -> usize {
        if self.key.is_empty() {
            display_width(&self.label)
        } else {
            display_width(self.key) + 1 + display_width(&self.label)
        }
    }
}

fn key_style() -> Style {
    Style::default()
        .fg(Color::Black)
        .bg(Color::Cyan)
        .add_modifier(Modifier::BOLD)
}

fn label_style() -> Style {
    Style::default().fg(Color::White).bg(Color::DarkGray)
}

fn gpio_selected(model: &TuiModel) -> bool {
    matches!(
        control_items(model).get(model.control_idx),
        Some(ControlItem::Gpio(_))
    )
}

fn keybar_segments(model: &TuiModel) -> Vec<Segment> {
    if let Some(confirm) = &model.hardware_confirm {
        return vec![
            Segment::new(
                "Enter/Space",
                format!("confirm {}", confirm.command.target_text()),
            ),
            Segment::new("Esc", "cancel"),
        ];
    }
    if model.saved_config.confirmation().is_some() {
        return vec![
            Segment::new("Enter", "confirm Saved Config save"),
            Segment::new("Esc", "cancel"),
        ];
    }
    if model.saved_config.error.is_some() {
        return vec![Segment::new("Esc", "dismiss Saved Config error")];
    }
    match model.active_page {
        ActivePage::Controls if gpio_selected(model) => vec![
            Segment::new("l", "LOW"),
            Segment::new("o", "HIGH"),
            Segment::new("i", "INPUT"),
            Segment::new("Mouse", "click/hold/2x"),
            Segment::new("Tab/Shift+Tab", "page"),
            Segment::new("g", "GPIO"),
            Segment::new("p", "pause"),
            Segment::new("r", "refresh"),
            Segment::new("PgUp/PgDn", "Move"),
            Segment::new("q", "quit"),
        ],
        ActivePage::Controls => vec![
            Segment::new("q", "quit"),
            Segment::new("Tab/Shift+Tab", "page"),
            Segment::new("Enter/click", "activate"),
            Segment::new("g", "GPIO"),
            Segment::new("p", "pause"),
            Segment::new("r", "refresh"),
            Segment::new("PgUp/PgDn", "Move"),
        ],
        ActivePage::SavedConfig => vec![
            Segment::new("", "Saved Config"),
            Segment::new("Up/Down", "item"),
            Segment::new("Space", "select"),
            Segment::new("s", "save"),
            Segment::new("x", "clear"),
            Segment::new("Tab/Shift+Tab", "page"),
            Segment::new("Esc", "back"),
        ],
        ActivePage::Status => vec![
            Segment::new("", "Status"),
            Segment::new("Up/Down", "scroll"),
            Segment::new("Tab/Shift+Tab", "page"),
            Segment::new("q", "quit"),
        ],
    }
}

pub(super) fn render_keybar(frame: &mut ratatui::Frame, area: Rect, model: &TuiModel) {
    let width = area.width as usize;
    let mut spans = Vec::new();
    let mut used = 0usize;
    for (index, segment) in keybar_segments(model).iter().enumerate() {
        let gap = if index == 0 { 0 } else { SEGMENT_GAP };
        if used + gap + segment.unit_len() > width {
            break;
        }
        if index > 0 {
            spans.push(Span::styled("  ", label_style()));
            used += SEGMENT_GAP;
        }
        if segment.key.is_empty() {
            spans.push(Span::styled(segment.label.clone(), label_style()));
        } else {
            spans.push(Span::styled(segment.key, key_style()));
            spans.push(Span::styled(" ", label_style()));
            spans.push(Span::styled(segment.label.clone(), label_style()));
        }
        used += segment.unit_len();
    }
    if used < width {
        spans.push(Span::styled(" ".repeat(width - used), label_style()));
    }
    frame.render_widget(Paragraph::new(Line::from(spans)), area);
}
