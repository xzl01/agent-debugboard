use super::confirm::{ConfirmableCommand, HardwareConfirmation};
use super::controls::{control_targets, ControlItem};
use super::events::handle_mouse;
use super::gpio_fixture::current_power_outputs;
use super::model::TuiModel;
use super::render::render_ui;
use crate::client::DEFAULT_BASE_URL;
use crate::ws_status::WsStatusSnapshot;
use anyhow::{anyhow, Result};
use crossterm::event::{KeyModifiers, MouseButton, MouseEvent, MouseEventKind};
use ratatui::backend::TestBackend;
use ratatui::layout::Rect;
use ratatui::Terminal;
use std::time::Duration;

fn model() -> TuiModel {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.apply_status_snapshot(WsStatusSnapshot {
        power_outputs: current_power_outputs(),
        ..Default::default()
    });
    model
}

fn draw(model: &mut TuiModel) -> Result<()> {
    draw_sized(model, 80, 30)
}

fn draw_sized(model: &mut TuiModel, width: u16, height: u16) -> Result<()> {
    let backend = TestBackend::new(width, height);
    let mut terminal = Terminal::new(backend)?;
    terminal.draw(|frame| render_ui(frame, model))?;
    Ok(())
}

fn click(model: &mut TuiModel, kind: MouseEventKind, column: u16, row: u16) -> Result<()> {
    handle_mouse(
        model,
        MouseEvent {
            kind,
            column,
            row,
            modifiers: KeyModifiers::NONE,
        },
    )?;
    Ok(())
}

fn control_rect(model: &TuiModel, wanted: &ControlItem) -> Option<Rect> {
    model
        .hit_map
        .controls
        .iter()
        .find(|(_, item)| item == wanted)
        .map(|(rect, _)| *rect)
}

fn power_confirmation() -> HardwareConfirmation {
    HardwareConfirmation::new(ConfirmableCommand::SetPower {
        output: "12v_out".to_string(),
        next_state: true,
    })
}

#[test]
fn left_click_on_power_row_opens_confirmation_without_http() -> Result<()> {
    let mut model = model();
    draw(&mut model)?;
    let rect = control_rect(&model, &ControlItem::Power("12v_out".to_string()))
        .ok_or_else(|| anyhow!("12v_out row is missing"))?;

    click(
        &mut model,
        MouseEventKind::Down(MouseButton::Left),
        rect.x,
        rect.y,
    )?;

    assert_eq!(
        model
            .hardware_confirm
            .as_ref()
            .map(|confirm| &confirm.command),
        Some(&ConfirmableCommand::SetPower {
            output: "12v_out".to_string(),
            next_state: true,
        })
    );
    assert_eq!(model.power_states.get("12v_out"), Some(&false));
    Ok(())
}

#[test]
fn cancel_button_click_cancels_the_pending_confirmation() -> Result<()> {
    let mut model = model();
    model.hardware_confirm = Some(power_confirmation());
    draw(&mut model)?;
    let cancel = model
        .hit_map
        .cancel_button
        .ok_or_else(|| anyhow!("cancel button is missing"))?;

    click(
        &mut model,
        MouseEventKind::Down(MouseButton::Left),
        cancel.x,
        cancel.y,
    )?;

    assert!(model.hardware_confirm.is_none());
    assert_eq!(model.status, "Power toggle cancelled");
    Ok(())
}

#[test]
fn clicks_outside_hit_targets_are_ignored() -> Result<()> {
    let mut model = model();
    draw(&mut model)?;

    // Footer row has no control hit targets.
    click(&mut model, MouseEventKind::Down(MouseButton::Left), 0, 29)?;
    assert_eq!(model.status, "HTTP mode");
    assert_eq!(model.control_idx, 0);
    assert!(model.hardware_confirm.is_none());

    // Right-click outside GPIO rows is ignored.
    let power = control_rect(&model, &ControlItem::Power("12v_out".to_string()))
        .ok_or_else(|| anyhow!("12v_out row is missing"))?;
    click(
        &mut model,
        MouseEventKind::Down(MouseButton::Right),
        power.x,
        power.y,
    )?;
    assert!(model.hardware_confirm.is_none());
    assert_eq!(model.control_idx, 0);

    // Mouse release never activates.
    let power = control_rect(&model, &ControlItem::Power("12v_out".to_string()))
        .ok_or_else(|| anyhow!("12v_out row is missing"))?;
    click(
        &mut model,
        MouseEventKind::Up(MouseButton::Left),
        power.x,
        power.y,
    )?;
    assert!(model.hardware_confirm.is_none());
    Ok(())
}

#[test]
fn modal_open_blocks_clicks_on_underlying_controls() -> Result<()> {
    let mut model = model();
    model.hardware_confirm = Some(power_confirmation());
    draw(&mut model)?;
    let power = control_rect(&model, &ControlItem::Power("5v_out".to_string()))
        .ok_or_else(|| anyhow!("5v_out row is missing"))?;

    click(
        &mut model,
        MouseEventKind::Down(MouseButton::Left),
        power.x,
        power.y,
    )?;

    assert!(model.hardware_confirm.is_some());
    Ok(())
}

#[test]
fn control_hit_rects_span_the_full_body_width() -> Result<()> {
    let mut model = model();
    draw(&mut model)?;
    let rect = control_rect(&model, &ControlItem::Power("12v_out".to_string()))
        .ok_or_else(|| anyhow!("12v_out row is missing"))?;
    assert_eq!(rect.width, 80, "hit rect must span the full body width");
    let far_edge = rect.x + rect.width - 1;

    click(
        &mut model,
        MouseEventKind::Down(MouseButton::Left),
        far_edge,
        rect.y,
    )?;

    assert_eq!(
        model
            .hardware_confirm
            .as_ref()
            .map(|confirm| &confirm.command),
        Some(&ConfirmableCommand::SetPower {
            output: "12v_out".to_string(),
            next_state: true,
        })
    );
    Ok(())
}

#[test]
fn hit_rects_follow_the_selection_driven_scroll_offset() -> Result<()> {
    let mut model = model();
    for pin in 7..=20 {
        let name = format!("GP{pin}");
        model.gpio_names.push(name.clone());
        model
            .gpio_notes
            .insert(name.clone(), format!("J16_PIN{pin}"));
        model.gpio_levels.insert(name.clone(), false);
        model.gpio_is_input.insert(name, true);
    }
    // Select the last GPIO so the selection-driven scroll pushes power rows out.
    model.control_idx = control_targets(&model).len() + model.gpio_names.len() - 1;
    draw_sized(&mut model, 80, 16)?;

    assert!(
        control_rect(&model, &ControlItem::Power("12v_out".to_string())).is_none(),
        "scrolled-out power rows must not keep hit rects"
    );
    assert!(
        control_rect(&model, &ControlItem::Gpio("GP7".to_string())).is_none(),
        "rows scrolled above the viewport must not keep hit rects"
    );
    assert!(control_rect(&model, &ControlItem::Gpio("GP20".to_string())).is_some());
    Ok(())
}
