use super::model::TuiModel;
use super::render::render_ui;
use super::render_telemetry::voltage_strip_line;
use crate::adc::AdcReading;
use crate::client::DEFAULT_BASE_URL;
use anyhow::{anyhow, Result};
use ratatui::backend::TestBackend;
use ratatui::buffer::Buffer;
use ratatui::Terminal;
use std::time::Duration;
use unicode_width::UnicodeWidthStr;

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

fn model_with_eight_adc_channels() -> Result<TuiModel> {
    let mut model = super::gpio_fixture::x1_2_model()?;
    let mut snapshot: crate::ws_status::WsStatusSnapshot =
        serde_json::from_str(super::gpio_fixture::X1_2_STATUS_JSON)?;
    snapshot.adc_channels = serde_json::from_value(crate::adc::fixture::channels())?;
    model.apply_status_snapshot(snapshot);
    let readings = crate::adc::transform_response(&crate::adc::fixture::http().to_string())
        .map_err(anyhow::Error::msg)?
        .readings;
    model.apply_adc_response(readings);
    Ok(model)
}

#[test]
fn voltage_strip_shows_all_descriptor_channels_without_expanding_current_scope() -> Result<()> {
    for width in [120u16, 80, 47, 32] {
        let mut model = model_with_eight_adc_channels()?;
        model.width = width as usize;
        let buffer = draw(&mut model, width, 24)?;
        let strip = row_text(&buffer, 9);
        for name in ["adc0", "adc1", "adc2", "adc3"] {
            assert_eq!(strip.matches(name).count(), 1, "{width}: {strip}");
        }
        if width >= 80 {
            assert!(strip.contains("adc0=20.000002V"), "{strip}");
            assert!(strip.contains("adc3=0.000000V"), "{strip}");
        }
        if width == 47 {
            for value in ["adc0=20.000", "adc1=1.1000", "adc2=2.2000", "adc3=0.0000"] {
                assert!(strip.contains(value), "{strip}");
            }
        }
        if width == 32 {
            assert!(strip.starts_with("adc0=20V"), "{strip}");
        }
        for signal in ["GPIO26_ADC0", "GPIO27_ADC1", "GPIO28_ADC2", "GPIO29_ADC3"] {
            assert_eq!(strip.contains(signal), width == 120, "{width}: {strip}");
        }
        assert!(!row_text(&buffer, 2).contains("adc0"));
        assert_eq!(model.channel_ids.len(), 4);
        assert_eq!(model.history.len(), 4);
        assert_eq!(model.power_names.len(), 5);
        assert!(!model.latest.contains_key("5v_usb_hub"));
        assert!(!model.channel_ids.iter().any(|name| name == "5v_usb_hub"));
        assert!(row_text(&buffer, 10).contains("Controls"));
        assert!(
            !row_text(&buffer, 23).trim().is_empty(),
            "keybar must retain its final row"
        );
        for (rect, _) in model.hit_map.controls.iter() {
            assert!(rect.y >= 12 && rect.y + rect.height <= 23);
        }
        assert!(model.hit_map.controls.at(0, 9).is_none());
        let line = voltage_strip_line(&model, width as usize).to_string();
        assert_eq!(UnicodeWidthStr::width(line.as_str()), width as usize);
    }
    Ok(())
}

#[test]
fn voltage_strip_names_and_gutters_keep_deterministic_column_boundaries() -> Result<()> {
    let model = model_with_eight_adc_channels()?;
    for (width, starts, gutters) in [
        (32, [0, 9, 17, 25], [8, 16, 24]),
        (47, [0, 12, 24, 36], [11, 23, 35]),
        (80, [0, 21, 41, 61], [20, 40, 60]),
        (120, [0, 31, 61, 91], [30, 60, 90]),
    ] {
        let line = voltage_strip_line(&model, width).to_string();
        for (name, start) in model.voltage_channel_ids.iter().zip(starts) {
            assert_eq!(char_index(&line, name)?, start, "{width}: {line}");
        }
        for gutter in gutters {
            assert_eq!(line.chars().nth(gutter), Some(' '), "{width}: {line}");
        }
    }
    Ok(())
}

