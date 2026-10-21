use super::control_columns::{column_plan, row_line, Column, RowLayout};
use super::control_rows::{control_rows, ControlRow, RowTone};
use super::controls::control_targets;
use super::gpio_fixture::{current_adc_channels, current_power_outputs};
use super::model::TuiModel;
use crate::adc::AdcReading;
use crate::client::DEFAULT_BASE_URL;
use crate::ws_status::{TuiStatusGpio, TuiStatusSwitchInfo, WsStatusSnapshot};
use anyhow::{anyhow, Result};
use ratatui::style::{Color, Modifier};
use std::time::Duration;

fn model_with_states() -> TuiModel {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    let mut snapshot = WsStatusSnapshot {
        power_outputs: current_power_outputs(),
        adc_channels: current_adc_channels(),
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
    snapshot.switches.insert(
        "tf_wp".to_string(),
        TuiStatusSwitchInfo {
            route: "writable".to_string(),
            routes: vec!["writable".to_string(), "protected".to_string()],
            requires_confirm: true,
        },
    );
    snapshot.gpios.push(TuiStatusGpio {
        name: "GP13".to_string(),
        pin: 13,
        value: Some(1),
        direction: "output".to_string(),
        note: "J16_PIN1".to_string(),
        ..Default::default()
    });
    snapshot.gpios.push(TuiStatusGpio {
        name: "GP14".to_string(),
        pin: 14,
        value: Some(0),
        direction: "input".to_string(),
        note: "J16_PIN2".to_string(),
        ..Default::default()
    });
    model.apply_status_snapshot(snapshot);
    model.power_states.insert("12v_out".to_string(), true);
    model
}

fn row_named<'a>(rows: &'a [ControlRow], name: &str) -> Result<&'a ControlRow> {
    rows.iter()
        .find(|row| row.name == name)
        .ok_or_else(|| anyhow!("missing control row {name}"))
}

#[test]
fn rows_follow_frozen_control_items_order() {
    let model = model_with_states();
    let rows = control_rows(&model);
    assert_eq!(rows.len(), control_targets(&model).len() + 2 + 1 + 2);
    let names: Vec<&str> = rows.iter().map(|row| row.name.as_str()).collect();
    assert_eq!(
        names,
        vec!["12v_out", "5v_out", "20v_out", "vdd_5v", "sd", "tf_wp", "Reboot", "GP13", "GP14"]
    );
    let kinds: Vec<&str> = rows.iter().map(|row| row.kind).collect();
    assert_eq!(
        kinds,
        vec!["power", "power", "power", "power", "switch", "switch", "system", "gpio", "gpio"]
    );
}

#[test]
fn reboot_row_is_a_static_normal_restart_after_switches() -> Result<()> {
    let model = model_with_states();
    let rows = control_rows(&model);
    let reboot = row_named(&rows, "Reboot")?;

    assert_eq!(reboot.kind, "system");
    assert_eq!(reboot.state_route, "ready");
    assert_eq!(reboot.mode, "confirm");
    assert_eq!(reboot.description, "normal cold restart; not ROM BOOTSEL");
    assert_eq!(reboot.tone, RowTone::System);
    assert!(
        rows.iter().position(|row| row.name == "tf_wp").unwrap()
            < rows.iter().position(|row| row.name == "Reboot").unwrap()
    );
    assert!(
        rows.iter().position(|row| row.name == "Reboot").unwrap()
            < rows.iter().position(|row| row.name == "GP13").unwrap()
    );
    Ok(())
}

#[test]
fn power_row_exposes_state_live_mode_and_description() -> Result<()> {
    let mut model = model_with_states();
    let rows = control_rows(&model);

    let on = row_named(&rows, "12v_out")?;
    assert_eq!(on.state_route, "on");
    assert_eq!(on.tone, RowTone::PowerOn);
    assert_eq!(on.live, "-");
    assert_eq!(on.mode, "-");
    assert_eq!(on.description, "12v_out");

    let off = row_named(&rows, "5v_out")?;
    assert_eq!(off.state_route, "off");
    assert_eq!(off.tone, RowTone::PowerOff);

    let vdd = row_named(&rows, "vdd_5v")?;
    assert_eq!(vdd.description, "vdd_5v");

    model.latest.insert(
        "12v_out".to_string(),
        AdcReading {
            name: "12v_out".to_string(),
            current_ua: Some(42000),
            ..Default::default()
        },
    );
    let rows = control_rows(&model);
    assert_eq!(row_named(&rows, "12v_out")?.live, "0.042000A");
    Ok(())
}

