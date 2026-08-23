use super::controls::{control_targets, ControlItem};
use super::gpio_fixture::current_power_outputs;
use super::model::TuiModel;
use super::render::render_ui;
use super::render_body::build_body_content;
use crate::client::DEFAULT_BASE_URL;
use crate::persistent_config::{ConfigAction, PersistentConfigResponse, PersistentConfigStatus};
use crate::ws_status::{TuiStatusGpio, TuiStatusSwitchInfo, WsStatusSnapshot};
use anyhow::{anyhow, Result};
use ratatui::backend::TestBackend;
use ratatui::buffer::Buffer;
use ratatui::layout::Rect;
use ratatui::style::Color;
use ratatui::text::Text;
use ratatui::Terminal;
use std::time::Duration;

const SHOW: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":1},"pending":1,"items":[{"id":"power/alpha","kind":"power","current":{"state":"off"},"saved":{"state":"on"},"selected":true,"requires_confirm":true,"apply_state":"pending"}]}"#;

fn load_saved_config(model: &mut TuiModel, raw: &str) -> Result<()> {
    let response =
        PersistentConfigResponse::from_raw(raw.to_string()).map_err(anyhow::Error::msg)?;
    response
        .validate(&ConfigAction::Get, None)
        .map_err(anyhow::Error::msg)?;
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
    Ok(())
}

fn many_items_raw(count: usize) -> String {
    let items = (0..count)
        .map(|index| {
            format!(
                r#"{{"id":"power/p{index}","kind":"power","current":{{"state":"off"}},"saved":{{"state":"on"}},"selected":false,"requires_confirm":false,"apply_state":"applied"}}"#
            )
        })
        .collect::<Vec<_>>()
        .join(",");
    format!(
        r#"{{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{{"available":true,"reason":"ready"}},"snapshot":{{"present":true,"version":1}},"pending":0,"items":[{items}]}}"#
    )
}

fn model() -> TuiModel {
    TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2))
}

fn dashboard_model() -> TuiModel {
    let mut model = model();
    let mut snapshot = WsStatusSnapshot {
        power_outputs: current_power_outputs(),
        ..Default::default()
    };
    snapshot.switches.insert(
        "sd".to_string(),
        TuiStatusSwitchInfo {
            route: "target".to_string(),
            routes: vec!["target".to_string(), "usb-reader".to_string()],
            ..Default::default()
        },
    );
    snapshot.gpios.push(TuiStatusGpio {
        name: "GP10".to_string(),
        pin: 10,
        value: Some(1),
        direction: "output".to_string(),
        note: "J16_PIN1".to_string(),
        ..Default::default()
    });
    model.apply_status_snapshot(snapshot);
    model
}

fn draw(model: &mut TuiModel, width: u16, height: u16) -> Result<Buffer> {
    let backend = TestBackend::new(width, height);
    let mut terminal = Terminal::new(backend)?;
    terminal.draw(|frame| render_ui(frame, model))?;
    Ok(terminal.backend().buffer().clone())
}

fn buffer_text(buffer: &Buffer) -> String {
    let mut text = String::new();
    for y in 0..buffer.area.height {
        for x in 0..buffer.area.width {
            text.push_str(buffer[(x, y)].symbol());
        }
        text.push('\n');
    }
    text
}

fn control_rect(model: &TuiModel, wanted: &ControlItem) -> Option<Rect> {
    model
        .hit_map
        .controls
        .iter()
        .find(|(_, item)| item == wanted)
        .map(|(rect, _)| *rect)
}

#[test]
fn controls_page_renders_one_row_per_object_without_separators() {
    let model = dashboard_model();
    let content = build_body_content(&model, 80);
    // dashboard_model: 4 power + 1 switch + 1 gpio = 6 object rows, nothing else.
    assert_eq!(content.lines.len(), 6);
    assert_eq!(content.selection_line, Some(0));
    assert_eq!(content.marks.len(), 6);
    let rendered: Vec<String> = content.lines.iter().map(ToString::to_string).collect();
    assert!(rendered[0].contains("power"));
    assert!(rendered[0].contains("12v_out"));
    assert!(rendered[0].contains("off"));
    assert!(rendered[4].contains("switch"));
    assert!(rendered[4].contains("sd"));
    assert!(rendered[4].contains("target"));
    assert!(rendered[5].contains("GPIO"));
    assert!(rendered[5].contains("GP10"));
    assert!(rendered[5].contains("● OUT HIGH"));
    assert!(rendered[5].contains("J16_PIN1"));
    assert!(rendered
        .iter()
        .all(|line| !line.trim().is_empty() && !line.contains("Power")));
}

