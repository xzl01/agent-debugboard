use super::actions::{cancel_hardware, confirm_hardware};
use super::gpio_mouse_events::handle_controls_mouse_at;
use super::hit_types::{ModalAction, SavedConfigRowTarget, TabTarget};
use super::model::TuiModel;
use super::pages::ActivePage;
use super::runtime::start_config_request;
use anyhow::Result;
use crossterm::event::{MouseButton, MouseEvent, MouseEventKind};
use std::time::Instant;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum MouseOutcome {
    Continue,
    Redraw,
}

pub(super) fn handle_mouse_at(
    model: &mut TuiModel,
    mouse: MouseEvent,
    now: Instant,
) -> Result<MouseOutcome> {
    if model.saved_config.busy.is_some() {
        return Ok(MouseOutcome::Redraw);
    }
    if model.saved_config.confirmation().is_some() {
        model.gpio_gesture.cancel();
        if let MouseEventKind::Down(MouseButton::Left) = mouse.kind {
            if let Some(target) = model
                .hit_map
                .saved_config_modal
                .at(mouse.column, mouse.row)
                .copied()
            {
                match target.action() {
                    ModalAction::Confirm => {
                        if let Some(request) = model.saved_config.confirm() {
                            start_config_request(model, request);
                        }
                    }
                    ModalAction::Cancel => {
                        model.saved_config.cancel_confirmation();
                        model.status = "Saved Config cancelled".to_string();
                    }
                }
            }
        }
        return Ok(if model.saved_config.confirmation().is_none() {
            MouseOutcome::Redraw
        } else {
            MouseOutcome::Continue
        });
    }
    if model.saved_config.error.is_some() {
        model.gpio_gesture.cancel();
        return Ok(MouseOutcome::Continue);
    }
    if model.hardware_confirm.is_some() {
        model.gpio_gesture.cancel();
        if let MouseEventKind::Down(MouseButton::Left) = mouse.kind {
            if let Some(target) = model
                .hit_map
                .hardware_modal
                .at(mouse.column, mouse.row)
                .copied()
            {
                match target.action() {
                    ModalAction::Confirm => confirm_hardware(model, now)?,
                    ModalAction::Cancel => cancel_hardware(model),
                }
            }
        }
        return Ok(if model.hardware_confirm.is_none() {
            MouseOutcome::Redraw
        } else {
            MouseOutcome::Continue
        });
    }
    if let Some(TabTarget(page)) = model.hit_map.tabs.at(mouse.column, mouse.row).copied() {
        if let MouseEventKind::Down(MouseButton::Left) = mouse.kind {
            if page != model.active_page {
                model.set_page(page);
                return Ok(MouseOutcome::Redraw);
            }
            return Ok(MouseOutcome::Continue);
        }
    }

    match model.active_page {
        ActivePage::Controls => {
            handle_controls_mouse_at(model, mouse, now)?;
            Ok(if model.hardware_confirm.is_some() {
                MouseOutcome::Redraw
            } else {
                MouseOutcome::Continue
            })
        }
        ActivePage::SavedConfig => {
            model.gpio_gesture.cancel();
            if let MouseEventKind::Down(MouseButton::Left) = mouse.kind {
                if let Some(SavedConfigRowTarget(id)) = model
                    .hit_map
                    .saved_config_rows
                    .at(mouse.column, mouse.row)
                    .cloned()
                {
                    if let Some(cursor) = model
                        .saved_config
                        .items
                        .iter()
                        .position(|item| item.id == id)
                    {
                        let was_selected = model.saved_config.is_selected(&id);
                        model.saved_config.focus();
                        model.saved_config.cursor = cursor;
                        model.saved_config.toggle_current();
                        if model.saved_config.is_selected(&id) != was_selected {
                            return Ok(MouseOutcome::Redraw);
                        }
                    }
                }
            }
            Ok(MouseOutcome::Continue)
        }
        ActivePage::Status => {
            model.gpio_gesture.cancel();
            Ok(MouseOutcome::Continue)
        }
    }
}
