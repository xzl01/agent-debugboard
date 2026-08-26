use super::config_render::render_confirmation;
use super::hit_types::TabTarget;
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
        render_hardware_confirmation(frame, model);
        render_confirmation(
            frame,
            &model.saved_config,
            &mut model.hit_map.saved_config_modal,
        );
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
    render_hardware_confirmation(frame, model);
    render_confirmation(
        frame,
        &model.saved_config,
        &mut model.hit_map.saved_config_modal,
    );
}

fn render_tabs(frame: &mut ratatui::Frame, area: Rect, model: &mut TuiModel) {
    let tabs = [
        ("Controls", ActivePage::Controls),
        ("Saved Config", ActivePage::SavedConfig),
        ("Status", ActivePage::Status),
    ];
    let mut spans = Vec::new();
    let mut x = area.x;
    for (index, (label, page)) in tabs.iter().enumerate() {
        if index > 0 {
            spans.push(Span::raw("  "));
            x = x.saturating_add(2);
        }
        let active = *page == model.active_page;
        let rendered_label = if active {
            format!(" {label} ")
        } else {
            (*label).to_string()
        };
        let label_width = rendered_label.len() as u16;
        let visible_width = label_width.min(area.x.saturating_add(area.width).saturating_sub(x));
        if area.height > 0 && visible_width > 0 {
            model
                .hit_map
                .tabs
                .push(Rect::new(x, area.y, visible_width, 1), TabTarget(*page));
        }
        let style = if active {
            Style::default().add_modifier(Modifier::BOLD | Modifier::REVERSED)
        } else {
            Style::default().fg(Color::DarkGray)
        };
        spans.push(Span::styled(rendered_label, style));
        x = x.saturating_add(label_width);
    }
    frame.render_widget(Paragraph::new(Line::from(spans)), area);
}
