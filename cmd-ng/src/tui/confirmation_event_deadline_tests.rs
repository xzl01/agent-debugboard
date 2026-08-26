use super::confirm::{ConfirmableCommand, HardwareConfirmation, CONFIRM_TIMEOUT};
use super::hit_types::HardwareModalTarget;
use super::model::TuiModel;
use super::mouse_fixture::{draw, model};
use super::runtime::{process_event, EventDisposition};
use anyhow::Result;
use crossterm::event::{
    Event, KeyCode, KeyEvent, KeyModifiers, MouseButton, MouseEvent, MouseEventKind,
};
use std::time::{Duration, Instant};

fn confirmation_model(start: Instant) -> TuiModel {
    let mut model = model();
    model.base_url = "http://127.0.0.1:0".to_string();
    model.timeout = Duration::from_millis(10);
    model.hardware_confirm = Some(HardwareConfirmation {
        command: ConfirmableCommand::SetPower {
            output: "12v_out".to_string(),
            next_state: true,
        },
        started: start,
    });
    model
}

fn process_at_deadline(model: &mut TuiModel, event: Event, deadline: Instant) -> Result<()> {
    let disposition = process_event(model, event, deadline).map_err(|error| {
        anyhow::anyhow!("expired confirmation must not dispatch hardware: {error}")
    })?;
    assert_eq!(disposition, EventDisposition::Redraw);
    Ok(())
}

fn assert_timeout_state(model: &TuiModel) {
    assert!(
        model.hardware_confirm.is_none(),
        "the expired confirmation must be cleared"
    );
    assert_eq!(
        model.power_states.get("12v_out"),
        Some(&false),
        "the deadline event must not mutate the power state"
    );
    assert_eq!(model.status, "Power confirmation timed out");
}

#[test]
fn keyboard_enter_at_confirmation_deadline_times_out_without_mutating_power() -> Result<()> {
    let start = Instant::now();
    let deadline = start + CONFIRM_TIMEOUT;
    let mut model = confirmation_model(start);

    process_at_deadline(
        &mut model,
        Event::Key(KeyEvent::new(KeyCode::Enter, KeyModifiers::NONE)),
        deadline,
    )?;

    assert_timeout_state(&model);
    Ok(())
}

#[test]
fn keyboard_space_at_confirmation_deadline_times_out_without_mutating_power() -> Result<()> {
    let start = Instant::now();
    let deadline = start + CONFIRM_TIMEOUT;
    let mut model = confirmation_model(start);

    process_at_deadline(
        &mut model,
        Event::Key(KeyEvent::new(KeyCode::Char(' '), KeyModifiers::NONE)),
        deadline,
    )?;

    assert_timeout_state(&model);
    Ok(())
}

#[test]
fn mouse_confirm_at_confirmation_deadline_times_out_without_mutating_power() -> Result<()> {
    let start = Instant::now();
    let deadline = start + CONFIRM_TIMEOUT;
    let mut model = confirmation_model(start);
    draw(&mut model)?;
    let confirm = model
        .hit_map
        .hardware_modal
        .iter()
        .find_map(|(rect, target)| (*target == HardwareModalTarget::confirm()).then_some(*rect))
        .ok_or_else(|| anyhow::anyhow!("confirmation modal button is missing"))?;

    let disposition = process_event(
        &mut model,
        Event::Mouse(MouseEvent {
            kind: MouseEventKind::Down(MouseButton::Left),
            column: confirm.x,
            row: confirm.y,
            modifiers: KeyModifiers::NONE,
        }),
        deadline,
    )
    .map_err(|error| anyhow::anyhow!("expired confirmation must not dispatch hardware: {error}"))?;

    assert_eq!(disposition, EventDisposition::Redraw);
    assert_timeout_state(&model);
    Ok(())
}
