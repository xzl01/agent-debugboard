use super::config_state::ConfigConfirmation;
use super::controls::ControlItem;
use super::hit_types::{HardwareModalTarget, SavedConfigModalTarget, SavedConfigRowTarget};
use super::mouse_fixture::{control_rect, draw_sized, model, power_confirmation};
use super::pages::ActivePage;
use super::runtime::{process_event, EventDisposition};
use crate::persistent_config::ConfigItemId;
use anyhow::{anyhow, Result};
use crossterm::event::{Event, KeyModifiers, MouseButton, MouseEvent, MouseEventKind};
use ratatui::layout::Rect;
use std::time::Instant;

fn mouse(kind: MouseEventKind, rect: Rect) -> Event {
    Event::Mouse(MouseEvent {
        kind,
        column: rect.x,
        row: rect.y,
        modifiers: KeyModifiers::NONE,
    })
}

#[test]
fn hardware_modal_cancel_maps_to_redraw() -> Result<()> {
    // Given: a rendered hardware confirmation.
    let now = Instant::now();
    let mut model = model();
    model.hardware_confirm = Some(power_confirmation());
    draw_sized(&mut model, 80, 8)?;
    let cancel = model
        .hit_map
        .hardware_modal
        .iter()
        .find_map(|(rect, target)| (*target == HardwareModalTarget::cancel()).then_some(*rect))
        .ok_or_else(|| anyhow!("missing hardware Cancel button"))?;

    // When: the Cancel button receives left Down.
    let disposition = process_event(
        &mut model,
        mouse(MouseEventKind::Down(MouseButton::Left), cancel),
        now,
    )?;

    // Then: closing the modal invalidates the rendered geometry.
    assert_eq!(disposition, EventDisposition::Redraw);
    assert!(model.hardware_confirm.is_none());
    Ok(())
}

#[test]
fn saved_config_modal_cancel_maps_to_redraw() -> Result<()> {
    // Given: a rendered Saved Config confirmation.
    let now = Instant::now();
    let mut model = model();
    model.saved_config.confirmation = Some(ConfigConfirmation::Save {
        items: Vec::new(),
        dangerous: Vec::new(),
    });
    draw_sized(&mut model, 80, 8)?;
    let cancel = model
        .hit_map
        .saved_config_modal
        .iter()
        .find_map(|(rect, target)| (*target == SavedConfigModalTarget::cancel()).then_some(*rect))
        .ok_or_else(|| anyhow!("missing Saved Config Cancel button"))?;

    // When: the Cancel button receives left Down.
    let disposition = process_event(
        &mut model,
        mouse(MouseEventKind::Down(MouseButton::Left), cancel),
        now,
    )?;

    // Then: closing the modal invalidates the rendered geometry.
    assert_eq!(disposition, EventDisposition::Redraw);
    assert!(model.saved_config.confirmation().is_none());
    Ok(())
}

#[test]
fn modal_outside_down_remains_continue() -> Result<()> {
    // Given: Saved Config confirmation covers an outside point.
    let now = Instant::now();
    let mut model = model();
    model.saved_config.confirmation = Some(ConfigConfirmation::Save {
        items: Vec::new(),
        dangerous: Vec::new(),
    });
    draw_sized(&mut model, 80, 8)?;

    // When: left Down lands outside the modal.
    let disposition = process_event(
        &mut model,
        mouse(
            MouseEventKind::Down(MouseButton::Left),
            Rect::new(0, 0, 1, 1),
        ),
        now,
    )?;

    // Then: modal precedence consumes it without requesting a redraw.
    assert_eq!(disposition, EventDisposition::Continue);
    assert!(model.saved_config.confirmation().is_some());
    Ok(())
}

#[test]
fn modal_button_up_remains_continue() -> Result<()> {
    // Given: Saved Config confirmation has a rendered button.
    let now = Instant::now();
    let mut model = model();
    model.saved_config.confirmation = Some(ConfigConfirmation::Save {
        items: Vec::new(),
        dangerous: Vec::new(),
    });
    draw_sized(&mut model, 80, 8)?;
    let button = model
        .hit_map
        .saved_config_modal
        .iter()
        .next()
        .map(|(rect, _)| *rect)
        .ok_or_else(|| anyhow!("missing Saved Config modal button"))?;

    // When: the button receives a non-actionable Up event.
    let disposition = process_event(
        &mut model,
        mouse(MouseEventKind::Up(MouseButton::Left), button),
        now,
    )?;

    // Then: the modal stays open without requesting a redraw.
    assert_eq!(disposition, EventDisposition::Continue);
    assert!(model.saved_config.confirmation().is_some());
    Ok(())
}

#[test]
fn controls_click_opening_a_modal_maps_to_redraw() -> Result<()> {
    // Given: a rendered power row with no active modal.
    let now = Instant::now();
    let mut model = model();
    draw_sized(&mut model, 80, 8)?;
    let power = control_rect(&model, &ControlItem::Power("12v_out".to_string()))
        .ok_or_else(|| anyhow!("missing 12v_out row"))?;

    // When: the row receives its primary click.
    let disposition = process_event(
        &mut model,
        mouse(MouseEventKind::Down(MouseButton::Left), power),
        now,
    )?;

    // Then: the modal is rendered before any subsequent ready input is admitted.
    assert_eq!(disposition, EventDisposition::Redraw);
    assert!(model.hardware_confirm.is_some());
    Ok(())
}

#[test]
fn stale_saved_config_row_target_remains_continue() -> Result<()> {
    // Given: a stale Saved Config row ID survives in the old hit map.
    let now = Instant::now();
    let rect = Rect::new(0, 4, 80, 1);
    let mut model = model();
    model.set_page(ActivePage::SavedConfig);
    model.hit_map.saved_config_rows.push(
        rect,
        SavedConfigRowTarget(ConfigItemId("power/missing".to_string())),
    );

    // When: the stale row receives left Down.
    let disposition = process_event(
        &mut model,
        mouse(MouseEventKind::Down(MouseButton::Left), rect),
        now,
    )?;

    // Then: no authoritative row toggles and no redraw is requested.
    assert_eq!(disposition, EventDisposition::Continue);
    assert!(!model.saved_config.focused);
    Ok(())
}
