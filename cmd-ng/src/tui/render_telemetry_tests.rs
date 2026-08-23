use super::model::TuiModel;
use super::render::render_ui;
use crate::adc::AdcReading;
use crate::client::DEFAULT_BASE_URL;
use anyhow::{anyhow, Result};
use ratatui::backend::TestBackend;
use ratatui::buffer::Buffer;
use ratatui::Terminal;
use std::time::Duration;

const BAR_GLYPHS: &str = "▁▂▃▄▅▆▇█";
const CHANNELS: [&str; 3] = ["5v_out", "12v_out", "20v_out"];

fn model_with_channels(peak_ma: i32) -> TuiModel {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    for channel in CHANNELS {
        model.latest.insert(
            channel.to_string(),
            AdcReading {
                name: channel.to_string(),
                current_ua: Some(peak_ma * 1000),
                power_enabled: Some(true),
                ..Default::default()
            },
        );
        let ramp: Vec<i32> = (1..=5).map(|step| peak_ma * step / 5).collect();
        model.history.insert(channel.to_string(), ramp);
    }
    model
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

fn buffer_text(buffer: &Buffer) -> String {
    (0..buffer.area.height)
        .map(|y| row_text(buffer, y))
        .collect::<Vec<_>>()
        .join("\n")
}

fn char_index(text: &str, needle: &str) -> Result<usize> {
    text.find(needle)
        .map(|byte| text[..byte].chars().count())
        .ok_or_else(|| anyhow!("{needle:?} missing in {text:?}"))
}

#[test]
fn scope_band_occupies_seven_rows_when_data_exists() -> Result<()> {
    for (width, height) in [(80u16, 24u16), (120, 32)] {
        let mut model = model_with_channels(10);
        let buffer = draw(&mut model, width, height)?;
        // 1 header + 6 graph rows below the 2-row status band push the tabs
        // to 0-based row 9 and the table header to row 10.
        assert!(
            row_text(&buffer, 9).contains("Controls"),
            "tabs must sit at 0-based row 9 below a 7-row scope at {width}x{height}: {:?}",
            row_text(&buffer, 9)
        );
        assert!(
            row_text(&buffer, 10).contains("TYPE"),
            "table header must sit at 0-based row 10 at {width}x{height}: {:?}",
            row_text(&buffer, 10)
        );
    }
    Ok(())
}

#[test]
fn scope_header_shows_adaptive_scale_not_a_fixed_one() -> Result<()> {
    let mut model = model_with_channels(10);
    let buffer = draw(&mut model, 80, 24)?;
    assert!(
        buffer_text(&buffer).contains("max=13mA"),
        "peak 10mA must yield scale ceil(10*5/4)=13mA, never a fixed scale"
    );

    let mut model = model_with_channels(220);
    let buffer = draw(&mut model, 80, 24)?;
    assert!(
        buffer_text(&buffer).contains("max=275mA"),
        "peak 220mA must yield scale ceil(220*5/4)=275mA, never a fixed scale"
    );
    Ok(())
}

#[test]
fn scope_graph_spans_multiple_rows_with_block_glyphs() -> Result<()> {
    let mut model = model_with_channels(10);
    let buffer = draw(&mut model, 80, 24)?;
    // 0-based rows 3..=8 are the six graph rows; a 10mA ramp on an adaptive
    // scale must paint visible bars across several of them.
    let painted = (3u16..=8)
        .filter(|y| {
            row_text(&buffer, *y)
                .chars()
                .any(|ch| BAR_GLYPHS.contains(ch))
        })
        .count();
    assert!(
        painted >= 3,
        "scope graph must occupy multiple rows with block glyphs, found {painted} painted rows"
    );
    Ok(())
}

#[test]
fn scope_channels_split_into_deterministic_columns() -> Result<()> {
    let mut model = model_with_channels(10);
    let buffer = draw(&mut model, 80, 24)?;
    let header = row_text(&buffer, 2);
    assert_eq!(char_index(&header, "5v_out")?, 0, "header={header:?}");
    assert_eq!(char_index(&header, "12v_out")?, 27, "header={header:?}");
    assert_eq!(char_index(&header, "20v_out")?, 54, "header={header:?}");

    let mut model = model_with_channels(10);
    let buffer = draw(&mut model, 120, 32)?;
    let header = row_text(&buffer, 2);
    assert_eq!(char_index(&header, "5v_out")?, 0, "header={header:?}");
    assert_eq!(char_index(&header, "12v_out")?, 40, "header={header:?}");
    assert_eq!(char_index(&header, "20v_out")?, 80, "header={header:?}");
    Ok(())
}

#[test]
fn scope_is_zero_height_without_data() -> Result<()> {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    let buffer = draw(&mut model, 80, 24)?;
    assert!(row_text(&buffer, 2).contains("Controls"));
    assert!(row_text(&buffer, 3).contains("TYPE"));
    Ok(())
}
