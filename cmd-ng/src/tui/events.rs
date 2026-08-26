use super::actions::{activate_item, cancel_hardware, confirm_hardware, ControlIntent};
use super::controls::{control_items, first_gpio_index, navigate, ControlItem, Nav};
use super::direct_gpio_key::decode_direct_gpio_intent;
use super::gpio_gesture::GpioGestureAction;
use super::model::TuiModel;
use super::pages::ActivePage;
use super::runtime::{poll_http, start_config_request};
use anyhow::Result;
use crossterm::event::{KeyCode, KeyEvent, KeyEventKind, KeyModifiers};
use std::time::Instant;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum KeyOutcome {
    Continue,
    Redraw,
    Exit,
}

pub(super) fn handle_key(model: &mut TuiModel, key: KeyEvent, now: Instant) -> Result<KeyOutcome> {
    if key.kind != KeyEventKind::Press {
        return Ok(KeyOutcome::Continue);
    }

    match (key.code, key.modifiers) {
        (KeyCode::Char('q'), _) | (KeyCode::Char('c'), KeyModifiers::CONTROL) => {
            model.closed = true;
            return Ok(KeyOutcome::Exit);
        }
        _ => {}
    }

    if model.saved_config.busy.is_some() {
        return Ok(KeyOutcome::Continue);
    }

    if model.saved_config.confirmation().is_some() {
        model.gpio_gesture.cancel();
        return match key.code {
            KeyCode::Enter => {
                if let Some(request) = model.saved_config.confirm() {
                    start_config_request(model, request);
                }
                Ok(KeyOutcome::Redraw)
            }
            KeyCode::Esc => {
                model.saved_config.cancel_confirmation();
                model.status = "Saved Config cancelled".to_string();
                Ok(KeyOutcome::Redraw)
            }
            _ => Ok(KeyOutcome::Continue),
        };
    }

    if model.saved_config.error.is_some() {
        model.gpio_gesture.cancel();
        return if key.code == KeyCode::Esc {
            model.saved_config.dismiss_error();
            model.status = "Saved Config error dismissed".to_string();
            Ok(KeyOutcome::Redraw)
        } else {
            Ok(KeyOutcome::Continue)
        };
    }

    if model.hardware_confirm.is_some() {
        model.gpio_gesture.cancel();
        return match key.code {
            KeyCode::Enter | KeyCode::Char(' ') => {
                confirm_hardware(model, now)?;
                Ok(KeyOutcome::Redraw)
            }
            KeyCode::Esc => {
                cancel_hardware(model);
                Ok(KeyOutcome::Redraw)
            }
            _ => Ok(KeyOutcome::Continue),
        };
    }

    let direct_gpio_intent = decode_direct_gpio_intent(key.code, key.modifiers);
    if direct_gpio_intent.is_some() {
        model.gpio_gesture.cancel();
    }
    let previous_page = model.active_page;
    let previous_gpio_pending = model.gpio_pending.is_some();
    let previous_saved_config_busy = model.saved_config.busy;

    match (key.code, key.modifiers) {
        (KeyCode::Char('p'), _) => {
            model.gpio_gesture.cancel();
            model.paused = !model.paused;
            if model.paused {
                model.status = "Paused".to_string();
            } else {
                model.status = "Resumed".to_string();
            }
        }
        (KeyCode::Char('r'), _) => {
            model.status = "Refreshing…".to_string();
            poll_http(model)?;
            if let Some(request) = model.saved_config.request_refresh() {
                start_config_request(model, request);
            }
            return Ok(KeyOutcome::Redraw);
        }
        (KeyCode::Char('c'), _) => {
            let next = if model.active_page == ActivePage::SavedConfig {
                ActivePage::Controls
            } else {
                ActivePage::SavedConfig
            };
            model.set_page(next);
        }
        (KeyCode::Char('s'), _) => {
            if let Some(request) = model.saved_config.request_save() {
                start_config_request(model, request);
            }
        }
        (KeyCode::Char('x'), _) => {
            if let Some(request) = model.saved_config.request_clear() {
                start_config_request(model, request);
            }
        }
        (KeyCode::Tab, _) => model.next_page(),
        (KeyCode::BackTab, _) => model.prev_page(),
        (KeyCode::Esc, _) if model.active_page == ActivePage::SavedConfig => {
            model.set_page(ActivePage::Controls);
        }
        (KeyCode::Up, _) | (KeyCode::Char('k'), _) => match model.active_page {
            ActivePage::Controls => {
                model.gpio_gesture.cancel();
                model.control_idx = navigate(model, Nav::Up);
            }
            ActivePage::SavedConfig => model.saved_config.move_cursor(-1),
            ActivePage::Status => {
                let scroll = model.page_scroll();
                model.set_page_scroll(scroll.saturating_sub(1));
            }
        },
        (KeyCode::Down, _) | (KeyCode::Char('j'), _) => match model.active_page {
            ActivePage::Controls => {
                model.gpio_gesture.cancel();
                model.control_idx = navigate(model, Nav::Down);
            }
            ActivePage::SavedConfig => model.saved_config.move_cursor(1),
            ActivePage::Status => {
                let scroll = model.page_scroll();
                model.set_page_scroll(scroll.saturating_add(1));
            }
        },
        (KeyCode::Left, _) | (KeyCode::Char('h'), _)
            if model.active_page == ActivePage::Controls =>
        {
            model.gpio_gesture.cancel();
            model.control_idx = navigate(model, Nav::Left);
        }
        (KeyCode::Right, _) if model.active_page == ActivePage::Controls => {
            model.gpio_gesture.cancel();
            model.control_idx = navigate(model, Nav::Right);
        }
        (KeyCode::Enter, _) | (KeyCode::Char(' '), _) => match model.active_page {
            ActivePage::Controls => activate_selection(model, ControlIntent::Primary)?,
            ActivePage::SavedConfig => model.saved_config.toggle_current(),
            ActivePage::Status => {}
        },
        (KeyCode::Char('g'), _) if model.active_page == ActivePage::Controls => {
            model.gpio_gesture.cancel();
            if let Some(idx) = first_gpio_index(model) {
                model.control_idx = idx;
            }
        }
        (KeyCode::Char('u'), KeyModifiers::CONTROL)
        | (KeyCode::PageUp, _)
        | (KeyCode::Char('['), _) => page_step(model, Step::Backward),
        (KeyCode::PageDown, _)
        | (KeyCode::Char('d'), KeyModifiers::CONTROL)
        | (KeyCode::Char(']'), _) => page_step(model, Step::Forward),
        (KeyCode::Esc, _) => model.gpio_gesture.cancel(),
        _ => {}
    }

    if let Some(intent) = direct_gpio_intent {
        if model.active_page == ActivePage::Controls {
            activate_selected_gpio(model, intent)?;
        }
    }
    if model.active_page != previous_page
        || model.gpio_pending.is_some() != previous_gpio_pending
        || model.saved_config.busy != previous_saved_config_busy
        || model.hardware_confirm.is_some()
        || model.saved_config.confirmation().is_some()
        || model.saved_config.error.is_some()
    {
        Ok(KeyOutcome::Redraw)
    } else {
        Ok(KeyOutcome::Continue)
    }
}

