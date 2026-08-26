use super::confirm::{ConfirmableCommand, HardwareConfirmation};
use super::controls::ControlItem;
use super::gpio_fixture::current_power_outputs;
use super::model::TuiModel;
use super::mouse_events::handle_mouse_at;
use super::render::render_ui;
use crate::client::DEFAULT_BASE_URL;
use crate::ws_status::WsStatusSnapshot;
use anyhow::Result;
use crossterm::event::{KeyModifiers, MouseEvent, MouseEventKind};
use ratatui::backend::TestBackend;
use ratatui::layout::Rect;
use ratatui::Terminal;
use std::time::Duration;

pub(super) fn model() -> TuiModel {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.apply_status_snapshot(WsStatusSnapshot {
        power_outputs: current_power_outputs(),
        ..Default::default()
    });
    model
}

pub(super) fn draw(model: &mut TuiModel) -> Result<()> {
    draw_sized(model, 80, 30)
}

pub(super) fn draw_sized(model: &mut TuiModel, width: u16, height: u16) -> Result<()> {
    let backend = TestBackend::new(width, height);
    let mut terminal = Terminal::new(backend)?;
    terminal.draw(|frame| render_ui(frame, model))?;
    Ok(())
}

pub(super) fn click(
    model: &mut TuiModel,
    kind: MouseEventKind,
    column: u16,
    row: u16,
) -> Result<()> {
    handle_mouse_at(
        model,
        MouseEvent {
            kind,
            column,
            row,
            modifiers: KeyModifiers::NONE,
        },
        std::time::Instant::now(),
    )?;
    Ok(())
}

pub(super) fn control_rect(model: &TuiModel, wanted: &ControlItem) -> Option<Rect> {
    model
        .hit_map
        .controls
        .iter()
        .find(|(_, item)| *item == wanted)
        .map(|(rect, _)| *rect)
}

pub(super) fn power_confirmation() -> HardwareConfirmation {
    HardwareConfirmation::new(ConfirmableCommand::SetPower {
        output: "12v_out".to_string(),
        next_state: true,
    })
}
