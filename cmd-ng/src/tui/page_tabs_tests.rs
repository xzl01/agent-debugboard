use super::config_rows::build_saved_config_content;
use super::model::{TuiModel, TuiSwitchState};
use super::pages::ActivePage;
use super::render::render_ui;
use super::render_body::build_body_content;
use crate::client::DEFAULT_BASE_URL;
use crate::persistent_config::{ConfigAction, PersistentConfigResponse, PersistentConfigStatus};
use anyhow::{anyhow, Result};
use ratatui::backend::TestBackend;
use ratatui::buffer::Buffer;
use ratatui::style::Modifier;
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

fn header_label_position(buffer: &Buffer, label: &str) -> Option<(u16, u16)> {
    for y in 0..4 {
        let row = (0..buffer.area.width)
            .map(|x| buffer[(x, y)].symbol())
            .collect::<String>();
        if let Some(byte_offset) = row.find(label) {
            let column = row[..byte_offset].chars().count() as u16;
            return Some((column, y));
        }
    }
    None
}

fn row_text(buffer: &Buffer, y: u16) -> String {
    (0..buffer.area.width)
        .map(|x| buffer[(x, y)].symbol())
        .collect()
}

fn loaded_saved_config() -> Result<TuiModel> {
    const SHOW: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":1},"pending":1,"items":[{"id":"power/alpha","kind":"power","current":{"state":"off"},"saved":{"state":"on"},"selected":true,"requires_confirm":true,"apply_state":"pending"}]}"#;
    let response =
        PersistentConfigResponse::from_raw(SHOW.to_string()).map_err(anyhow::Error::msg)?;
    response
        .validate(&ConfigAction::Get, None)
        .map_err(anyhow::Error::msg)?;
    let mut model = model();
    model
        .saved_config
        .observe_summary(Some(PersistentConfigStatus {
            available: true,
            reason: "ready".to_string(),
            saved_count: 1,
            pending_count: 1,
        }));
    model
        .saved_config
        .apply_authoritative(response)
        .map_err(anyhow::Error::msg)?;
    Ok(model)
}

#[test]
fn visible_page_tabs_highlight_the_active_page_at_supported_sizes() -> Result<()> {
    for (width, height) in [(80, 24), (120, 32)] {
        for (page, active_label) in [
            (ActivePage::Controls, "Controls"),
            (ActivePage::SavedConfig, "Saved Config"),
            (ActivePage::Status, "Status"),
        ] {
            let mut model = model();
            model.set_page(page);

            let buffer = draw(&mut model, width, height)?;

            for label in ["Controls", "Saved Config", "Status"] {
                let (x, y) = header_label_position(&buffer, label)
                    .ok_or_else(|| anyhow!("missing {label} tab at {width}x{height}"))?;
                assert_eq!(
                    buffer[(x, y)].modifier.contains(Modifier::REVERSED),
                    label == active_label,
                    "unexpected {label} tab style for {active_label} at {width}x{height}"
                );
            }
        }
    }
    Ok(())
}

#[test]
fn keybar_keeps_complete_segments_at_supported_sizes() -> Result<()> {
    for (width, height, page, expected) in [
        (80, 24, ActivePage::Controls, "g GPIO"),
        (80, 24, ActivePage::SavedConfig, "x clear"),
        (120, 32, ActivePage::Controls, "PgUp/PgDn Move"),
        (120, 32, ActivePage::SavedConfig, "Esc back"),
    ] {
        let mut model = model();
        model.set_page(page);

        let buffer = draw(&mut model, width, height)?;

        let keybar = row_text(&buffer, height - 1);
        assert!(keybar.contains(expected), "keybar={keybar:?}");
    }
    Ok(())
}

#[test]
fn status_content_renders_one_row_per_switch_and_monitor_field() {
    let mut model = model();
    model.set_page(ActivePage::Status);
    for (name, route) in [
        ("sd", "usb-reader"),
        ("tf_wp", "writable"),
        ("usb", "pc"),
        ("vin", "3.3v"),
    ] {
        model.switches.insert(
            name.to_string(),
            TuiSwitchState {
                name: name.to_string(),
                desired_route: route.to_string(),
                actual_route: route.to_string(),
                ..Default::default()
            },
        );
    }

    let content = build_body_content(&model, 76);

    assert_eq!(content.lines.len(), 8, "4 switch rows + 4 monitor rows");
    assert!(content.lines.iter().all(|line| line.width() <= 76));
}

#[test]
fn saved_config_renders_one_row_per_item_at_eighty_columns() -> Result<()> {
    let model = loaded_saved_config()?;

    let content = build_saved_config_content(&model.saved_config, 76);

    // Given one loaded item with a pending count: one badge row + one item row.
    assert_eq!(content.lines.len(), 2);
    assert!(content.lines.iter().all(|line| line.width() <= 76));
    let item = content.lines[1].to_string();
    assert!(item.contains("[x]"), "item={item}");
    assert!(item.contains("power/alpha"), "item={item}");
    assert!(item.contains("danger"), "item={item}");
    Ok(())
}
