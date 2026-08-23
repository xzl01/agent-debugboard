use super::confirm::{ConfirmableCommand, HardwareConfirmation};
use super::model::TuiModel;
use super::render::render_ui;
use super::render_modal::modal_area;
use crate::client::DEFAULT_BASE_URL;
use anyhow::{anyhow, Result};
use ratatui::backend::TestBackend;
use ratatui::buffer::Buffer;
use ratatui::layout::Rect;
use ratatui::style::Color;
use ratatui::Terminal;
use std::time::Duration;

fn model() -> TuiModel {
    TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2))
}

fn draw(model: &mut TuiModel, width: u16, height: u16) -> Result<Buffer> {
    let backend = TestBackend::new(width, height);
    let mut terminal = Terminal::new(backend)?;
    terminal.draw(|frame| render_ui(frame, model))?;
    Ok(terminal.backend().buffer().clone())
}

fn buffer_text(buffer: &Buffer) -> String {
    let mut text = String::new();
    for y in 0..buffer.area.height {
        for x in 0..buffer.area.width {
            text.push_str(buffer[(x, y)].symbol());
        }
        text.push('\n');
    }
    text
}

fn power_confirmation() -> HardwareConfirmation {
    HardwareConfirmation::new(ConfirmableCommand::SetPower {
        output: "12v_out".to_string(),
        next_state: true,
    })
}

#[test]
fn hardware_confirmation_modal_is_centered_red_and_names_the_target() -> Result<()> {
    let mut model = model();
    model.hardware_confirm = Some(power_confirmation());
    let buffer = draw(&mut model, 80, 24)?;
    let text = buffer_text(&buffer);

    assert!(text.contains("Confirm Power Toggle"), "modal title missing");
    assert!(
        text.contains("power 12v_out: off -> on"),
        "modal target missing"
    );
    assert!(text.contains("[ Confirm ]"), "confirm hit target missing");
    assert!(text.contains("[ Cancel ]"), "cancel hit target missing");

    let area = modal_area(Rect::new(0, 0, 80, 24))
        .ok_or_else(|| anyhow!("confirmation modal area is missing"))?;
    assert_eq!(
        buffer[(area.x, area.y)].fg,
        Color::Red,
        "border must be red"
    );
    assert!(model.hit_map.confirm_button.is_some());
    assert!(model.hit_map.cancel_button.is_some());
    Ok(())
}

#[test]
fn hardware_confirmation_modal_survives_compact_terminals() -> Result<()> {
    let mut model = model();
    model.hardware_confirm = Some(power_confirmation());
    let buffer = draw(&mut model, 24, 8)?;
    assert_eq!(buffer.area.width, 24);
    assert_eq!(buffer.area.height, 8);
    Ok(())
}
