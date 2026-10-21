use super::controls::{control_items, ControlItem};
use super::gpio_fixture::X1_2_STATUS_JSON;
use super::model::TuiModel;
use super::render::render_ui;
use crate::client::DEFAULT_BASE_URL;
use crate::ws_status::WsStatusSnapshot;
use anyhow::{anyhow, Result};
use ratatui::backend::TestBackend;
use ratatui::buffer::Buffer;
use ratatui::style::Color;
use ratatui::Terminal;
use std::time::{Duration, Instant};

const WIDTH: u16 = 80;
const HEIGHT: u16 = 24;
const SWITCH_NAMES: [&str; 2] = ["sd", "tf_wp"];
const CONTROLS_SEGMENTS: [&str; 7] = [
    "q quit",
    "Tab/Shift+Tab page",
    "Enter/click activate",
    "g GPIO",
    "p pause",
    "r refresh",
    "PgUp/PgDn Move",
];

fn object_names() -> Vec<String> {
    let snapshot: WsStatusSnapshot = serde_json::from_str(X1_2_STATUS_JSON).unwrap();
    let mut names: Vec<String> = snapshot
        .power_outputs
        .iter()
        .map(|output| output.name.clone())
        .collect();
    names.extend(SWITCH_NAMES.iter().map(|name| name.to_string()));
    names.push("Reboot".to_string());
    names.extend(
        snapshot
            .gpios
            .into_iter()
            .map(|gpio| gpio.layout_label.unwrap_or(gpio.name)),
    );
    names
}

fn fixture_model() -> TuiModel {
    let mut snapshot: WsStatusSnapshot = serde_json::from_str(X1_2_STATUS_JSON).unwrap();
    for (index, output) in snapshot.power_outputs.iter_mut().enumerate() {
        output.state = if index % 2 == 0 { "on" } else { "off" }.to_string();
        output.value = (index % 2 == 0) as i32;
    }
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.apply_status_snapshot(snapshot);
    if let Some(state) = model.switches.get_mut("sd") {
        state.desired_route = "usb-reader".to_string();
    }
    if let Some(state) = model.switches.get_mut("tf_wp") {
        state.desired_route = "protected".to_string();
        state.pending_route = Some("protected".to_string());
        state.pending_until = Some(Instant::now() + Duration::from_secs(2));
        state.route_intent_active = true;
    }
    model
}

fn draw(model: &mut TuiModel, width: u16, height: u16) -> Result<Buffer> {
    model.width = width as usize;
    model.height = height as usize;
    let backend = TestBackend::new(width, height);
    let mut terminal = Terminal::new(backend)?;
    terminal.draw(|frame| render_ui(frame, model))?;
    Ok(terminal.backend().buffer().clone())
}

fn row_text(buffer: &Buffer, y: u16) -> String {
    (0..buffer.area.width)
        .map(|x| buffer[(x, y)].symbol())
        .collect()
}

fn find_row(buffer: &Buffer, needle: &str) -> Option<u16> {
    (0..buffer.area.height).find(|y| row_text(buffer, *y).contains(needle))
}

#[test]
fn normal_regions_render_without_box_drawing_borders() -> Result<()> {
    let mut model = fixture_model();
    let buffer = draw(&mut model, WIDTH, HEIGHT)?;
    const BORDER_CHARS: &str = "─│┌┐└┘├┤┬┴┼═║╔╗╚╝╭╮╰╯";
    for y in 0..buffer.area.height {
        let row = row_text(&buffer, y);
        if let Some(found) = row.chars().find(|ch| BORDER_CHARS.contains(*ch)) {
            return Err(anyhow!(
                "border character {found:?} on row {y} outside confirmation modals: {row:?}"
            ));
        }
    }
    Ok(())
}

#[test]
fn telemetry_band_is_zero_height_without_adc_readings() -> Result<()> {
    let mut model = fixture_model();
    let buffer = draw(&mut model, WIDTH, HEIGHT)?;
    let found = find_row(&buffer, "vdd_5v");
    assert_eq!(
        found,
        Some(7),
        "with empty telemetry the 4th object (vdd_5v) must sit on row 7 \
         (status 2 + tabs 1 + header 1); the legacy layout reserves a 7-row \
         sparkline band even without data"
    );
    Ok(())
}

#[test]
fn controls_table_header_lists_type_name_state_route_in_order() -> Result<()> {
    let mut model = fixture_model();
    let buffer = draw(&mut model, WIDTH, HEIGHT)?;
    let header_row = find_row(&buffer, "TYPE")
        .ok_or_else(|| anyhow!("Controls table header with a TYPE column is missing"))?;
    let text = row_text(&buffer, header_row);
    let type_at = text
        .find("TYPE")
        .ok_or_else(|| anyhow!("TYPE missing in header row: {text:?}"))?;
    let name_at = text
        .find("NAME")
        .ok_or_else(|| anyhow!("NAME missing in header row: {text:?}"))?;
    let state_at = text
        .find("STATE-ROUTE")
        .ok_or_else(|| anyhow!("STATE-ROUTE missing in header row: {text:?}"))?;
    assert!(
        type_at < name_at && name_at < state_at,
        "header columns out of order (TYPE@{type_at} NAME@{name_at} STATE-ROUTE@{state_at}): {text:?}"
    );
    Ok(())
}