#[test]
fn fifth_x1_2_power_output_keeps_state_without_an_adc_descriptor_or_sample() -> Result<()> {
    let raw: serde_json::Value = serde_json::from_str(super::gpio_fixture::X1_2_STATUS_JSON)?;
    let hub = raw["power_outputs"]
        .as_array()
        .unwrap()
        .iter()
        .find(|output| output["name"] == "5v_usb_hub")
        .unwrap();
    assert_eq!(hub["signal"], "GPIO24_USB_CRL");
    assert_eq!(hub["pin"], 24);
    let mut snapshot: WsStatusSnapshot = serde_json::from_value(raw)?;
    snapshot.adc_channels = serde_json::from_value(crate::adc::fixture::channels())?;
    let mut model = super::gpio_fixture::x1_2_model()?;
    for (state, value) in [("off", 0), ("on", 1)] {
        let hub = snapshot
            .power_outputs
            .iter_mut()
            .find(|output| output.name == "5v_usb_hub")
            .unwrap();
        hub.state = state.to_string();
        hub.value = value;
        model.apply_status_snapshot(snapshot.clone());
        model.apply_adc_response(
            crate::adc::transform_response(&crate::adc::fixture::http().to_string())
                .map_err(anyhow::Error::msg)?
                .readings,
        );
        let rows = control_rows(&model);
        let hub = row_named(&rows, "5v_usb_hub")?;
        assert_eq!(hub.state_route, state);
        assert_eq!(hub.live, "-");
        assert_eq!(hub.description, "-");
        assert_eq!(model.power_names.len(), 5);
        assert_eq!(model.channel_ids.len(), 4);
        assert_eq!(model.voltage_channel_ids.len(), 4);
        assert_eq!(model.latest.len(), 8);
        assert!(!model.latest.contains_key("5v_usb_hub"));
        assert!(!model.history.contains_key("5v_usb_hub"));
    }
    Ok(())
}

#[test]
fn empty_invalid_or_voltage_only_power_readings_do_not_fabricate_zero_current() -> Result<()> {
    let mut model = model_with_states();
    for reading in [
        AdcReading::default(),
        AdcReading {
            current_valid: Some(false),
            current_ua: Some(0),
            ..Default::default()
        },
        AdcReading {
            kind: crate::adc::AdcKind::Voltage,
            voltage_uv: Some(0),
            ..Default::default()
        },
    ] {
        model.latest.insert("12v_out".to_string(), reading);
        assert_eq!(row_named(&control_rows(&model), "12v_out")?.live, "-");
    }
    model.latest.insert(
        "12v_out".to_string(),
        AdcReading {
            current_ua: Some(0),
            ..Default::default()
        },
    );
    assert_eq!(
        row_named(&control_rows(&model), "12v_out")?.live,
        "0.000000A"
    );
    Ok(())
}

#[test]
fn switch_row_covers_state_tones_and_effective_confirm_mode() -> Result<()> {
    let mut model = model_with_states();
    let rows = control_rows(&model);

    let ready = row_named(&rows, "sd")?;
    assert_eq!(ready.state_route, "target");
    assert_eq!(ready.tone, RowTone::SwitchReady);
    assert_eq!(ready.mode, "confirm");
    assert_eq!(ready.description, "target/usb-reader");

    let confirm = row_named(&rows, "tf_wp")?;
    assert_eq!(confirm.mode, "confirm");

    if let Some(state) = model.switches.get_mut("sd") {
        state.desired_route = "usb-reader".to_string();
    }
    let rows = control_rows(&model);
    let mismatch = row_named(&rows, "sd")?;
    assert_eq!(mismatch.state_route, "usb-reader(->target)");
    assert_eq!(mismatch.tone, RowTone::SwitchMismatch);

    if let Some(state) = model.switches.get_mut("sd") {
        state.pending_route = Some("usb-reader".to_string());
    }
    let rows = control_rows(&model);
    let pending = row_named(&rows, "sd")?;
    assert_eq!(pending.state_route, "usb-reader(->target) (pending)");
    assert_eq!(pending.tone, RowTone::SwitchPending);
    Ok(())
}

