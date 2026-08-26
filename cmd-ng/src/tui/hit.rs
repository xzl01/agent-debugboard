use super::controls::ControlItem;
use super::hit_types::{
    HardwareModalTarget, SavedConfigModalTarget, SavedConfigRowTarget, TabTarget,
};
use ratatui::layout::Rect;

#[derive(Debug)]
pub(super) struct HitRegion<T> {
    rect: Rect,
    target: T,
}

impl<T> HitRegion<T> {
    fn contains(&self, column: u16, row: u16) -> bool {
        column >= self.rect.x
            && column < self.rect.x + self.rect.width
            && row >= self.rect.y
            && row < self.rect.y + self.rect.height
    }
}

#[derive(Debug)]
pub(super) struct HitRegions<T> {
    regions: Vec<HitRegion<T>>,
}

impl<T> Default for HitRegions<T> {
    fn default() -> Self {
        Self {
            regions: Vec::new(),
        }
    }
}

impl<T> HitRegions<T> {
    pub(super) fn push(&mut self, rect: Rect, target: T) {
        self.regions.push(HitRegion { rect, target });
    }

    pub(super) fn at(&self, column: u16, row: u16) -> Option<&T> {
        self.regions
            .iter()
            .find(|region| region.contains(column, row))
            .map(|region| &region.target)
    }

    pub(super) fn clear(&mut self) {
        self.regions.clear();
    }

    #[cfg(test)]
    pub(super) fn is_empty(&self) -> bool {
        self.regions.is_empty()
    }

    #[cfg(test)]
    pub(super) fn iter(&self) -> impl Iterator<Item = (&Rect, &T)> {
        self.regions
            .iter()
            .map(|region| (&region.rect, &region.target))
    }
}

#[derive(Debug, Default)]
pub(super) struct HitMap {
    pub(super) controls: HitRegions<ControlItem>,
    pub(super) tabs: HitRegions<TabTarget>,
    pub(super) saved_config_rows: HitRegions<SavedConfigRowTarget>,
    pub(super) hardware_modal: HitRegions<HardwareModalTarget>,
    pub(super) saved_config_modal: HitRegions<SavedConfigModalTarget>,
}

impl HitMap {
    pub(super) fn clear(&mut self) {
        self.controls.clear();
        self.tabs.clear();
        self.saved_config_rows.clear();
        self.hardware_modal.clear();
        self.saved_config_modal.clear();
    }
}