#[test]
fn object_rows_are_dense_without_separator_rows() -> Result<()> {
    let mut model = fixture_model();
    let buffer = draw(&mut model, WIDTH, HEIGHT)?;
    let names = object_names();
    let first =
        find_row(&buffer, "12v_out").ok_or_else(|| anyhow!("first power object row is missing"))?;
    // Five power + two switch + Reboot + eight projected GPIO rows.
    for y in first..first + 16 {
        let row = row_text(&buffer, y);
        assert!(
            names.iter().any(|name| row.contains(name)),
            "row {y} must render a hardware object; blank separator or section rows are forbidden: {row:?}"
        );
    }
    Ok(())
}

#[test]
fn visible_object_rows_meet_canonical_budgets() -> Result<()> {
    let names = object_names();
    for (width, height) in [(80u16, 24u16), (120, 32)] {
        let mut model = fixture_model();
        let buffer = draw(&mut model, width, height)?;
        let count = (0..buffer.area.height)
            .filter(|y| {
                let row = row_text(&buffer, *y);
                names.iter().any(|name| row.contains(name))
            })
            .count();
        assert_eq!(
            count, 16,
            "eleven GPIOs form eight visual rows at {width}x{height}"
        );
        assert_eq!(control_items(&model).len(), 19);
        assert_eq!(model.power_names.len(), 5);
        assert!(find_row(&buffer, "5v_usb_hub").is_some());
        assert_eq!(
            model
                .switches
                .keys()
                .map(String::as_str)
                .collect::<Vec<_>>(),
            SWITCH_NAMES
        );
        assert_eq!(model.gpio_names.len(), 11);
        for pin in ["GP5", "GP6"] {
            assert!(
                find_row(&buffer, pin).is_some(),
                "missing direct GPIO {pin}"
            );
        }
    }
    Ok(())
}

#[test]
fn selected_single_gpio_row_background_spans_only_its_left_half() -> Result<()> {
    let mut model = fixture_model();
    model.control_idx = control_items(&model)
        .iter()
        .position(|item| item == &ControlItem::Gpio("GP7".to_string()))
        .unwrap();
    let buffer = draw(&mut model, WIDTH, HEIGHT)?;
    let row =
        find_row(&buffer, "MASKROM").ok_or_else(|| anyhow!("selected MASKROM row is missing"))?;
    for x in 0..buffer.area.width / 2 {
        let cell = &buffer[(x, row)];
        assert_eq!(
            cell.bg,
            Color::White,
            "selected cell must paint the White selection background across x=0..{}; \
             x={x} has {:?}",
            buffer.area.width / 2 - 1,
            cell.bg
        );
    }
    for x in buffer.area.width / 2..buffer.area.width {
        assert_ne!(buffer[(x, row)].bg, Color::White);
        assert!(model.hit_map.controls.at(x, row).is_none());
    }
    Ok(())
}

#[test]
fn unfocused_gpio_low_cell_has_no_white_background() -> Result<()> {
    let mut model = fixture_model();
    model.control_idx = control_items(&model)
        .iter()
        .position(|item| item == &ControlItem::Gpio("GP7".to_string()))
        .unwrap();
    let buffer = draw(&mut model, WIDTH, HEIGHT)?;
    let row = find_row(&buffer, "GP13").ok_or_else(|| anyhow!("GP13 row is missing"))?;
    let white_cells = (0..buffer.area.width)
        .filter(|x| buffer[(*x, row)].bg == Color::White)
        .count();
    assert_eq!(
        white_cells, 0,
        "an unfocused GP13 LOW row must not use a White background; \
         the legacy low tone collides with the selection background"
    );
    let marker_cell = (0..buffer.area.width)
        .map(|x| &buffer[(x, row)])
        .find(|cell| cell.symbol() == "◌")
        .ok_or_else(|| anyhow!("GP13 input LOW marker cell is missing on row {row}"))?;
    assert_eq!(
        marker_cell.fg,
        Color::DarkGray,
        "state.gpio.low must be DarkGray on the terminal default background, got fg={:?}",
        marker_cell.fg
    );
    Ok(())
}

#[test]
fn keybar_drops_whole_segments_instead_of_clipping_them() -> Result<()> {
    let mut model = fixture_model();
    let buffer = draw(&mut model, 100, HEIGHT)?;
    let keybar = row_text(&buffer, HEIGHT - 1);
    assert!(keybar.contains("q quit"), "missing keybar: {keybar:?}");
    for segment in keybar
        .trim_end()
        .split("  ")
        .map(str::trim)
        .filter(|segment| !segment.is_empty())
    {
        assert!(
            CONTROLS_SEGMENTS.contains(&segment),
            "keybar segment {segment:?} is partially clipped; segments must render \
             whole or be dropped: {keybar:?}"
        );
    }
    Ok(())
}
