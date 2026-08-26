use super::gpio_fixture::{body_line, control_rect, draw, find_row, projection_model, row_text};
use super::gpio_gesture::GpioGestureInput;
use super::gpio_io::{GpioAction, GpioJob};
use super::model::TuiModel;
use super::render_body::build_body_content;
use crate::client::DEFAULT_BASE_URL;
use crate::ws_status::WsStatusSnapshot;
use anyhow::{anyhow, Result};
use ratatui::buffer::Buffer;
use ratatui::style::{Color, Modifier};
use std::time::{Duration, Instant};
use unicode_width::UnicodeWidthStr;

// Every firmware row holds exactly two entries, so no half-empty paired row
// distorts the full-width measurement.
const PAIRED_STATUS_JSON: &str = r#"{"gpios":[
  {"name":"GP8","pin":8,"value":0,"direction":"input","note":"CON_REST","layoutGroup":"J13","layoutLabel":"RSET","layoutRow":0,"layoutColumn":0},
  {"name":"GP9","pin":9,"value":1,"direction":"input","note":"CON_USER","layoutGroup":"J13","layoutLabel":"USER","layoutRow":0,"layoutColumn":1},
  {"name":"GP10","pin":10,"value":1,"direction":"output","note":"J16_PIN1","layoutGroup":"J16","layoutLabel":"GP10","layoutRow":5,"layoutColumn":0},
  {"name":"GP16","pin":16,"value":0,"direction":"output","note":"J16_PIN2","layoutGroup":"J16","layoutLabel":"GP16","layoutRow":5,"layoutColumn":1}
]}"#;

fn paired_model() -> Result<TuiModel> {
    let snapshot: WsStatusSnapshot =
        serde_json::from_str(PAIRED_STATUS_JSON).map_err(anyhow::Error::msg)?;
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.apply_status_snapshot(snapshot);
    Ok(model)
}

fn hold_down(model: &mut TuiModel, pin: &str) {
    let _ = model.gpio_gesture.down(
        GpioGestureInput {
            pin: Some(pin),
            column: 0,
            row: 0,
        },
        Instant::now(),
    );
}

fn await_second(model: &mut TuiModel, pin: &str) {
    let now = Instant::now();
    let input = GpioGestureInput {
        pin: Some(pin),
        column: 0,
        row: 0,
    };
    let _ = model.gpio_gesture.down(input, now);
    model.gpio_gesture.up(input, now);
}

fn set_pending(model: &mut TuiModel, target: &str, action: GpioAction) {
    model.gpio_pending = Some(GpioJob {
        action,
        target: target.to_string(),
    });
}

fn tag_start(buffer: &Buffer, y: u16, tag: &str) -> Option<u16> {
    let symbols: Vec<String> = (0..buffer.area.width)
        .map(|x| buffer[(x, y)].symbol().to_string())
        .collect();
    let tag_len = tag.chars().count();
    (0..buffer.area.width).find(|start| {
        let mut text = String::new();
        let mut x = *start as usize;
        while text.chars().count() < tag_len && x < symbols.len() {
            text.push_str(&symbols[x]);
            x += 1;
        }
        text == tag
    })
}

#[test]
fn active_hold_tags_only_the_down_target() -> Result<()> {
    let mut model = projection_model()?;
    model.width = 80;
    hold_down(&mut model, "GP16");

    let line = body_line(&model, 80, "GP16")?;
    assert!(
        line.contains("○ OUT LOW [HOLD…]"),
        "the hold tag must follow the authoritative state suffix: {line:?}"
    );
    let other = body_line(&model, 80, "GP15")?;
    assert!(
        !other.contains("[HOLD…]"),
        "an unrelated pin row stays untagged: {other:?}"
    );
    let tagged = build_body_content(&model, 80)
        .lines
        .iter()
        .map(ToString::to_string)
        .filter(|line| line.contains("[HOLD…]"))
        .count();
    assert_eq!(tagged, 1, "exactly one cell carries the hold tag");
    Ok(())
}

#[test]
fn unselected_hold_tag_is_warn_yellow_and_bold() -> Result<()> {
    let mut model = projection_model()?;
    // control_idx 0 selects GP10; GP16 stays unselected on the same visual row.
    hold_down(&mut model, "GP16");
    let buffer = draw(&mut model, 80, 24)?;
    let row = find_row(&buffer, "[HOLD…]")?;
    let start = tag_start(&buffer, row, "[HOLD…]")
        .ok_or_else(|| anyhow!("hold tag cells missing on row {row}"))?;
    for x in start..start + 7 {
        let cell = &buffer[(x, row)];
        assert_eq!(cell.fg, Color::Yellow, "hold tag cell x={x} must warn");
        assert!(
            cell.modifier.contains(Modifier::BOLD),
            "hold tag cell x={x} must be bold"
        );
        assert_ne!(
            cell.bg,
            Color::White,
            "an unselected hold tag must not wear the selection background"
        );
    }
    Ok(())
}

