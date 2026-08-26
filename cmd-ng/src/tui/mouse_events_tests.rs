use super::config_state::ConfigConfirmation;
use super::controls::ControlItem;
use super::hit_types::{HardwareModalTarget, TabTarget};
use super::mouse_events::handle_mouse_at;
use super::mouse_fixture::{control_rect, draw, model, power_confirmation};
use super::pages::ActivePage;
use anyhow::Result;
use crossterm::event::{KeyModifiers, MouseButton, MouseEvent, MouseEventKind};
use ratatui::layout::Rect;
use std::time::Instant;

fn left_down(rect: Rect) -> MouseEvent {
    MouseEvent {
        kind: MouseEventKind::Down(MouseButton::Left),
        column: rect.x,
        row: rect.y,
        modifiers: KeyModifiers::NONE,
    }
}

#[test]
fn saved_config_confirmation_precedes_hardware_modal_mouse_input() -> Result<()> {
    let rect = Rect::new(2, 3, 4, 1);
    let mut model = model();
    model.hardware_confirm = Some(power_confirmation());
    model.saved_config.confirmation = Some(ConfigConfirmation::Save {
        items: Vec::new(),
        dangerous: Vec::new(),
    });
    model
        .hit_map
        .hardware_modal
        .push(rect, HardwareModalTarget::cancel());

    handle_mouse_at(&mut model, left_down(rect), Instant::now())?;

    assert!(model.saved_config.confirmation().is_some());
    assert!(model.hardware_confirm.is_some());
    Ok(())
}

#[test]
fn saved_config_error_precedes_hardware_modal_mouse_input() -> Result<()> {
    let rect = Rect::new(2, 3, 4, 1);
    let mut model = model();
    model.hardware_confirm = Some(power_confirmation());
    model.saved_config.error = Some("failure".to_string());
    model
        .hit_map
        .hardware_modal
        .push(rect, HardwareModalTarget::cancel());

    handle_mouse_at(&mut model, left_down(rect), Instant::now())?;

    assert_eq!(model.saved_config.error.as_deref(), Some("failure"));
    assert!(model.hardware_confirm.is_some());
    Ok(())
}

#[test]
fn hardware_modal_mouse_input_precedes_page_routing() -> Result<()> {
    let rect = Rect::new(2, 3, 4, 1);
    let mut model = model();
    model.set_page(ActivePage::SavedConfig);
    model.hardware_confirm = Some(power_confirmation());
    model
        .hit_map
        .hardware_modal
        .push(rect, HardwareModalTarget::cancel());

    handle_mouse_at(&mut model, left_down(rect), Instant::now())?;

    assert!(model.hardware_confirm.is_none());
    assert_eq!(model.active_page, ActivePage::SavedConfig);
    Ok(())
}

#[test]
fn tab_hit_precedes_controls_and_switches_pages() -> Result<()> {
    let mut model = model();
    draw(&mut model)?;
    let power =
        control_rect(&model, &ControlItem::Power("12v_out".to_string())).expect("power hit region");
    model
        .hit_map
        .tabs
        .push(power, TabTarget(ActivePage::SavedConfig));

    handle_mouse_at(&mut model, left_down(power), Instant::now())?;

    assert_eq!(model.active_page, ActivePage::SavedConfig);
    assert!(model.hardware_confirm.is_none());
    Ok(())
}
