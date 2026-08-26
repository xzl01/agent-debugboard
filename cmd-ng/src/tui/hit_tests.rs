use super::controls::ControlItem;
use super::hit::{HitMap, HitRegions};
use super::hit_types::{
    HardwareModalTarget, SavedConfigModalTarget, SavedConfigRowTarget, TabTarget,
};
use super::pages::ActivePage;
use crate::persistent_config::ConfigItemId;
use ratatui::layout::Rect;

#[test]
fn hit_regions_include_top_left_and_exclude_bottom_right_boundaries() {
    let mut hits = HitRegions::default();
    hits.push(Rect::new(10, 20, 2, 3), TabTarget(ActivePage::Status));

    assert_eq!(
        hits.at(10, 20),
        Some(&TabTarget(ActivePage::Status)),
        "the top-left cell belongs to the region"
    );
    assert_eq!(
        hits.at(11, 22),
        Some(&TabTarget(ActivePage::Status)),
        "the last interior cell belongs to the region"
    );
    assert_eq!(hits.at(9, 20), None, "the left edge is exclusive outside");
    assert_eq!(hits.at(12, 20), None, "the right edge is exclusive");
    assert_eq!(hits.at(10, 19), None, "the row above is outside");
    assert_eq!(hits.at(10, 23), None, "the bottom edge is exclusive");
}

#[test]
fn hit_map_clear_removes_every_typed_scope() {
    let rect = Rect::new(1, 2, 3, 4);
    let mut hits = HitMap::default();
    hits.controls
        .push(rect, ControlItem::Power("12v_out".to_string()));
    hits.tabs.push(rect, TabTarget(ActivePage::SavedConfig));
    hits.saved_config_rows.push(
        rect,
        SavedConfigRowTarget(ConfigItemId("power/alpha".to_string())),
    );
    hits.hardware_modal
        .push(rect, HardwareModalTarget::confirm());
    hits.saved_config_modal
        .push(rect, SavedConfigModalTarget::cancel());

    hits.clear();

    assert!(hits.controls.is_empty());
    assert!(hits.tabs.is_empty());
    assert!(hits.saved_config_rows.is_empty());
    assert!(hits.hardware_modal.is_empty());
    assert!(hits.saved_config_modal.is_empty());
}
