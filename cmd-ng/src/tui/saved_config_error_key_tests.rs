use super::controls::ControlItem;
use super::keyboard_boundary_fixture::{error_model, key, SWITCH_NAME};
use super::pages::ActivePage;
use super::runtime::{process_event, EventDisposition};
use anyhow::Result;
use crossterm::event::KeyCode;
use std::time::Instant;

fn assert_blocked(mut model: super::model::TuiModel, code: KeyCode) -> Result<()> {
    let page = model.active_page;
    let control_idx = model.control_idx;
    let poll = Instant::now();
    let status = model.status.clone();
    let selected = model.saved_config.selected_ids();
    model.last_http_poll = Some(poll);

    let disposition = process_event(&mut model, key(code), Instant::now())?;

    assert_eq!(disposition, EventDisposition::Continue);
    assert_eq!(model.active_page, page);
    assert_eq!(model.control_idx, control_idx);
    assert_eq!(model.last_http_poll, Some(poll));
    assert_eq!(model.status, status);
    assert_eq!(model.saved_config.selected_ids(), selected);
    assert_eq!(model.saved_config.error.as_deref(), Some("storage_error"));
    assert!(model.saved_config.confirmation().is_none());
    assert!(model.saved_config.busy.is_none());
    assert!(model.hardware_confirm.is_none());
    assert!(model.gpio_pending.is_none());
    Ok(())
}

#[test]
fn saved_config_error_blocks_page_refresh_save_and_clear_keys() -> Result<()> {
    for code in [
        KeyCode::Tab,
        KeyCode::Char('c'),
        KeyCode::Char('r'),
        KeyCode::Char('s'),
        KeyCode::Char('x'),
    ] {
        // Given a blocking Saved Config error on the Status page.
        let model = error_model(
            ActivePage::Status,
            ControlItem::Power("12v_out".to_string()),
        )?;

        // When a global page, refresh, save, or clear key is pressed.
        assert_blocked(model, code)?;
    }
    Ok(())
}

#[test]
fn saved_config_error_blocks_power_and_switch_enter() -> Result<()> {
    for selected in [
        ControlItem::Power("12v_out".to_string()),
        ControlItem::Switch(SWITCH_NAME.to_string()),
    ] {
        // Given a blocking error with a hardware control selected.
        let model = error_model(ActivePage::Controls, selected)?;

        // When Enter would normally install a hardware confirmation.
        assert_blocked(model, KeyCode::Enter)?;
    }
    Ok(())
}

#[test]
fn saved_config_error_blocks_all_direct_gpio_keys() -> Result<()> {
    for code in [KeyCode::Char('l'), KeyCode::Char('o'), KeyCode::Char('i')] {
        // Given a blocking error with a GPIO selected.
        let model = error_model(ActivePage::Controls, ControlItem::Gpio("GP13".to_string()))?;

        // When a direct GPIO action key is pressed.
        assert_blocked(model, code)?;
    }
    Ok(())
}

#[test]
fn saved_config_error_escape_redraws_and_q_still_exits() -> Result<()> {
    // Given a blocking error, Esc remains its only dismiss key.
    let mut dismiss = error_model(
        ActivePage::SavedConfig,
        ControlItem::Power("12v_out".to_string()),
    )?;

    // When Esc dismisses it.
    let dismissed = process_event(&mut dismiss, key(KeyCode::Esc), Instant::now())?;

    // Then the error closes behind a redraw boundary.
    assert_eq!(dismissed, EventDisposition::Redraw);
    assert!(dismiss.saved_config.error.is_none());
    assert_eq!(dismiss.active_page, ActivePage::SavedConfig);

    // Given another blocking error, q keeps global exit precedence.
    let mut quit = error_model(
        ActivePage::SavedConfig,
        ControlItem::Power("12v_out".to_string()),
    )?;

    // When q is pressed.
    let exited = process_event(&mut quit, key(KeyCode::Char('q')), Instant::now())?;

    // Then the TUI exits without dismissing or bypassing the error into another action.
    assert_eq!(exited, EventDisposition::Exit);
    assert!(quit.closed);
    assert_eq!(quit.saved_config.error.as_deref(), Some("storage_error"));
    Ok(())
}
