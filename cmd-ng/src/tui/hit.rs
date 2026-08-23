use super::controls::ControlItem;
use ratatui::layout::Rect;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum ModalButton {
    Confirm,
    Cancel,
}

#[derive(Debug, Default)]
pub(super) struct HitMap {
    pub(super) controls: Vec<(Rect, ControlItem)>,
    pub(super) confirm_button: Option<Rect>,
    pub(super) cancel_button: Option<Rect>,
}

impl HitMap {
    pub(super) fn clear(&mut self) {
        self.controls.clear();
        self.confirm_button = None;
        self.cancel_button = None;
    }

    pub(super) fn push_control(&mut self, rect: Rect, item: ControlItem) {
        self.controls.push((rect, item));
    }

    pub(super) fn control_at(&self, column: u16, row: u16) -> Option<&ControlItem> {
        self.controls
            .iter()
            .find(|(rect, _)| rect_contains(*rect, column, row))
            .map(|(_, item)| item)
    }

    pub(super) fn modal_button_at(&self, column: u16, row: u16) -> Option<ModalButton> {
        if self
            .confirm_button
            .is_some_and(|rect| rect_contains(rect, column, row))
        {
            return Some(ModalButton::Confirm);
        }
        if self
            .cancel_button
            .is_some_and(|rect| rect_contains(rect, column, row))
        {
            return Some(ModalButton::Cancel);
        }
        None
    }
}

fn rect_contains(rect: Rect, column: u16, row: u16) -> bool {
    column >= rect.x && column < rect.x + rect.width && row >= rect.y && row < rect.y + rect.height
}
