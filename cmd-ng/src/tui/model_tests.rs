use super::control_rows::{control_rows, RowTone};
use super::controls::control_targets;
use super::model::{current_milliamp_estimate, TuiModel};
use super::TuiActionMsg;
use crate::adc::AdcReading;
use crate::client::DEFAULT_BASE_URL;
use crate::monitoring::BoardMonitoring;
use crate::ws_status::{TuiStatusGpio, TuiStatusSwitchInfo, WsStatusSnapshot};
use std::time::{Duration, Instant};

#[test]
fn saved_config_starts_unsupported_without_firmware_summary() {
    let model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));

    assert!(!model.saved_config.is_supported());
}

#[test]
fn apply_status_snapshot_updates_gpio_and_power_state() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.gpio_names = vec!["GP10".to_string()];
    model
        .gpio_notes
        .insert("GP10".to_string(), "J16_PIN1".to_string());
    model.gpio_levels.insert("GP10".to_string(), true);
    model.gpio_is_input.insert("GP10".to_string(), false);
    model.apply_status_snapshot(WsStatusSnapshot {
        power_outputs: vec![
            crate::ws_status::TuiStatusPowerOutput {
                name: "5v_out".to_string(),
                state: "on".to_string(),
                value: 1,
            },
            crate::ws_status::TuiStatusPowerOutput {
                name: "vdd_5v".to_string(),
                state: "on".to_string(),
                value: 1,
            },
        ],
        gpios: vec![
            TuiStatusGpio {
                name: "GP11".to_string(),
                pin: 11,
                value: Some(0),
                direction: "output".to_string(),
                note: "J16_PIN2".to_string(),
                ..Default::default()
            },
            TuiStatusGpio {
                name: "GP7".to_string(),
                pin: 7,
                value: Some(0),
                direction: "input".to_string(),
                note: "CON_MAS".to_string(),
                ..Default::default()
            },
        ],
        board_monitoring: BoardMonitoring::default(),
        ..Default::default()
    });

    // Snapshot becomes authoritative under REST polling.
    assert_eq!(model.power_states.get("5v_out"), Some(&true));
    assert_eq!(model.power_states.get("vdd_5v"), Some(&true));
    assert_eq!(model.gpio_levels.get("GP11"), Some(&false));
    assert_eq!(model.gpio_is_input.get("GP11"), Some(&false));
    assert_eq!(model.gpio_notes.get("GP11"), Some(&"J16_PIN2".to_string()));
    assert_eq!(model.gpio_levels.get("GP10"), None);
}

#[test]
fn apply_adc_response_updates_history_and_power_state() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.apply_adc_response(vec![AdcReading {
        name: "5v_out".to_string(),
        signal: "S_C_5V".to_string(),
        raw: None,
        current_valid: Some(true),
        mv: Some(17),
        ma_est: Some(500),
        power_enabled: Some(true),
        kind: crate::adc::AdcKind::Current,
        sensor_channel: "current".to_string(),
        unit: "A".to_string(),
        value: Some(500000),
        sensor_value: None,
        current_ua: Some(500000),
        voltage_uv: None,
    }]);

    assert_eq!(model.power_states.get("5v_out"), Some(&true));
    assert_eq!(model.history.get("5v_out").map(|v| v.len()), Some(1));
}

#[test]
fn milliamp_estimate_falls_back_to_current_microamps() {
    let reading = AdcReading {
        name: "5v_out".to_string(),
        signal: "S_C_5V".to_string(),
        raw: None,
        current_valid: None,
        mv: None,
        ma_est: None,
        power_enabled: Some(true),
        kind: crate::adc::AdcKind::Current,
        sensor_channel: "current".to_string(),
        unit: "A".to_string(),
        value: Some(850_000),
        sensor_value: None,
        current_ua: Some(850_000),
        voltage_uv: None,
    };

    assert_eq!(current_milliamp_estimate(&reading), 850);
}

#[test]
fn voltage_reading_is_ignored_by_power_history_and_tui_sections() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.apply_adc_response(vec![AdcReading {
        name: "adc3".to_string(),
        signal: "ADC3".to_string(),
        raw: Some(42),
        current_valid: None,
        mv: Some(1234),
        ma_est: None,
        power_enabled: None,
        kind: crate::adc::AdcKind::Voltage,
        sensor_channel: "voltage".to_string(),
        unit: "V".to_string(),
        sensor_value: Some(crate::adc::AdcSensorValue {
            val1: 1,
            val2: 234000,
        }),
        value: Some(1_234_000),
        current_ua: None,
        voltage_uv: Some(1_234_000),
    }]);

    assert!(!model.history.contains_key("adc3"));
    assert!(!model.latest.contains_key("adc3"));
    assert!(!model.power_states.contains_key("adc3"));
}

