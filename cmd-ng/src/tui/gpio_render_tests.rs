use super::gpio_fixture::{
    body_line, control_rect, draw, find_row, item_index, projection_model, row_text,
};
use super::gpio_io::{GpioAction, GpioJob};
use super::model::TuiModel;
use super::render_body::build_body_content;
use crate::client::DEFAULT_BASE_URL;
use crate::ws_status::WsStatusSnapshot;
use anyhow::{anyhow, Result};
use ratatui::buffer::Buffer;
use ratatui::style::{Color, Modifier};
use std::time::Duration;
use unicode_width::UnicodeWidthStr;

const CJK_STATUS_JSON: &str = r#"{"gpios":[
  {"name":"GP8","pin":8,"value":0,"direction":"input","note":"复位按键","layoutGroup":"J13","layoutLabel":"复位","layoutRow":0,"layoutColumn":0},
  {"name":"GP9","pin":9,"value":1,"direction":"input","note":"用户按键","layoutGroup":"J13","layoutLabel":"用户","layoutRow":0,"layoutColumn":1},
  {"name":"GP10","pin":10,"value":1,"direction":"output","note":"J16_PIN1","layoutGroup":"J16","layoutLabel":"GP10","layoutRow":5,"layoutColumn":0},
  {"name":"GP16","pin":16,"value":0,"direction":"output","note":"J16_PIN2","layoutGroup":"J16","layoutLabel":"GP16","layoutRow":5,"layoutColumn":1}
]}"#;

fn cjk_model() -> Result<TuiModel> {
    let snapshot: WsStatusSnapshot =
        serde_json::from_str(CJK_STATUS_JSON).map_err(anyhow::Error::msg)?;
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.apply_status_snapshot(snapshot);
    Ok(model)
}

fn set_pending(model: &mut TuiModel, target: &str, action: GpioAction) {
    model.gpio_pending = Some(GpioJob {
        action,
        target: target.to_string(),
    });
}

