use super::config_columns::saved_config_header_line;
use super::control_columns::{header_line, RowLayout};
use super::model::TuiModel;
use super::pages::ActivePage;
use super::status_page::status_header_line;
use ratatui::layout::Rect;
use ratatui::widgets::Paragraph;

pub(super) fn render_page_header(frame: &mut ratatui::Frame, area: Rect, model: &TuiModel) {
    let width = area.width as usize;
    let line = match model.active_page {
        ActivePage::Controls => header_line(&RowLayout::new(width)),
        ActivePage::SavedConfig => saved_config_header_line(width),
        ActivePage::Status => status_header_line(width),
    };
    frame.render_widget(Paragraph::new(line), area);
}
