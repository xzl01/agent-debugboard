use super::controls::ControlItem;
use super::gpio_fixture::{current_power_outputs, draw};
use super::model::TuiModel;
use super::mouse_events::handle_mouse_at;
use super::mouse_fixture::control_rect;
use super::render_modal::modal_area;
use crate::ws_status::{TuiStatusSwitchInfo, WsStatusSnapshot};
use anyhow::{anyhow, Result};
use crossterm::event::{KeyModifiers, MouseButton, MouseEvent, MouseEventKind};
use ratatui::layout::Rect;
use ratatui::style::{Color, Modifier};
use std::time::{Duration, Instant};

const SWITCH_NAME: &str = "future_switch";

fn model() -> TuiModel {
    let mut snapshot = WsStatusSnapshot {
        power_outputs: current_power_outputs(),
        ..Default::default()
    };
    snapshot.switches.insert(
        SWITCH_NAME.to_string(),
        TuiStatusSwitchInfo {
            route: "route-a".to_string(),
            routes: vec!["route-a".to_string(), "route-b".to_string()],
            requires_confirm: false,
        },
    );
    let mut model = TuiModel::new("http://127.0.0.1:9".to_string(), Duration::from_millis(30));
    model.apply_status_snapshot(snapshot);
    model
}

#[test]
fn clicked_switch_row_keeps_full_width_accent_select_beneath_modal_at_80x24() -> Result<()> {
    let mut model = model();
    draw(&mut model, 80, 24)?;
    let item = ControlItem::Switch(SWITCH_NAME.to_string());
    let rect = control_rect(&model, &item).ok_or_else(|| anyhow!("missing switch row hit"))?;

    handle_mouse_at(
        &mut model,
        MouseEvent {
            kind: MouseEventKind::Down(MouseButton::Left),
            column: rect.x,
            row: rect.y,
            modifiers: KeyModifiers::NONE,
        },
        Instant::now(),
    )?;
    let buffer = draw(&mut model, 80, 24)?;
    let modal = modal_area(Rect::new(0, 0, 80, 24))
        .ok_or_else(|| anyhow!("missing hardware modal area"))?;

    assert!(model.hardware_confirm.is_some());
    assert_eq!(rect.width, 80);
    let mut visible_cells = 0;
    for x in rect.x..rect.x + rect.width {
        let covered = rect.y >= modal.y
            && rect.y < modal.y + modal.height
            && x >= modal.x
            && x < modal.x + modal.width;
        if covered {
            continue;
        }
        visible_cells += 1;
        let cell = &buffer[(x, rect.y)];
        assert_eq!(cell.bg, Color::White, "x={x} lost accent.select bg");
        assert!(
            cell.modifier.contains(Modifier::BOLD),
            "x={x} lost accent.select emphasis"
        );
    }
    assert_eq!(visible_cells, 16);
    Ok(())
}
