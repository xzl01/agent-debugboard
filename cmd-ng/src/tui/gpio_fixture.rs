use super::controls::{control_items, ControlItem};
use super::model::TuiModel;
use super::render::render_ui;
use super::render_body::build_body_content;
use crate::client::DEFAULT_BASE_URL;
use crate::ws_status::{TuiStatusAdcChannel, TuiStatusPowerOutput, WsStatusSnapshot};
use anyhow::{anyhow, Result};
use ratatui::backend::TestBackend;
use ratatui::buffer::Buffer;
use ratatui::layout::Rect;
use ratatui::Terminal;
use std::time::Duration;

// Synthetic metadata stress fixture, not a physical board pin map.
// Shuffled snapshot order on purpose: GP10 (J16 row5) first, GP15 (J16 row0)
// late, GP20 with no layout metadata last. J16 row0 holds a single entry, so
// nothing may pair with it from another firmware row.
pub(super) const STATUS_JSON: &str = r#"{"gpios":[
  {"name":"GP10","pin":10,"value":1,"direction":"output","note":"J16_PIN1","layoutGroup":"J16","layoutLabel":"GP10","layoutRow":5,"layoutColumn":0},
  {"name":"GP8","pin":8,"value":0,"direction":"input","note":"CON_REST","layoutGroup":"J13","layoutLabel":"RSET","layoutRow":0,"layoutColumn":0},
  {"name":"GP9","pin":9,"value":1,"direction":"input","note":"CON_USER","layoutGroup":"J13","layoutLabel":"USER","layoutRow":0,"layoutColumn":1},
  {"name":"GP16","pin":16,"value":0,"direction":"output","note":"J16_PIN2","layoutGroup":"J16","layoutLabel":"GP16","layoutRow":5,"layoutColumn":1},
  {"name":"GP15","pin":15,"value":1,"direction":"output","note":"J16_PIN11","layoutGroup":"J16","layoutLabel":"GP15","layoutRow":0,"layoutColumn":0},
  {"name":"GP20","pin":20,"value":0,"direction":"input","note":"J16_PIN10"}
]}"#;

// Synthetic paired groups keep generic rendering and navigation tests separate
// from the real x1.2 pin map below. Snapshot and catalog order are reversed.
pub(super) const GENERIC_EDGE_STATUS_JSON: &str = r#"{
  "gpio_layout_groups":[
    {"name":"KEY","edge":"right","order":1},
    {"name":"IO","edge":"right","order":0}
  ],
  "gpios":[
    {"name":"GP8","pin":8,"value":0,"direction":"input","note":"CON_REST","layoutGroup":"KEY","layoutLabel":"RSET","layoutRow":0,"layoutColumn":0},
    {"name":"GP9","pin":9,"value":1,"direction":"input","note":"CON_USER","layoutGroup":"KEY","layoutLabel":"USER","layoutRow":0,"layoutColumn":1},
    {"name":"GP10","pin":10,"value":1,"direction":"output","note":"HEADER_PIN1","layoutGroup":"IO","layoutLabel":"GP10","layoutRow":0,"layoutColumn":0},
    {"name":"GP16","pin":16,"value":0,"direction":"output","note":"HEADER_PIN2","layoutGroup":"IO","layoutLabel":"GP16","layoutRow":0,"layoutColumn":1}
  ]
}"#;

// User-confirmed x1.2 hardware. J8 pins 1..6 are GP12..GP16/GP29;
// GP17/GP18 remain safe GPIOs but are not J8 pins. J13 pin 4 is GND,
// not a GPIO cell. Deliberately shuffle GPIOs and list J13 metadata first.
pub(super) const X1_2_STATUS_JSON: &str = r#"{
  "gpio_layout_groups":[
    {"name":"J13","edge":"right","order":1},
    {"name":"J8","edge":"right","order":0}
  ],
  "gpios":[
    {"name":"GP8","pin":8,"value":0,"direction":"input","note":"CON_REST","layoutGroup":"J13","layoutLabel":"RSET","layoutRow":0,"layoutColumn":1},
    {"name":"GP17","pin":17,"value":0,"direction":"input","note":"","layoutGroup":""},
    {"name":"GP12","pin":12,"value":1,"direction":"output","note":"J8_PIN1","layoutGroup":"J8","layoutLabel":"GP12","layoutRow":9,"layoutColumn":0},
    {"name":"GP9","pin":9,"value":1,"direction":"input","note":"CON_USER","layoutGroup":"J13","layoutLabel":"USER","layoutRow":0,"layoutColumn":0},
    {"name":"GP13","pin":13,"value":0,"direction":"input","note":"J8_PIN2","layoutGroup":"J8","layoutLabel":"GP13","layoutRow":8,"layoutColumn":0},
    {"name":"GP7","pin":7,"value":0,"direction":"input","note":"CON_MAS","layoutGroup":"J13","layoutLabel":"MASKROM","layoutRow":1,"layoutColumn":0},
    {"name":"GP14","pin":14,"value":0,"direction":"input","note":"J8_PIN3","layoutGroup":"J8","layoutLabel":"GP14","layoutRow":7,"layoutColumn":0},
    {"name":"GP18","pin":18,"value":0,"direction":"input","note":"","layoutGroup":""},
    {"name":"GP15","pin":15,"value":0,"direction":"input","note":"J8_PIN4","layoutGroup":"J8","layoutLabel":"GP15","layoutRow":6,"layoutColumn":0},
    {"name":"GP16","pin":16,"value":0,"direction":"input","note":"J8_PIN5","layoutGroup":"J8","layoutLabel":"GP16","layoutRow":5,"layoutColumn":0},
    {"name":"GP29","pin":29,"value":0,"direction":"input","note":"ADC3","layoutGroup":"J8","layoutLabel":"ADC3","layoutRow":4,"layoutColumn":0}
  ]
}"#;

