use super::confirm::{ConfirmableCommand, HardwareConfirmation};
use super::model::TuiModel;
use super::render::render_ui;
use crate::adc::AdcReading;
use crate::client::DEFAULT_BASE_URL;
use anyhow::Result;
use ratatui::backend::TestBackend;
use ratatui::buffer::Buffer;
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

fn row_text(buffer: &Buffer, y: u16) -> String {
    (0..buffer.area.width)
        .map(|x| buffer[(x, y)].symbol())
        .collect()
}

fn power_confirmation() -> HardwareConfirmation {
    HardwareConfirmation::new(ConfirmableCommand::SetPower {
        output: "12v_out".to_string(),
        next_state: true,
    })
}

#[test]
fn status_band_shows_title_url_and_status_without_key_help() -> Result<()> {
    let mut model = model();
    let buffer = draw(&mut model, 80, 24)?;
    let band = format!("{}{}", row_text(&buffer, 0), row_text(&buffer, 1));
    assert!(band.contains("Radxa Linkr Debugger TUI"), "band={band}");
    assert!(band.contains(DEFAULT_BASE_URL), "band={band}");
    assert!(band.contains("HTTP mode"), "band={band}");
    assert!(!band.contains("keys:"), "band must not carry key help");
    Ok(())
}

#[test]
fn status_band_marks_paused_polling() -> Result<()> {
    let mut model = model();
    model.paused = true;
    let buffer = draw(&mut model, 80, 24)?;
    assert!(row_text(&buffer, 0).contains("paused"));
    Ok(())
}

#[test]
fn scope_band_shows_channel_power_current_scale_and_stays_borderless() -> Result<()> {
    let mut model = model();
    model.latest.insert(
        "5v_out".to_string(),
        AdcReading {
            name: "5v_out".to_string(),
            current_ua: Some(42000),
            power_enabled: Some(true),
            ..Default::default()
        },
    );
    model
        .history
        .insert("5v_out".to_string(), vec![100, 200, 300]);

    let buffer = draw(&mut model, 80, 24)?;
    let scope_header = row_text(&buffer, 2);
    assert!(scope_header.contains("5v_out"), "header={scope_header}");
    assert!(scope_header.contains("on"), "header={scope_header}");
    assert!(scope_header.contains("42mA"), "header={scope_header}");
    assert!(scope_header.contains("max=375mA"), "header={scope_header}");
    assert!(!scope_header.contains('│'), "header={scope_header}");
    assert!(row_text(&buffer, 9).contains("Controls"));
    assert!(row_text(&buffer, 10).contains("TYPE"));
    Ok(())
}

#[test]
fn tiny_terminals_render_nothing_without_panicking() -> Result<()> {
    let mut zero = model();
    let buffer = draw(&mut zero, 0, 0)?;
    assert_eq!(buffer.area.width, 0);
    let mut model = model();
    let buffer = draw(&mut model, 80, 5)?;
    assert_eq!(buffer.area.height, 5);
    assert!(model.hit_map.controls.is_empty());
    Ok(())
}

#[test]
fn keybar_key_blocks_are_visually_distinct_from_labels() -> Result<()> {
    let mut model = model();
    let buffer = draw(&mut model, 80, 24)?;
    let keybar_y = 23;
    assert_eq!(buffer[(0, keybar_y)].symbol(), "q");
    assert_eq!(buffer[(0, keybar_y)].fg, Color::Black);
    assert_eq!(buffer[(0, keybar_y)].bg, Color::Cyan);
    assert!(buffer[(0, keybar_y)]
        .modifier
        .contains(ratatui::style::Modifier::BOLD));
    assert_eq!(buffer[(2, keybar_y)].symbol(), "q");
    assert_eq!(buffer[(2, keybar_y)].fg, Color::White);
    assert_eq!(buffer[(2, keybar_y)].bg, Color::DarkGray);
    assert_ne!(
        buffer[(0, keybar_y)].bg,
        buffer[(2, keybar_y)].bg,
        "key block and label must have distinct backgrounds"
    );
    let keybar = row_text(&buffer, keybar_y);
    assert!(keybar.starts_with("q quit"), "keybar={keybar}");
    Ok(())
}

#[test]
fn keybar_is_fixed_on_the_last_row_and_lists_operations() -> Result<()> {
    let mut model = model();
    let buffer = draw(&mut model, 80, 24)?;
    let keybar = row_text(&buffer, 23);
    assert!(keybar.contains("q quit"), "keybar={keybar}");
    assert!(keybar.contains("Tab/Shift+Tab"), "keybar={keybar}");
    assert!(keybar.contains("click"), "keybar={keybar}");
    Ok(())
}

#[test]
fn keybar_reflects_the_active_page() -> Result<()> {
    let mut model = model();
    model.next_page();
    model.next_page();
    let buffer = draw(&mut model, 80, 24)?;
    let keybar = row_text(&buffer, 23);
    assert!(keybar.contains("Status"), "keybar={keybar}");
    assert!(keybar.contains("Tab/Shift+Tab"), "keybar={keybar}");
    Ok(())
}

#[test]
fn keybar_switches_to_confirm_cancel_help_while_modal_is_open() -> Result<()> {
    let mut model = model();
    model.hardware_confirm = Some(power_confirmation());
    let buffer = draw(&mut model, 80, 24)?;
    let keybar = row_text(&buffer, 23);
    assert!(keybar.contains("Esc cancel"), "keybar={keybar}");
    assert!(!keybar.contains("q quit"), "keybar={keybar}");
    Ok(())
}
