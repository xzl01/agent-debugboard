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
const CJK_CHANNELS: [&str; 3] = ["五伏输出", "十二伏输出", "二十伏输出"];

#[derive(Clone, Copy)]
struct ScopeCase {
    width: u16,
    height: u16,
    starts: [usize; 3],
    gutters: [u16; 2],
}

const SCOPE_CASES: [ScopeCase; 4] = [
    ScopeCase {
        width: 47,
        height: 24,
        starts: [0, 16, 32],
        gutters: [15, 31],
    },
    ScopeCase {
        width: 48,
        height: 24,
        starts: [0, 17, 33],
        gutters: [16, 32],
    },
    ScopeCase {
        width: 80,
        height: 24,
        starts: [0, 27, 54],
        gutters: [26, 53],
    },
    ScopeCase {
        width: 120,
        height: 32,
        starts: [0, 41, 81],
        gutters: [40, 80],
    },
];

fn model_with_channels(peak_ma: i32) -> TuiModel {
    model_with_named_channels(&CHANNELS, peak_ma)
}

fn model_with_named_channels(channels: &[&str], peak_ma: i32) -> TuiModel {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    model.channel_ids = channels
        .iter()
        .map(|channel| (*channel).to_string())
        .collect();
    for channel in channels {
        model.latest.insert(
            (*channel).to_string(),
            AdcReading {
                name: (*channel).to_string(),
                current_ua: Some(peak_ma * 1000),
                power_enabled: Some(true),
                ..Default::default()
            },
        );
        model
            .history
            .insert((*channel).to_string(), vec![peak_ma; 120]);
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
fn scope_header_reserves_one_column_gutters_at_canonical_widths() -> Result<()> {
    for case in SCOPE_CASES {
        // Given
        let mut model = model_with_channels(10);

        // When
        let buffer = draw(&mut model, case.width, case.height)?;
        let header = row_text(&buffer, 2);

        // Then
        for (channel, start) in CHANNELS.iter().zip(case.starts) {
            assert_eq!(char_index(&header, channel)?, start, "header={header:?}");
        }
        for gutter in case.gutters {
            assert_eq!(buffer[(gutter, 2)].symbol(), " ", "header={header:?}");
        }
        assert_eq!(header.chars().count(), case.width as usize);
    }
    Ok(())
}

#[test]
fn scope_graph_reserves_gutters_in_all_six_rows() -> Result<()> {
    for case in SCOPE_CASES {
        // Given
        let mut model = model_with_channels(10);

        // When
        let buffer = draw(&mut model, case.width, case.height)?;

        // Then
        for y in 3u16..=8 {
            for gutter in case.gutters {
                assert_eq!(
                    buffer[(gutter, y)].symbol(),
                    " ",
                    "width={} y={y}",
                    case.width
                );
            }
            assert_eq!(row_text(&buffer, y).chars().count(), case.width as usize);
        }
        for y in 4u16..=8 {
            for gutter in case.gutters {
                assert!(BAR_GLYPHS.contains(buffer[(gutter - 1, y)].symbol()));
                assert!(BAR_GLYPHS.contains(buffer[(gutter + 1, y)].symbol()));
            }
        }
    }
    Ok(())
}

#[test]
fn scope_cjk_channel_names_do_not_cross_gutters() -> Result<()> {
    // Given
    let mut model = model_with_named_channels(&CJK_CHANNELS, 10);

    // When
    let buffer = draw(&mut model, 47, 24)?;
    let header = row_text(&buffer, 2);

    // Then
    for (symbol, start) in ["五", "十", "二"].iter().zip([0u16, 16, 32]) {
        assert_eq!(buffer[(start, 2)].symbol(), *symbol, "header={header:?}");
    }
    for gutter in [15u16, 31] {
        assert_eq!(buffer[(gutter, 2)].symbol(), " ", "header={header:?}");
    }
    assert_eq!(header.chars().count(), 47);
    Ok(())
}

#[test]
fn scope_single_channel_keeps_the_full_width() -> Result<()> {
    // Given
    let mut model = model_with_named_channels(&["single"], 10);

    // When
    let buffer = draw(&mut model, 47, 24)?;

    // Then
    assert_eq!(char_index(&row_text(&buffer, 2), "single")?, 0);
    for y in 4u16..=8 {
        assert!(BAR_GLYPHS.contains(buffer[(0, y)].symbol()));
        assert!(BAR_GLYPHS.contains(buffer[(46, y)].symbol()));
    }
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