fn body_widths(model: &TuiModel, width: usize) -> Vec<usize> {
    build_body_content(model, width)
        .lines
        .iter()
        .map(|line| UnicodeWidthStr::width(line.to_string().as_str()))
        .collect()
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
fn pending_drive_high_appends_the_tag_to_the_target_cell_only() -> Result<()> {
    let mut model = projection_model()?;
    model.width = 80;
    set_pending(&mut model, "GP10", GpioAction::DriveHigh);

    let line = body_line(&model, 80, "GP10")?;
    assert!(
        line.contains("● OUT HIGH [HIGH…]"),
        "the pending tag must follow the authoritative state suffix: {line:?}"
    );
    assert_eq!(
        line.matches("[HIGH…]").count(),
        1,
        "only the targeted pin carries the tag: {line:?}"
    );
    let other = body_line(&model, 80, "GP15")?;
    assert!(!other.contains("[HIGH…]"), "unrelated pin row: {other:?}");
    Ok(())
}

#[test]
fn pending_tag_matches_the_requested_action() -> Result<()> {
    for (action, tag, others) in [
        (GpioAction::DriveLow, "[LOW…]", ["[HIGH…]", "[INPUT…]"]),
        (GpioAction::DriveHigh, "[HIGH…]", ["[LOW…]", "[INPUT…]"]),
        (GpioAction::SetInput, "[INPUT…]", ["[LOW…]", "[HIGH…]"]),
    ] {
        let mut model = projection_model()?;
        model.width = 80;
        set_pending(&mut model, "GP10", action);
        let line = body_line(&model, 80, "GP10")?;
        assert!(line.contains(tag), "expected {tag} in {line:?}");
        for other in others {
            assert!(!line.contains(other), "unexpected {other} in {line:?}");
        }
    }
    Ok(())
}

#[test]
fn unselected_pending_tag_is_warn_yellow_and_bold() -> Result<()> {
    let mut model = projection_model()?;
    // control_idx 0 selects GP10; GP16 stays unselected on the same visual row.
    set_pending(&mut model, "GP16", GpioAction::DriveHigh);
    let buffer = draw(&mut model, 80, 24)?;
    let row = find_row(&buffer, "[HIGH…]")?;
    let start = tag_start(&buffer, row, "[HIGH…]")
        .ok_or_else(|| anyhow!("pending tag cells missing on row {row}"))?;
    for x in start..start + 6 {
        let cell = &buffer[(x, row)];
        assert_eq!(cell.fg, Color::Yellow, "pending tag cell x={x} must warn");
        assert!(
            cell.modifier.contains(Modifier::BOLD),
            "pending tag cell x={x} must be bold"
        );
        assert_ne!(
            cell.bg,
            Color::White,
            "an unselected pending tag must not wear the selection background"
        );
    }
    Ok(())
}

#[test]
fn selected_pending_cell_keeps_focus_background_and_a_visible_bold_tag() -> Result<()> {
    let mut model = projection_model()?;
    model.control_idx = item_index(&model, "GP10")?;
    set_pending(&mut model, "GP10", GpioAction::DriveHigh);
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
        row.contains("● OUT HIGH [HIGH…]"),
        "the pending tag stays visible on the selected cell: {row:?}"
    );
    let start = tag_start(&buffer, rect.y, "[HIGH…]")
        .ok_or_else(|| anyhow!("pending tag cells missing on the selected row"))?;
    for x in start..start + 6 {
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
fn pending_cell_layout_holds_at_every_canonical_width() -> Result<()> {
    for width in [47usize, 48, 80, 120] {
        let mut model = cjk_model()?;
        model.width = width;
        set_pending(&mut model, "GP10", GpioAction::DriveHigh);

        let widths = body_widths(&model, width);
        assert!(
            widths.iter().all(|line_width| *line_width == width),
            "every body line must fill exactly {width} display columns, got {widths:?}"
        );
        let line = body_line(&model, width, "GP1")?;
        assert!(
            line.contains("● OUT HIGH [HIGH…]"),
            "state suffix and pending tag survive at width {width}: {line:?}"
        );
        if width == 48 {
            assert!(
                !line.contains("J16 GP10"),
                "group/label yields to the pending tag at width 48: {line:?}"
            );
        }
    }
    Ok(())
}

#[test]
fn cjk_cells_never_split_wide_glyphs_or_wrap() -> Result<()> {
    for width in [47usize, 48, 80, 120] {
        let mut model = cjk_model()?;
        model.width = width;
        let widths = body_widths(&model, width);
        assert!(
            widths.iter().all(|line_width| *line_width == width),
            "CJK body lines must fill exactly {width} columns, got {widths:?}"
        );
        for line in build_body_content(&model, width)
            .lines
            .iter()
            .map(ToString::to_string)
        {
            assert!(
                !line.contains('\u{FFFD}'),
                "replacement glyph leaked at width {width}: {line:?}"
            );
        }
        if width == 48 {
            let paired = body_line(&model, width, "复位")?;
            assert!(
                paired.contains("用户"),
                "the J13 firmware row keeps both CJK cells paired at 48: {paired:?}"
            );
        }
        if width == 47 {
            let reset = body_line(&model, width, "复位")?;
            assert!(
                !reset.contains("用户"),
                "below 48 columns each CJK cell owns its row: {reset:?}"
            );
        }
    }
    Ok(())
}

#[test]
fn narrow_cells_drop_the_pending_tag_before_the_state_suffix() -> Result<()> {
    let mut model = cjk_model()?;
    model.width = 47;
    set_pending(&mut model, "GP10", GpioAction::DriveHigh);

    // Cell width 12 fits the suffix but not the tag: the tag drops and the
    // status line keeps carrying the in-flight action text.
    let line = body_line(&model, 12, "OUT")?;
    assert!(line.contains("● OUT HIGH"), "suffix stays whole: {line:?}");
    assert!(!line.contains("[HIGH…]"), "tag drops first: {line:?}");
    assert_eq!(UnicodeWidthStr::width(line.as_str()), 12);

    // A cell narrower than the suffix hard-clips without wrapping.
    let line = build_body_content(&model, 8)
        .lines
        .iter()
        .map(ToString::to_string)
        .find(|line| !line.trim().is_empty())
        .ok_or_else(|| anyhow!("missing hard-clipped gpio line"))?;
    assert_eq!(UnicodeWidthStr::width(line.as_str()), 8);
    Ok(())
}

#[test]
fn identical_state_renders_identical_buffers() -> Result<()> {
    let mut model = cjk_model()?;
    model.width = 80;
    set_pending(&mut model, "GP10", GpioAction::DriveHigh);
    model.control_idx = item_index(&model, "GP10")?;
    let first = draw(&mut model, 80, 24)?;
    let second = draw(&mut model, 80, 24)?;
    assert_eq!(first, second, "rendering must be deterministic");
    Ok(())
}