pub(super) fn current_power_outputs() -> Vec<TuiStatusPowerOutput> {
    ["12v_out", "5v_out", "20v_out", "vdd_5v"]
        .into_iter()
        .map(|name| TuiStatusPowerOutput {
            name: name.to_string(),
            state: "off".to_string(),
            value: 0,
        })
        .collect()
}

pub(super) fn current_adc_channels() -> Vec<TuiStatusAdcChannel> {
    ["5v_out", "12v_out", "20v_out", "vdd_5v"]
        .into_iter()
        .map(|name| TuiStatusAdcChannel {
            name: name.to_string(),
            sensor_channel: "current".to_string(),
        })
        .collect()
}

pub(super) fn projection_model() -> Result<TuiModel> {
    let snapshot: WsStatusSnapshot =
        serde_json::from_str(STATUS_JSON).map_err(anyhow::Error::msg)?;
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.width = 80;
    model.apply_status_snapshot(snapshot);
    Ok(model)
}

pub(super) fn generic_edge_model() -> Result<TuiModel> {
    let snapshot: WsStatusSnapshot = serde_json::from_str(GENERIC_EDGE_STATUS_JSON)?;
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.width = 80;
    model.apply_status_snapshot(snapshot);
    Ok(model)
}

pub(super) fn x1_2_model() -> Result<TuiModel> {
    let snapshot: WsStatusSnapshot = serde_json::from_str(X1_2_STATUS_JSON)?;
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.width = 80;
    model.apply_status_snapshot(snapshot);
    Ok(model)
}

pub(super) fn draw(model: &mut TuiModel, width: u16, height: u16) -> Result<Buffer> {
    let backend = TestBackend::new(width, height);
    let mut terminal = Terminal::new(backend)?;
    terminal.draw(|frame| render_ui(frame, model))?;
    Ok(terminal.backend().buffer().clone())
}

pub(super) fn row_text(buffer: &Buffer, y: u16) -> String {
    (0..buffer.area.width)
        .map(|x| buffer[(x, y)].symbol())
        .collect()
}

pub(super) fn find_row(buffer: &Buffer, needle: &str) -> Result<u16> {
    (0..buffer.area.height)
        .find(|y| row_text(buffer, *y).contains(needle))
        .ok_or_else(|| anyhow!("no buffer row contains {needle:?}"))
}

pub(super) fn body_line(model: &TuiModel, width: usize, needle: &str) -> Result<String> {
    build_body_content(model, width)
        .lines
        .iter()
        .map(ToString::to_string)
        .find(|line| line.contains(needle))
        .ok_or_else(|| anyhow!("no body line contains {needle:?} at width {width}"))
}

pub(super) fn control_rect(model: &TuiModel, gpio: &str) -> Result<Rect> {
    let wanted = ControlItem::Gpio(gpio.to_string());
    model
        .hit_map
        .controls
        .iter()
        .find(|(_, item)| **item == wanted)
        .map(|(rect, _)| *rect)
        .ok_or_else(|| anyhow!("no hit rect for {gpio}"))
}

pub(super) fn item_index(model: &TuiModel, gpio: &str) -> Result<usize> {
    let wanted = ControlItem::Gpio(gpio.to_string());
    control_items(model)
        .iter()
        .position(|item| *item == wanted)
        .ok_or_else(|| anyhow!("{gpio} missing from control_items"))
}