enum Step {
    Backward,
    Forward,
}

fn page_step(model: &mut TuiModel, step: Step) {
    model.gpio_gesture.cancel();
    match (model.active_page, step) {
        (ActivePage::Controls, Step::Backward) => {
            model.control_idx = navigate(model, Nav::PageUp);
        }
        (ActivePage::Controls, Step::Forward) => {
            model.control_idx = navigate(model, Nav::PageDown);
        }
        (ActivePage::SavedConfig, Step::Backward) => model.saved_config.move_cursor(-3),
        (ActivePage::SavedConfig, Step::Forward) => model.saved_config.move_cursor(3),
        (ActivePage::Status, Step::Backward) => {
            let scroll = model.page_scroll();
            model.set_page_scroll(scroll.saturating_sub(3));
        }
        (ActivePage::Status, Step::Forward) => {
            let scroll = model.page_scroll();
            model.set_page_scroll(scroll.saturating_add(3));
        }
    }
}

fn activate_selection(model: &mut TuiModel, intent: ControlIntent) -> Result<()> {
    let Some(item) = control_items(model).get(model.control_idx).cloned() else {
        return Ok(());
    };
    activate_item(model, item, intent)
}

pub(super) fn activate_selected_gpio(model: &mut TuiModel, intent: ControlIntent) -> Result<()> {
    model.gpio_gesture.cancel();
    let Some(item @ ControlItem::Gpio(_)) = control_items(model).get(model.control_idx).cloned()
    else {
        return Ok(());
    };
    activate_item(model, item, intent)
}

pub(super) fn activate_gpio_gesture(
    model: &mut TuiModel,
    gesture_action: GpioGestureAction,
) -> Result<()> {
    let intent = match gesture_action.action {
        super::gpio_io::GpioAction::DriveLow => ControlIntent::DriveLow,
        super::gpio_io::GpioAction::DriveHigh => ControlIntent::DriveHigh,
        super::gpio_io::GpioAction::SetInput => ControlIntent::SetInput,
    };
    model.gpio_gesture.cancel();
    activate_item(model, ControlItem::Gpio(gesture_action.pin), intent)
}

pub(super) fn select_item(model: &mut TuiModel, item: &ControlItem) {
    if let Some(idx) = control_items(model)
        .iter()
        .position(|candidate| candidate == item)
    {
        model.control_idx = idx;
    }
}
