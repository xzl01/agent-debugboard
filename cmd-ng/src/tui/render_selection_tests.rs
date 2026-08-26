use super::controls::ControlItem;
use super::gpio_fixture::{current_power_outputs, draw};
use super::model::TuiModel;
use crate::client::DEFAULT_BASE_URL;
use crate::ws_status::WsStatusSnapshot;
use anyhow::{anyhow, Result};
use ratatui::layout::Rect;
use ratatui::style::Color;
use std::time::Duration;

fn control_rect(model: &TuiModel, wanted: &ControlItem) -> Option<Rect> {
    model
        .hit_map
        .controls
        .iter()
        .find(|(_, item)| *item == wanted)
        .map(|(rect, _)| *rect)
}

#[test]
fn selected_power_row_keeps_its_state_color_in_the_buffer() -> Result<()> {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.apply_status_snapshot(WsStatusSnapshot {
        power_outputs: current_power_outputs(),
        ..Default::default()
    });
    model.power_states.insert("12v_out".to_string(), true);
    model.control_idx = 0;
    let buffer = draw(&mut model, 80, 30)?;

    let rect = control_rect(&model, &ControlItem::Power("12v_out".to_string()))
        .ok_or_else(|| anyhow!("12v_out row is missing"))?;
    for x in rect.x..rect.x + rect.width {
        assert_eq!(
            buffer[(x, rect.y)].bg,
            Color::White,
            "selected row must paint White bg across the full hit width (x={x})"
        );
    }
    let state_cell = (rect.x..rect.x + rect.width)
        .map(|x| &buffer[(x, rect.y)])
        .find(|cell| cell.symbol() == "n")
        .ok_or_else(|| anyhow!("selected ON state cell is missing"))?;
    assert_eq!(
        state_cell.fg,
        Color::Green,
        "selected ON row keeps green fg"
    );
    Ok(())
}
