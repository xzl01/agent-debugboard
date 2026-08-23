use super::actions::{activate_item, cancel_hardware, confirm_hardware, ControlIntent};
use super::controls::{control_items, first_gpio_index, navigate, ControlItem, Nav};
use super::hit::ModalButton;
use super::model::TuiModel;
use super::pages::ActivePage;
use super::{poll_http, start_config_request};
use anyhow::Result;
use crossterm::event::{
    KeyCode, KeyEvent, KeyEventKind, KeyModifiers, MouseButton, MouseEvent, MouseEventKind,
};

pub(super) fn handle_key(model: &mut TuiModel, key: KeyEvent) -> Result<bool> {
    if key.kind != KeyEventKind::Press {
        return Ok(false);
    }

    match (key.code, key.modifiers) {
        (KeyCode::Char('q'), _) | (KeyCode::Char('c'), KeyModifiers::CONTROL) => {
            model.closed = true;
            return Ok(true);
        }
        _ => {}
    }

    if model.saved_config.confirmation().is_some() {
        match key.code {
            KeyCode::Enter => {
                if let Some(request) = model.saved_config.confirm() {
                    start_config_request(model, request);
                }
            }
            KeyCode::Esc => {
                model.saved_config.cancel_confirmation();
                model.status = "Saved Config cancelled".to_string();
            }
            _ => {}
        }
        return Ok(false);
    }

    if key.code == KeyCode::Esc && model.saved_config.error.is_some() {
        model.saved_config.dismiss_error();
        model.status = "Saved Config error dismissed".to_string();
        return Ok(false);
    }

    if model.hardware_confirm.is_some() {
        match key.code {
            KeyCode::Enter | KeyCode::Char(' ') => confirm_hardware(model)?,
            KeyCode::Esc => cancel_hardware(model),
            _ => {}
        }
        return Ok(false);
    }

    match (key.code, key.modifiers) {
        (KeyCode::Char('p'), _) => {
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
            ActivePage::Controls => model.control_idx = navigate(model, Nav::Up),
            ActivePage::SavedConfig => model.saved_config.move_cursor(-1),
            ActivePage::Status => {
                let scroll = model.page_scroll();
                model.set_page_scroll(scroll.saturating_sub(1));
            }
        },
        (KeyCode::Down, _) | (KeyCode::Char('j'), _) => match model.active_page {
            ActivePage::Controls => model.control_idx = navigate(model, Nav::Down),
            ActivePage::SavedConfig => model.saved_config.move_cursor(1),
            ActivePage::Status => {
                let scroll = model.page_scroll();
                model.set_page_scroll(scroll.saturating_add(1));
            }
        },
        (KeyCode::Left, _) | (KeyCode::Char('h'), _)
            if model.active_page == ActivePage::Controls =>
        {
            model.control_idx = navigate(model, Nav::Left);
        }
        (KeyCode::Right, _) | (KeyCode::Char('l'), _)
            if model.active_page == ActivePage::Controls =>
        {
            model.control_idx = navigate(model, Nav::Right);
        }
        (KeyCode::Enter, _) | (KeyCode::Char(' '), _) => match model.active_page {
            ActivePage::Controls => activate_selection(model, ControlIntent::Primary)?,
            ActivePage::SavedConfig => model.saved_config.toggle_current(),
            ActivePage::Status => {}
        },
        (KeyCode::Char('g'), _) if model.active_page == ActivePage::Controls => {
            if let Some(idx) = first_gpio_index(model) {
                model.control_idx = idx;
            }
        }
        (KeyCode::Char('i'), _) if model.active_page == ActivePage::Controls => {
            activate_selection(model, ControlIntent::RestoreInput)?;
        }
        (KeyCode::Char('u'), KeyModifiers::CONTROL)
        | (KeyCode::PageUp, _)
        | (KeyCode::Char('['), _) => page_step(model, Step::Backward),
        (KeyCode::PageDown, _)
        | (KeyCode::Char('d'), KeyModifiers::CONTROL)
        | (KeyCode::Char(']'), _) => page_step(model, Step::Forward),
        _ => {}
    }
    Ok(false)
}

enum Step {
    Backward,
    Forward,
}

fn page_step(model: &mut TuiModel, step: Step) {
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

pub(super) fn handle_mouse(model: &mut TuiModel, mouse: MouseEvent) -> Result<()> {
    match mouse.kind {
        MouseEventKind::Down(MouseButton::Left) => {
            if model.hardware_confirm.is_some() {
                match model.hit_map.modal_button_at(mouse.column, mouse.row) {
                    Some(ModalButton::Confirm) => confirm_hardware(model)?,
                    Some(ModalButton::Cancel) => cancel_hardware(model),
                    None => {}
                }
                return Ok(());
            }
            if model.saved_config.confirmation().is_some() {
                return Ok(());
            }
            if let Some(item) = model.hit_map.control_at(mouse.column, mouse.row).cloned() {
                select_item(model, &item);
                activate_item(model, item, ControlIntent::Primary)?;
            }
        }
        MouseEventKind::Down(MouseButton::Right) => {
            if model.hardware_confirm.is_some() || model.saved_config.confirmation().is_some() {
                return Ok(());
            }
            if let Some(item @ ControlItem::Gpio(_)) =
                model.hit_map.control_at(mouse.column, mouse.row).cloned()
            {
                select_item(model, &item);
                activate_item(model, item, ControlIntent::RestoreInput)?;
            }
        }
        _ => {}
    }
    Ok(())
}

fn select_item(model: &mut TuiModel, item: &ControlItem) {
    if let Some(idx) = control_items(model)
        .iter()
        .position(|candidate| candidate == item)
    {
        model.control_idx = idx;
    }
}