#[test]
fn voltage_strip_keeps_two_decimals_when_name_and_column_budget_allow_it() -> Result<()> {
    let mut model = model_with_eight_adc_channels()?;
    // Four four-column names, '=', xx.xx/x.xx and three gutters need 40 columns.
    // At 32 the 8/7/7/7 content columns cannot fit that many characters.
    let line = voltage_strip_line(&model, 40).to_string();
    for value in ["adc0=20.00", "adc1=1.10", "adc2=2.20", "adc3=0.00"] {
        assert!(line.contains(value), "{line}");
    }
    // Shorter, arbitrary firmware identities leave room for >=2 decimals
    // even at 32; signal prefixes must not steal that budget.
    for (old, short) in model
        .voltage_channel_ids
        .clone()
        .iter()
        .zip(["z", "x", "u", "w"])
    {
        let mut reading = model.latest.remove(old).unwrap();
        reading.name = short.to_string();
        model.latest.insert(short.to_string(), reading);
        let mut descriptor = model.adc_channel_metadata.remove(old).unwrap();
        descriptor.name = short.to_string();
        model
            .adc_channel_metadata
            .insert(short.to_string(), descriptor);
    }
    model.voltage_channel_ids = ["z", "x", "u", "w"].map(str::to_string).to_vec();
    let line = voltage_strip_line(&model, 32).to_string();
    for value in ["z=20.000", "x=1.100", "u=2.200", "w=0.000"] {
        assert!(line.contains(value), "{line}");
    }
    assert!(!line.contains("GPIO"));
    Ok(())
}

#[test]
fn voltage_strip_missing_invalid_or_wrong_kind_readings_keep_names_and_unknowns() -> Result<()> {
    let mut model = model_with_eight_adc_channels()?;
    model.latest.remove("adc0");
    model.latest.get_mut("adc1").unwrap().voltage_uv = None;
    model.latest.get_mut("adc2").unwrap().voltage_valid = Some(false);
    model.latest.get_mut("adc3").unwrap().kind = crate::adc::AdcKind::Current;
    for width in [32, 47, 80, 120] {
        let line = voltage_strip_line(&model, width).to_string();
        for name in &model.voltage_channel_ids {
            assert!(line.contains(&format!("{name}=--")), "{width}: {line}");
        }
        assert_eq!(UnicodeWidthStr::width(line.as_str()), width);
        assert!(!line.contains("0.00"));
    }
    Ok(())
}

#[test]
fn voltage_strip_uses_generic_cjk_metadata_and_unknown_is_not_zero() -> Result<()> {
    let mut model = model_with_eight_adc_channels()?;
    model.voltage_channel_ids.reverse();
    let descriptor = model.adc_channel_metadata.get_mut("adc3").unwrap();
    descriptor.label = "参考电压".to_string();
    descriptor.note = "输入端测量".to_string();
    descriptor.signal = "".to_string();
    model.latest.get_mut("adc3").unwrap().signal.clear();
    model.latest.get_mut("adc3").unwrap().voltage_uv = None;
    let line = voltage_strip_line(&model, 120).to_string();
    assert!(line.starts_with("adc3=-- 参考电压 输入端测量"), "{line}");
    for width in [120, 80, 47, 32, 1] {
        let text = voltage_strip_line(&model, width).to_string();
        assert_eq!(UnicodeWidthStr::width(text.as_str()), width);
        assert!(!text.contains('\u{FFFD}'));
    }
    // A firmware-provided CJK name is still the identity, not its label or
    // GPIO signal. Display-column clipping keeps it inside its own cell.
    let mut descriptor = model.adc_channel_metadata.remove("adc3").unwrap();
    descriptor.name = "参考".to_string();
    model
        .adc_channel_metadata
        .insert("参考".to_string(), descriptor);
    let mut reading = model.latest.remove("adc3").unwrap();
    reading.name = "参考".to_string();
    model.latest.insert("参考".to_string(), reading);
    model.voltage_channel_ids[0] = "参考".to_string();
    for width in [32, 47, 80, 120] {
        let text = voltage_strip_line(&model, width).to_string();
        assert!(text.starts_with("参考=--"), "{text}");
        assert_eq!(UnicodeWidthStr::width(text.as_str()), width);
        assert!(!text.contains('\u{FFFD}'));
    }
    Ok(())
}