#[test]
fn apply_status_snapshot_tracks_actual_enumerated_switch_routes() -> Result<(), &'static str> {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    let mut snapshot = WsStatusSnapshot::default();
    snapshot.switches.insert(
        "example".to_string(),
        TuiStatusSwitchInfo {
            route: "alternate".to_string(),
            routes: vec!["current".to_string(), "alternate".to_string()],
            ..Default::default()
        },
    );

    model.apply_status_snapshot(snapshot);

    let state = model
        .switches
        .get("example")
        .ok_or("snapshot switch missing")?;
    assert_eq!(state.actual_route, "alternate");
    assert_eq!(state.desired_route, "alternate");
    Ok(())
}

#[test]
fn expired_switch_request_preserves_desired_route_as_mismatch() -> Result<(), &'static str> {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    let mut snapshot = WsStatusSnapshot::default();
    snapshot.switches.insert(
        "sd".to_string(),
        TuiStatusSwitchInfo {
            route: "target".to_string(),
            routes: vec!["target".to_string(), "usb-reader".to_string()],
            ..Default::default()
        },
    );
    model.apply_status_snapshot(snapshot.clone());
    let state = model
        .switches
        .get_mut("sd")
        .ok_or("snapshot switch missing")?;
    state.desired_route = "usb-reader".to_string();
    state.pending_route = Some("usb-reader".to_string());
    state.pending_until = Some(Instant::now() - Duration::from_secs(1));
    state.route_intent_active = true;

    model.apply_status_snapshot(snapshot);

    let state = model.switches.get("sd").ok_or("snapshot switch missing")?;
    assert_eq!(state.desired_route, "usb-reader");
    assert_eq!(state.actual_route, "target");
    assert!(state.pending_route.is_none());
    assert!(state.pending_until.is_none());
    assert_eq!(
        control_rows(&model)
            .iter()
            .find(|row| row.name == "sd")
            .map(|row| row.tone),
        Some(RowTone::SwitchMismatch)
    );
    Ok(())
}

#[test]
fn matching_switch_readback_clears_latched_route_intent() -> Result<(), &'static str> {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    let mut snapshot = WsStatusSnapshot::default();
    snapshot.switches.insert(
        "sd".to_string(),
        TuiStatusSwitchInfo {
            route: "target".to_string(),
            routes: vec!["target".to_string(), "usb-reader".to_string()],
            ..Default::default()
        },
    );
    model.apply_status_snapshot(snapshot);
    let state = model
        .switches
        .get_mut("sd")
        .ok_or("snapshot switch missing")?;
    state.desired_route = "usb-reader".to_string();
    state.route_intent_active = true;

    let mut matching = WsStatusSnapshot::default();
    matching.switches.insert(
        "sd".to_string(),
        TuiStatusSwitchInfo {
            route: "usb-reader".to_string(),
            routes: vec!["target".to_string(), "usb-reader".to_string()],
            ..Default::default()
        },
    );
    model.apply_status_snapshot(matching);

    let state = model.switches.get("sd").ok_or("snapshot switch missing")?;
    assert_eq!(state.desired_route, "usb-reader");
    assert_eq!(state.actual_route, "usb-reader");
    assert!(!state.route_intent_active);
    assert_eq!(
        control_rows(&model)
            .iter()
            .find(|row| row.name == "sd")
            .map(|row| row.tone),
        Some(RowTone::SwitchReady)
    );
    Ok(())
}

#[test]
fn apply_action_msg_updates_status() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.apply_action_msg(TuiActionMsg {
        status: "power 12v_out=on".to_string(),
        err: None,
    });
    assert_eq!(model.status, "power 12v_out=on");
}

#[test]
fn gpio_selection_and_input_mode_update_status() {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.gpio_names = vec!["GP13".to_string()];
    model.gpio_levels.insert("GP13".to_string(), false);
    model.gpio_is_input.insert("GP13".to_string(), false);
    model.control_idx = control_targets(&model).len() + 3;

    model.apply_action_msg(TuiActionMsg {
        status: "gpio GP13=1".to_string(),
        err: None,
    });
    assert_eq!(model.status, "gpio GP13=1");

    model.apply_action_msg(TuiActionMsg {
        status: "gpio GP13=input".to_string(),
        err: None,
    });
    assert_eq!(model.status, "gpio GP13=input");
}
