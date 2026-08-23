use super::pages::{clamp_scroll, ensure_visible, ActivePage};
use super::TuiModel;
use crate::client::DEFAULT_BASE_URL;
use crate::persistent_config::{ConfigAction, PersistentConfigResponse, PersistentConfigStatus};
use std::time::Duration;

const SHOW: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":1},"pending":0,"items":[{"id":"power/alpha","kind":"power","current":{"state":"off"},"saved":{"state":"on"},"selected":true,"requires_confirm":true,"apply_state":"pending"},{"id":"switch/beta","kind":"switch","current":{"route":"pc"},"saved":{"route":"target"},"selected":false,"requires_confirm":false,"apply_state":"applied"}]}"#;

fn model() -> TuiModel {
    TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2))
}

fn loaded_model() -> TuiModel {
    let response = PersistentConfigResponse::from_raw(SHOW.to_string()).unwrap();
    response.validate(&ConfigAction::Get, None).unwrap();
    let mut model = model();
    model
        .saved_config
        .observe_summary(Some(PersistentConfigStatus {
            available: true,
            reason: "ready".to_string(),
            saved_count: 1,
            pending_count: 0,
        }));
    model.saved_config.apply_authoritative(response).unwrap();
    model
}

#[test]
fn active_page_defaults_to_controls() {
    assert_eq!(model().active_page, ActivePage::Controls);
}

#[test]
fn next_page_cycles_controls_saved_config_status() {
    let mut model = model();
    model.next_page();
    assert_eq!(model.active_page, ActivePage::SavedConfig);
    model.next_page();
    assert_eq!(model.active_page, ActivePage::Status);
    model.next_page();
    assert_eq!(model.active_page, ActivePage::Controls);
}

#[test]
fn prev_page_cycles_in_reverse() {
    let mut model = model();
    model.prev_page();
    assert_eq!(model.active_page, ActivePage::Status);
    model.prev_page();
    assert_eq!(model.active_page, ActivePage::SavedConfig);
    model.prev_page();
    assert_eq!(model.active_page, ActivePage::Controls);
}

#[test]
fn entering_saved_config_focuses_loaded_config_and_leaving_blurs() {
    let mut model = loaded_model();
    model.next_page();
    assert!(model.saved_config.focused);
    model.next_page();
    assert!(!model.saved_config.focused);
    model.prev_page();
    assert!(model.saved_config.focused);
}

#[test]
fn entering_saved_config_without_backend_support_does_not_focus() {
    let mut model = model();
    model.next_page();
    assert_eq!(model.active_page, ActivePage::SavedConfig);
    assert!(!model.saved_config.focused);
}

#[test]
fn config_cursor_and_selection_persist_across_page_switches() {
    let mut model = loaded_model();
    model.next_page();
    model.saved_config.move_cursor(1);
    model.saved_config.toggle_current();
    let selected = model.saved_config.selected_ids();

    model.next_page();
    model.next_page();
    model.next_page();

    assert_eq!(model.active_page, ActivePage::SavedConfig);
    assert_eq!(model.saved_config.cursor, 1);
    assert_eq!(model.saved_config.selected_ids(), selected);
    assert!(model.saved_config.focused);
}

#[test]
fn page_scroll_offsets_are_independent() {
    let mut model = model();
    model.set_page_scroll(5);
    model.next_page();
    model.set_page_scroll(2);
    model.next_page();
    model.set_page_scroll(7);

    assert_eq!(model.page_scroll(), 7);
    model.prev_page();
    assert_eq!(model.page_scroll(), 2);
    model.prev_page();
    assert_eq!(model.page_scroll(), 5);
}

#[test]
fn ensure_visible_scrolls_down_when_selection_is_below_the_viewport() {
    assert_eq!(ensure_visible(10, 0, 5, 20), 6);
}

#[test]
fn ensure_visible_scrolls_up_when_selection_is_above_the_viewport() {
    assert_eq!(ensure_visible(2, 8, 5, 20), 2);
}

#[test]
fn ensure_visible_keeps_offset_when_selection_is_visible() {
    assert_eq!(ensure_visible(4, 2, 5, 20), 2);
}

#[test]
fn ensure_visible_clamps_to_the_end_of_short_content() {
    assert_eq!(ensure_visible(3, 0, 10, 5), 0);
}

#[test]
fn clamp_scroll_limits_offset_to_the_scrollable_range() {
    assert_eq!(clamp_scroll(30, 20, 5), 15);
    assert_eq!(clamp_scroll(3, 20, 5), 3);
    assert_eq!(clamp_scroll(5, 4, 10), 0);
}