#[test]
fn voltage_strip_resize_keeps_selection_hit_boundaries_and_keybar() -> Result<()> {
    use super::controls::{control_items, ControlItem};
    use super::gpio_fixture::{control_rect, item_index};
    use super::runtime::{process_event, EventDisposition};
    use crossterm::event::Event;
    let mut model = model_with_eight_adc_channels()?;
    model.control_idx = item_index(&model, "GP8")?;
    for width in [120u16, 80, 47, 32, 80] {
        assert_eq!(
            process_event(
                &mut model,
                Event::Resize(width, 24),
                std::time::Instant::now()
            )?,
            EventDisposition::Redraw
        );
        assert!(model.hit_map.controls.is_empty());
        model.width = width as usize;
        let buffer = draw(&mut model, width, 24)?;
        let target = ControlItem::Gpio("GP8".to_string());
        assert_eq!(control_items(&model)[model.control_idx], target);
        let rect = control_rect(&model, "GP8")?;
        assert_eq!(model.hit_map.controls.at(rect.x, rect.y), Some(&target));
        assert!(rect.y >= 12 && rect.y < 23);
        for name in &model.voltage_channel_ids {
            assert!(row_text(&buffer, 9).contains(name));
        }
        for x in 0..width {
            assert!(model.hit_map.controls.at(x, 9).is_none());
            assert!(model.hit_map.controls.at(x, 23).is_none());
        }
        assert!(!row_text(&buffer, 23).trim().is_empty());
    }
    for height in [6u16, 7, 8] {
        let buffer = draw(&mut model, 32, height)?;
        assert!(!row_text(&buffer, height - 1).trim().is_empty());
        for (rect, _) in model.hit_map.controls.iter() {
            assert!(rect.y + rect.height < height);
        }
    }
    Ok(())
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

#[test]
fn scope_uses_snapshot_current_descriptors_including_vdd_and_new_channels() -> Result<()> {
    let mut model = TuiModel::new(DEFAULT_BASE_URL.to_string(), Duration::from_secs(2));
    let snapshot = serde_json::from_value(serde_json::json!({
        "adc_channels": [
            {"name": "vdd_5v", "sensor_channel": "current"},
            {"name": "adc3", "sensor_channel": "voltage"},
            {"name": "5v_out", "sensor_channel": "current"},
            {"name": "12v_out", "sensor_channel": "current"},
            {"name": "20v_out", "sensor_channel": "current"},
            {"name": "aux", "sensor_channel": "current"}
        ]
    }))?;
    model.apply_status_snapshot(snapshot);
    assert_eq!(
        model.channel_ids,
        ["vdd_5v", "5v_out", "12v_out", "20v_out", "aux"]
    );
    let mut readings: Vec<_> = model
        .channel_ids
        .iter()
        .map(|name| AdcReading {
            name: name.clone(),
            current_ua: Some(900_000),
            power_enabled: Some(true),
            ..Default::default()
        })
        .collect();
    readings.push(AdcReading {
        name: "adc3".to_string(),
        kind: crate::adc::AdcKind::Voltage,
        voltage_uv: Some(1_200_000),
        ..Default::default()
    });
    model.apply_adc_response(readings);

    let buffer = draw(&mut model, 180, 24)?;
    let header = row_text(&buffer, 2);
    for name in ["vdd_5v", "5v_out", "12v_out", "20v_out", "aux"] {
        assert!(header.contains(name), "header={header}");
    }
    assert!(header.starts_with("vdd_5v on 900mA"), "header={header}");
    assert!(!header.contains("adc3"));
    assert!(BAR_GLYPHS.contains(buffer[(0, 8)].symbol()));

    model.apply_status_snapshot(crate::ws_status::WsStatusSnapshot::default());
    let buffer = draw(&mut model, 180, 24)?;
    assert!(row_text(&buffer, 2).contains("Controls"));
    Ok(())
}