#[test]
fn gpio_row_covers_level_mode_note_and_tone() -> Result<()> {
    let model = model_with_states();
    let rows = control_rows(&model);

    let high = row_named(&rows, "GP13")?;
    assert_eq!(high.state_route, "1");
    assert_eq!(high.tone, RowTone::GpioHigh);
    assert_eq!(high.mode, "out");
    assert_eq!(high.live, "-");
    assert_eq!(high.description, "J16_PIN1");

    let low = row_named(&rows, "GP14")?;
    assert_eq!(low.state_route, "0");
    assert_eq!(low.tone, RowTone::GpioLow);
    assert_eq!(low.mode, "in");
    Ok(())
}

#[test]
fn column_plan_drops_columns_right_to_left() {
    let widths =
        |plan: &[Column]| -> Vec<usize> { plan.iter().map(|column| column.width).collect() };
    assert_eq!(widths(&column_plan(120)), vec![6, 12, 14, 9, 7, 62]);
    assert_eq!(widths(&column_plan(66)), vec![6, 12, 14, 9, 7, 8]);
    assert_eq!(widths(&column_plan(60)), vec![6, 12, 14, 9, 7]);
    assert_eq!(widths(&column_plan(50)), vec![6, 12, 14, 7]);
    assert_eq!(widths(&column_plan(40)), vec![6, 12, 14]);
    assert_eq!(widths(&column_plan(30)), vec![6, 12, 8]);
    assert_eq!(widths(&column_plan(20)), vec![6, 12]);
    assert_eq!(widths(&column_plan(6)), vec![6]);
}

#[test]
fn unselected_low_gpio_state_cell_is_darkgray_without_background() -> Result<()> {
    let model = model_with_states();
    let rows = control_rows(&model);
    let low = row_named(&rows, "GP14")?;
    let layout = RowLayout::new(76);
    let line = row_line(low, &layout, false);
    let state = line
        .spans
        .iter()
        .find(|span| span.content.starts_with('0'))
        .ok_or_else(|| anyhow!("missing LOW state span"))?;
    assert_eq!(state.style.fg, Some(Color::DarkGray));
    assert_eq!(state.style.bg, None);
    let high = row_named(&rows, "GP13")?;
    let line = row_line(high, &layout, false);
    let state = line
        .spans
        .iter()
        .find(|span| span.content.starts_with('1'))
        .ok_or_else(|| anyhow!("missing HIGH state span"))?;
    assert_eq!(state.style.fg, Some(Color::Red));
    assert!(state.style.add_modifier.contains(Modifier::BOLD));
    Ok(())
}

#[test]
fn selected_row_paints_full_width_and_preserves_state_fg() -> Result<()> {
    let model = model_with_states();
    let rows = control_rows(&model);
    let on = row_named(&rows, "12v_out")?;
    let layout = RowLayout::new(76);
    let line = row_line(on, &layout, true);
    let total: usize = line
        .spans
        .iter()
        .map(|span| span.content.chars().count())
        .sum();
    assert_eq!(total, 76, "selected row must cover the full content width");
    for span in &line.spans {
        assert_eq!(
            span.style.bg,
            Some(Color::White),
            "span {span:?} lacks White bg"
        );
        assert!(span.style.add_modifier.contains(Modifier::BOLD));
    }
    let state = line
        .spans
        .iter()
        .find(|span| span.content.starts_with("on"))
        .ok_or_else(|| anyhow!("missing selected ON state span"))?;
    assert_eq!(state.style.fg, Some(Color::Green));
    let kind = line
        .spans
        .first()
        .ok_or_else(|| anyhow!("selected row has no spans"))?;
    assert_eq!(kind.style.fg, Some(Color::Black));
    Ok(())
}
