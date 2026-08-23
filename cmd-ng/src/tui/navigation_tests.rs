use super::controls::{control_targets, navigate, Nav};
use super::gpio_fixture::current_power_outputs;
use super::model::TuiModel;
use crate::client::DEFAULT_BASE_URL;
use crate::ws_status::{TuiStatusGpio, TuiStatusSwitchInfo, WsStatusSnapshot};
use std::time::Duration;

fn model_with_switch_and_gpio() -> TuiModel {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.width = 120;
    let mut snapshot = WsStatusSnapshot {
        power_outputs: current_power_outputs(),
        ..Default::default()
    };
    snapshot.switches.insert(
        "sd".to_string(),
        TuiStatusSwitchInfo {
            route: "target".to_string(),
            routes: vec!["target".to_string(), "usb-reader".to_string()],
            ..Default::default()
        },
    );
    snapshot.gpios.push(TuiStatusGpio {
        name: "GP13".to_string(),
        pin: 13,
        direction: "input".to_string(),
        ..Default::default()
    });
    model.apply_status_snapshot(snapshot);
    model
}

#[test]
fn right_is_inert_on_power_rows() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.width = 80;
    assert_eq!(navigate(&model, Nav::Right), 0);
}

#[test]
fn navigate_left_stays_at_zero() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.width = 80;
    assert_eq!(navigate(&model, Nav::Left), 0);
}

#[test]
fn navigation_is_independent_of_terminal_width() {
    let mut narrow = model_with_switch_and_gpio();
    narrow.width = 24;
    let mut wide = model_with_switch_and_gpio();
    wide.width = 120;
    for nav in [
        Nav::Down,
        Nav::Down,
        Nav::Down,
        Nav::Down,
        Nav::Down,
        Nav::Up,
    ] {
        assert_eq!(navigate(&narrow, nav), navigate(&wide, nav));
        narrow.control_idx = navigate(&narrow, nav);
        wide.control_idx = navigate(&wide, nav);
    }
}

#[test]
fn down_crosses_section_boundaries_along_object_rows() {
    let mut model = model_with_switch_and_gpio();
    // One object per row: power idx 0..=3, switch idx 4, gpio idx 5.
    model.control_idx = control_targets(&model).len() - 1;
    model.control_idx = navigate(&model, Nav::Down);
    assert_eq!(model.control_idx, control_targets(&model).len());
    model.control_idx = navigate(&model, Nav::Down);
    assert_eq!(model.control_idx, control_targets(&model).len() + 1);
}

#[test]
fn up_crosses_section_boundaries_along_object_rows() {
    let mut model = model_with_switch_and_gpio();
    model.control_idx = control_targets(&model).len() + 1;
    model.control_idx = navigate(&model, Nav::Up);
    assert_eq!(model.control_idx, control_targets(&model).len());
    model.control_idx = navigate(&model, Nav::Up);
    assert_eq!(model.control_idx, control_targets(&model).len() - 1);
}

#[test]
fn down_stops_at_the_last_object_row() {
    let mut model = model_with_switch_and_gpio();
    model.control_idx = control_targets(&model).len() + 1;
    assert_eq!(
        navigate(&model, Nav::Down),
        control_targets(&model).len() + 1
    );
}

#[test]
fn up_stops_at_the_first_object_row() {
    let mut model = model_with_switch_and_gpio();
    model.control_idx = 0;
    assert_eq!(navigate(&model, Nav::Up), 0);
}

#[test]
fn left_and_right_stay_within_the_active_section() {
    let mut model = model_with_switch_and_gpio();
    model.control_idx = control_targets(&model).len() - 1;
    assert_eq!(
        navigate(&model, Nav::Right),
        control_targets(&model).len() - 1
    );

    // The first switch item must not escape left into the power section.
    model.control_idx = control_targets(&model).len();
    assert_eq!(navigate(&model, Nav::Left), control_targets(&model).len());
}

#[test]
fn page_down_and_page_up_move_three_object_rows() {
    let mut model = model_with_switch_and_gpio();
    model.control_idx = 0;
    model.control_idx = navigate(&model, Nav::PageDown);
    assert_eq!(model.control_idx, 3);
    model.control_idx = navigate(&model, Nav::PageDown);
    assert_eq!(model.control_idx, control_targets(&model).len() + 1);
    model.control_idx = navigate(&model, Nav::PageUp);
    assert_eq!(model.control_idx, 2);
}
