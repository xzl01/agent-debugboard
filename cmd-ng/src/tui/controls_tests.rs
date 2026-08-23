use super::control_rows::control_rows;
use super::controls::{control_items, control_targets, first_gpio_index, ControlItem};
use super::model::TuiModel;
use crate::client::DEFAULT_BASE_URL;
use crate::ws_status::{
    TuiStatusGpio, TuiStatusPowerOutput, TuiStatusSwitchInfo, WsStatusSnapshot,
};
use std::time::Duration;

#[test]
fn control_items_include_switches_between_power_and_gpio() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.gpio_names = vec!["GP13".to_string()];
    let mut snapshot = WsStatusSnapshot::default();
    snapshot.switches.insert(
        "alpha".to_string(),
        TuiStatusSwitchInfo {
            route: "one".to_string(),
            routes: vec!["one".to_string(), "two".to_string()],
            ..Default::default()
        },
    );
    snapshot.switches.insert(
        "beta".to_string(),
        TuiStatusSwitchInfo {
            route: "left".to_string(),
            routes: vec!["left".to_string(), "right".to_string()],
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

    let items = control_items(&model);
    assert_eq!(
        items[control_targets(&model).len()],
        ControlItem::Switch("alpha".to_string())
    );
    assert_eq!(
        items[control_targets(&model).len() + 1],
        ControlItem::Switch("beta".to_string())
    );
    assert_eq!(
        items[control_targets(&model).len() + 2],
        ControlItem::Gpio("GP13".to_string())
    );
}

#[test]
fn switch_control_is_visible_only_when_reported_by_firmware() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    assert!(control_items(&model)
        .iter()
        .all(|item| !matches!(item, ControlItem::Switch(_))));

    let mut snapshot = WsStatusSnapshot::default();
    snapshot.switches.insert(
        "vin".to_string(),
        TuiStatusSwitchInfo {
            route: "3.3v".to_string(),
            routes: vec!["1.8v".to_string(), "3.3v".to_string()],
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

    let items = control_items(&model);
    assert_eq!(
        items[control_targets(&model).len()],
        ControlItem::Switch("vin".to_string())
    );
    assert_eq!(
        first_gpio_index(&model),
        Some(control_targets(&model).len() + 1)
    );
    assert!(control_rows(&model)
        .iter()
        .any(|row| row.kind == "switch" && row.name == "vin" && row.state_route == "3.3v"));
}

#[test]
fn control_rows_show_current_switch_routes() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    let mut snapshot = WsStatusSnapshot::default();
    snapshot.switches.insert(
        "sd".to_string(),
        TuiStatusSwitchInfo {
            route: "usb-reader".to_string(),
            routes: vec!["target".to_string(), "usb-reader".to_string()],
            ..Default::default()
        },
    );
    snapshot.switches.insert(
        "usb".to_string(),
        TuiStatusSwitchInfo {
            route: "target".to_string(),
            routes: vec!["pc".to_string(), "target".to_string()],
            ..Default::default()
        },
    );
    model.apply_status_snapshot(snapshot);

    let rows = control_rows(&model);
    assert!(rows
        .iter()
        .any(|row| row.name == "sd" && row.state_route == "usb-reader"));
    assert!(rows
        .iter()
        .any(|row| row.name == "usb" && row.state_route == "target"));
}

#[test]
fn power_controls_follow_firmware_catalog_order_including_future_outputs() {
    // Given: firmware advertises a future rail before an existing rail.
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    let snapshot = WsStatusSnapshot {
        power_outputs: vec![
            TuiStatusPowerOutput {
                name: "future_aux".to_string(),
                state: "on".to_string(),
                value: 1,
            },
            TuiStatusPowerOutput {
                name: "12v_out".to_string(),
                state: "off".to_string(),
                value: 0,
            },
        ],
        ..Default::default()
    };

    // When: the TUI applies the authoritative status snapshot.
    model.apply_status_snapshot(snapshot);

    // Then: controls preserve the firmware catalog without host-known filtering.
    assert_eq!(
        control_items(&model),
        vec![
            ControlItem::Power("future_aux".to_string()),
            ControlItem::Power("12v_out".to_string()),
        ]
    );
}

#[test]
fn power_controls_remove_outputs_absent_from_the_latest_snapshot() {
    // Given: an initial firmware catalog containing two rails.
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.apply_status_snapshot(WsStatusSnapshot {
        power_outputs: vec![
            TuiStatusPowerOutput {
                name: "12v_out".to_string(),
                state: "on".to_string(),
                value: 1,
            },
            TuiStatusPowerOutput {
                name: "retired_aux".to_string(),
                state: "off".to_string(),
                value: 0,
            },
        ],
        ..Default::default()
    });

    // When: the next authoritative snapshot no longer advertises the retired rail.
    model.apply_status_snapshot(WsStatusSnapshot {
        power_outputs: vec![TuiStatusPowerOutput {
            name: "12v_out".to_string(),
            state: "off".to_string(),
            value: 0,
        }],
        ..Default::default()
    });

    // Then: both the catalog and state map drop the stale rail.
    assert_eq!(
        control_items(&model),
        vec![ControlItem::Power("12v_out".to_string())]
    );
    assert_eq!(model.power_states.get("retired_aux"), None);
}

#[test]
fn catalog_refresh_preserves_selected_hardware_identity_or_clamps_removed_item() {
    // Given: a selected power output in a firmware catalog.
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.apply_status_snapshot(WsStatusSnapshot {
        power_outputs: vec![
            TuiStatusPowerOutput {
                name: "alpha".to_string(),
                state: "off".to_string(),
                value: 0,
            },
            TuiStatusPowerOutput {
                name: "beta".to_string(),
                state: "off".to_string(),
                value: 0,
            },
        ],
        ..Default::default()
    });
    model.control_idx = 1;

    // When: firmware reorders the catalog while retaining the selected output.
    model.apply_status_snapshot(WsStatusSnapshot {
        power_outputs: vec![
            TuiStatusPowerOutput {
                name: "beta".to_string(),
                state: "on".to_string(),
                value: 1,
            },
            TuiStatusPowerOutput {
                name: "alpha".to_string(),
                state: "off".to_string(),
                value: 0,
            },
        ],
        ..Default::default()
    });

    // Then: selection follows the same hardware identity.
    assert_eq!(model.control_idx, 0);
    assert_eq!(
        control_items(&model).get(model.control_idx),
        Some(&ControlItem::Power("beta".to_string()))
    );

    // When: firmware subsequently removes that selected output.
    model.apply_status_snapshot(WsStatusSnapshot {
        power_outputs: vec![TuiStatusPowerOutput {
            name: "alpha".to_string(),
            state: "off".to_string(),
            value: 0,
        }],
        ..Default::default()
    });

    // Then: the cursor is clamped to the remaining catalog.
    assert_eq!(model.control_idx, 0);
    assert_eq!(
        control_items(&model).get(model.control_idx),
        Some(&ControlItem::Power("alpha".to_string()))
    );
}