#[test]
fn status_page_renders_switch_rows_without_controls_page_rows() {
    let mut model = dashboard_model();
    model.next_page();
    model.next_page();

    let content = build_body_content(&model, 80);
    let rendered = Text::from(content.lines).to_string();

    assert!(rendered.contains("sd"));
    assert!(rendered.contains("target"));
    assert!(rendered.contains("ready"));
    assert!(!rendered.contains("12v_out"));
    assert!(content.marks.is_empty());
}

#[test]
fn saved_config_page_renders_items_without_controls_page_rows() -> Result<()> {
    let mut model = dashboard_model();
    load_saved_config(&mut model, SHOW)?;
    model.next_page();

    let content = build_body_content(&model, 80);
    let rendered = Text::from(content.lines).to_string();

    assert!(rendered.contains("power/alpha"));
    assert!(rendered.contains("[pending:1]"));
    assert!(!rendered.contains("12v_out"));
    assert!(!rendered.contains("Saved Config"));
    assert!(content.marks.is_empty());
    Ok(())
}

#[test]
fn controls_page_scrolls_to_keep_the_selection_visible() -> Result<()> {
    let mut model = model();
    for pin in 7..=20 {
        model.gpio_names.push(format!("GP{pin}"));
    }
    model.control_idx = model.gpio_names.len() - 1 + control_targets(&model).len();

    let buffer = draw(&mut model, 80, 16)?;
    let text = buffer_text(&buffer);

    assert!(
        text.contains("GP20"),
        "selected gpio must stay visible: {text}"
    );
    assert!(model.page_scroll() > 0);
    Ok(())
}

#[test]
fn saved_config_page_scrolls_to_keep_the_cursor_anchor_visible() -> Result<()> {
    let mut model = model();
    load_saved_config(&mut model, &many_items_raw(8))?;
    model.next_page();
    model.saved_config.cursor = 7;

    let buffer = draw(&mut model, 100, 12)?;
    let text = buffer_text(&buffer);

    assert!(
        text.contains("power/p7"),
        "cursor row must stay visible: {text}"
    );
    assert!(
        !text.contains("power/p0"),
        "scrolled-out row must not render: {text}"
    );
    Ok(())
}

#[test]
fn stale_page_offset_is_clamped_after_content_shrinks() -> Result<()> {
    let mut model = model();
    model.next_page();
    model.set_page_scroll(50);

    draw(&mut model, 80, 24)?;

    assert_eq!(model.page_scroll(), 0);
    Ok(())
}

#[test]
fn control_hit_targets_exist_only_on_the_controls_page() -> Result<()> {
    let mut model = dashboard_model();
    draw(&mut model, 80, 24)?;
    assert!(!model.hit_map.controls.is_empty());

    model.next_page();
    draw(&mut model, 80, 24)?;
    assert!(model.hit_map.controls.is_empty());

    model.next_page();
    draw(&mut model, 80, 24)?;
    assert!(model.hit_map.controls.is_empty());
    Ok(())
}

#[test]
fn body_carries_no_operation_instructions() -> Result<()> {
    let dashboard = dashboard_model();
    let content = build_body_content(&dashboard, 80);
    let rendered = Text::from(content.lines).to_string();
    assert!(!rendered.contains("g jump to first GPIO"));
    assert!(
        !rendered.contains("No-args starts the TUI"),
        "body must not carry the no-args sentence: {rendered}"
    );

    let mut plain = model();
    let buffer = draw(&mut plain, 80, 24)?;
    assert!(!buffer_text(&buffer).contains("No-args starts the TUI"));
    Ok(())
}

#[test]
fn selected_power_row_keeps_its_state_color_in_the_buffer() -> Result<()> {
    let mut model = model();
    model.apply_status_snapshot(WsStatusSnapshot {
        power_outputs: current_power_outputs(),
        ..Default::default()
    });
    model.power_states.insert("12v_out".to_string(), true);
    model.control_idx = 0;
    let buffer = draw(&mut model, 80, 30)?;

    let rect = control_rect(&model, &ControlItem::Power("12v_out".to_string()))
        .ok_or_else(|| anyhow!("12v_out row is missing"))?;
    for x in rect.x..rect.x + rect.width {
        assert_eq!(
            buffer[(x, rect.y)].bg,
            Color::White,
            "selected row must paint White bg across the full hit width (x={x})"
        );
    }
    let state_cell = (rect.x..rect.x + rect.width)
        .map(|x| &buffer[(x, rect.y)])
        .find(|cell| cell.symbol() == "n")
        .ok_or_else(|| anyhow!("selected ON state cell is missing"))?;
    assert_eq!(
        state_cell.fg,
        Color::Green,
        "selected ON row keeps green fg"
    );
    Ok(())
}
