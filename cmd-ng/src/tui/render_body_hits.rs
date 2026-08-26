use super::model::TuiModel;
use super::render_body::{BodyContent, BodyTarget};
use ratatui::layout::Rect;

pub(super) struct BodyViewport {
    pub(super) area: Rect,
    pub(super) offset: usize,
}

pub(super) fn register_body_hits(
    viewport: BodyViewport,
    content: &BodyContent,
    model: &mut TuiModel,
) {
    for mark in &content.marks {
        if mark.line < viewport.offset {
            continue;
        }
        let visible_line = mark.line - viewport.offset;
        if visible_line >= viewport.area.height as usize
            || mark.x_start >= viewport.area.width as usize
        {
            continue;
        }
        let x_end = mark.x_end.min(viewport.area.width as usize);
        let rect = Rect::new(
            viewport.area.x + mark.x_start as u16,
            viewport.area.y + visible_line as u16,
            (x_end - mark.x_start) as u16,
            1,
        );
        match &mark.target {
            BodyTarget::Control(item) => model.hit_map.controls.push(rect, item.clone()),
            BodyTarget::SavedConfig(target) => {
                model.hit_map.saved_config_rows.push(rect, target.clone());
            }
        }
    }
}