#[test]
fn selected_hold_cell_keeps_focus_background_and_a_visible_bold_tag() -> Result<()> {
    let mut model = projection_model()?;
    hold_down(&mut model, "GP10");
    let buffer = draw(&mut model, 80, 24)?;

    let rect = control_rect(&model, "GP10")?;
    for x in rect.x..rect.x + rect.width {
        assert_eq!(
            buffer[(x, rect.y)].bg,
            Color::White,
            "the selected cell keeps its full-region focus background at x={x}"
        );
    }
    let row = row_text(&buffer, rect.y);
    assert!(
        row.contains("● OUT HIGH [HOLD…]"),
        "the hold tag stays visible on the selected cell: {row:?}"
    );
    let start = tag_start(&buffer, rect.y, "[HOLD…]")
        .ok_or_else(|| anyhow!("hold tag cells missing on the selected row"))?;
    for x in start..start + 7 {
        let cell = &buffer[(x, rect.y)];
        assert_eq!(
            cell.bg,
            Color::White,
            "selected tag cell x={x} keeps focus bg"
        );
        assert!(
            cell.modifier.contains(Modifier::BOLD),
            "selected tag cell x={x} stays bold"
        );
    }
    Ok(())
}

#[test]
fn await_second_renders_no_hold_tag() -> Result<()> {
    let mut model = projection_model()?;
    model.width = 80;
    await_second(&mut model, "GP10");

    let line = body_line(&model, 80, "GP10")?;
    assert!(
        !line.contains("[HOLD…]"),
        "await-second must not carry a hold tag: {line:?}"
    );
    assert!(
        line.contains("● OUT HIGH"),
        "the authoritative suffix still renders: {line:?}"
    );
    Ok(())
}

#[test]
fn pending_action_tag_wins_over_a_hold_tag() -> Result<()> {
    let mut model = projection_model()?;
    model.width = 80;
    // Abnormal fixture: an in-flight job and an active hold on the same pin.
    set_pending(&mut model, "GP10", GpioAction::DriveLow);
    hold_down(&mut model, "GP10");

    let line = body_line(&model, 80, "GP10")?;
    assert!(
        line.contains("[LOW…]"),
        "the pending action tag wins the single tag slot: {line:?}"
    );
    assert!(
        !line.contains("[HOLD…]"),
        "the hold tag yields to the pending action tag: {line:?}"
    );
    Ok(())
}

#[test]
fn narrow_cells_drop_the_hold_tag_before_the_state_suffix() -> Result<()> {
    let mut model = projection_model()?;
    model.width = 47;
    hold_down(&mut model, "GP10");

    // Cell width 12 fits the suffix but not the tag: the tag drops whole.
    let line = body_line(&model, 12, "OUT")?;
    assert!(line.contains("● OUT HIGH"), "suffix stays whole: {line:?}");
    assert!(!line.contains("[HOLD…]"), "tag drops first: {line:?}");
    assert_eq!(UnicodeWidthStr::width(line.as_str()), 12);
    Ok(())
}

#[test]
fn hold_cell_layout_holds_at_every_canonical_width() -> Result<()> {
    for width in [48usize, 80, 120] {
        let mut model = paired_model()?;
        model.width = width;
        hold_down(&mut model, "GP10");

        let widths: Vec<usize> = build_body_content(&model, width)
            .lines
            .iter()
            .map(|line| UnicodeWidthStr::width(line.to_string().as_str()))
            .collect();
        assert!(
            widths.iter().all(|line_width| *line_width == width),
            "every body line must fill exactly {width} display columns, got {widths:?}"
        );
        let line = body_line(&model, width, "GP1")?;
        assert!(
            line.contains("● OUT HIGH [HOLD…]"),
            "state suffix and hold tag survive at width {width}: {line:?}"
        );
    }
    Ok(())
}

#[test]
fn identical_hold_state_renders_identical_buffers() -> Result<()> {
    let mut model = projection_model()?;
    hold_down(&mut model, "GP10");
    let first = draw(&mut model, 80, 24)?;
    let second = draw(&mut model, 80, 24)?;
    assert_eq!(first, second, "rendering must be deterministic");
    Ok(())
}
