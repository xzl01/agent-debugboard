use super::config_render::render_confirmation;
use super::model::TuiModel;
use super::page_header::render_page_header;
use super::pages::ActivePage;
use super::render_body::render_body;
use super::render_keybar::render_keybar;
use super::render_modal::render_hardware_confirmation;
use super::render_status::render_status_band;
use super::render_telemetry::{render_telemetry, telemetry_rows};
use ratatui::layout::{Constraint, Layout, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::Paragraph;

const MIN_HEIGHT: u16 = 6;

pub(super) fn render_ui(frame: &mut ratatui::Frame, model: &mut TuiModel) {
    model.hit_map.clear();
    let area = frame.area();
    if area.width == 0 || area.height < MIN_HEIGHT {
        render_confirmation(frame, &model.saved_config);
        render_hardware_confirmation(frame, model);
        return;
    }
    let telemetry = telemetry_rows(model, area.height) as u16;
    let chunks = Layout::vertical([
        Constraint::Length(2),
        Constraint::Length(telemetry),
        Constraint::Length(1),
        Constraint::Length(1),
        Constraint::Min(1),
        Constraint::Length(1),
    ])
    .split(area);

    render_status_band(frame, chunks[0], model);
    render_telemetry(frame, chunks[1], model);
    render_tabs(frame, chunks[2], model);
    render_page_header(frame, chunks[3], model);
    render_body(frame, chunks[4], model);
    render_keybar(frame, chunks[5], model);
    render_confirmation(frame, &model.saved_config);
    render_hardware_confirmation(frame, model);
}

fn render_tabs(frame: &mut ratatui::Frame, area: Rect, model: &TuiModel) {
    let tabs = [
        ("Controls", ActivePage::Controls),
        ("Saved Config", ActivePage::SavedConfig),
        ("Status", ActivePage::Status),
    ];
    let mut spans = Vec::new();
    for (index, (label, page)) in tabs.iter().enumerate() {
        if index > 0 {
            spans.push(Span::raw("  "));
        }
        if *page == model.active_page {
            spans.push(Span::styled(
                format!(" {label} "),
                Style::default().add_modifier(Modifier::BOLD | Modifier::REVERSED),
            ));
        } else {
            spans.push(Span::styled(*label, Style::default().fg(Color::DarkGray)));
        }
    }
    frame.render_widget(Paragraph::new(Line::from(spans)), area);
}
