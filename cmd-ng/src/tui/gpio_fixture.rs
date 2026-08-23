use super::controls::{control_items, ControlItem};
use super::model::TuiModel;
use super::render::render_ui;
use crate::client::DEFAULT_BASE_URL;
use crate::ws_status::{TuiStatusPowerOutput, WsStatusSnapshot};
use anyhow::{anyhow, Result};
use ratatui::backend::TestBackend;
use ratatui::buffer::Buffer;
use ratatui::layout::Rect;
use ratatui::Terminal;
use std::time::Duration;

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

pub(super) fn projection_model() -> Result<TuiModel> {
    let snapshot: WsStatusSnapshot =
        serde_json::from_str(STATUS_JSON).map_err(anyhow::Error::msg)?;
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

pub(super) fn control_rect(model: &TuiModel, gpio: &str) -> Result<Rect> {
    let wanted = ControlItem::Gpio(gpio.to_string());
    model
        .hit_map
        .controls
        .iter()
        .find(|(_, item)| *item == wanted)
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
