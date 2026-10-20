use super::model::{current_milliamp_estimate, TuiModel};
use super::text_width::{clip_display, display_width};
use ratatui::layout::Rect;
use ratatui::style::{Color, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::Paragraph;

const GRAPH_ROWS: usize = 6;
const SCOPE_ROWS: usize = 1 + GRAPH_ROWS;
const SCOPE_MIN_HEIGHT: u16 = 24;
const GLYPHS: [char; 7] = ['▁', '▂', '▃', '▄', '▅', '▆', '▇'];

fn channel_has_data(model: &TuiModel, channel: &str) -> bool {
    model.latest.contains_key(channel)
        || model
            .history
            .get(channel)
            .is_some_and(|series| !series.is_empty())
}

fn live_channels(model: &TuiModel) -> Vec<String> {
    model
        .channel_ids
        .iter()
        .filter(|channel| channel_has_data(model, channel))
        .cloned()
        .collect()
}

pub(super) fn telemetry_rows(model: &TuiModel, height: u16) -> usize {
    if height >= SCOPE_MIN_HEIGHT && !live_channels(model).is_empty() {
        SCOPE_ROWS
    } else {
        0
    }
}

pub(super) fn voltage_rows(model: &TuiModel, height: u16) -> usize {
    // Keep the existing status/tabs/header/body/keybar minimum of six rows.
    usize::from(height >= 7 && !model.voltage_channel_ids.is_empty())
}

pub(super) fn voltage_strip_line(model: &TuiModel, width: usize) -> Line<'static> {
    if model.voltage_channel_ids.is_empty() || width == 0 {
        return Line::default();
    }
    let (widths, gutter) = column_widths(width, model.voltage_channel_ids.len());
    let mut spans = Vec::new();
    for (index, name) in model.voltage_channel_ids.iter().enumerate() {
        let descriptor = model.adc_channel_metadata.get(name);
        let reading = model
            .latest
            .get(name)
            .filter(|reading| reading.kind == crate::adc::AdcKind::Voltage);
        let signal = descriptor
            .map(|channel| channel.signal.as_str())
            .filter(|signal| !signal.is_empty())
            .or_else(|| reading.map(|reading| reading.signal.as_str()))
            .unwrap_or_default();
        let label = descriptor
            .map(|channel| channel.label.as_str())
            .filter(|label| !label.is_empty())
            .or_else(|| reading.map(|reading| reading.label.as_str()))
            .unwrap_or_default();
        let note = reading
            .map(|reading| reading.note.as_str())
            .filter(|note| !note.is_empty())
            .or_else(|| descriptor.map(|channel| channel.note.as_str()))
            .unwrap_or_default();
        let uv = reading
            .filter(|reading| reading.voltage_valid != Some(false))
            .and_then(|reading| reading.voltage_uv);
        let budget = widths[index];
        // The descriptor name is the stable identity. Never trade its suffix
        // or numeric precision for a long (often shared-prefix) signal/label.
        let name_width = display_width(name);
        let value_budget = budget.saturating_sub(name_width + 1);
        let value = uv
            .map(|uv| {
                let mut value = (0..=6)
                    .rev()
                    .map(|precision| format!("{:.precision$}", f64::from(uv) / 1_000_000.0))
                    .find(|value| display_width(value) <= value_budget)
                    .unwrap_or_default();
                if !value.is_empty() && display_width(&value) < value_budget {
                    value.push('V');
                }
                value
            })
            .unwrap_or_else(|| "--".to_string());
        let mut remaining = budget;
        remaining = push_clipped(&mut spans, name.clone(), Style::default(), remaining);
        if name_width < budget && !value.is_empty() {
            remaining = push_clipped(&mut spans, "=".to_string(), Style::default(), remaining);
            remaining = push_clipped(
                &mut spans,
                value,
                Style::default().fg(Color::Cyan),
                remaining,
            );
        }
        if !signal.is_empty() && signal != name && display_width(signal) < remaining {
            remaining = push_clipped(
                &mut spans,
                format!(" {signal}"),
                Style::default().fg(Color::DarkGray),
                remaining,
            );
        }
        for detail in [label, note] {
            if !detail.is_empty() && detail != name && detail != signal {
                remaining = push_clipped(
                    &mut spans,
                    format!(" {detail}"),
                    Style::default().fg(Color::DarkGray),
                    remaining,
                );
            }
        }
        spans.push(Span::raw(" ".repeat(remaining)));
        if index + 1 < widths.len() && gutter > 0 {
            spans.push(Span::raw(" "));
        }
    }
    Line::from(spans)
}

pub(super) fn render_voltage_strip(frame: &mut ratatui::Frame, area: Rect, model: &TuiModel) {
    if area.height > 0 {
        frame.render_widget(
            Paragraph::new(voltage_strip_line(model, area.width as usize)),
            area,
        );
    }
}

fn column_widths(total: usize, count: usize) -> (Vec<usize>, usize) {
    let gutter_count = count.saturating_sub(1);
    let gutter_width = usize::from(gutter_count > 0 && total >= count.saturating_add(gutter_count));
    let content = total - gutter_width * gutter_count;
    let base = content / count;
    let remainder = content % count;
    let widths = (0..count)
        .map(|index| if index < remainder { base + 1 } else { base })
        .collect();
    (widths, gutter_width)
}

fn channel_scale(model: &TuiModel, channel: &str, window: &[i32]) -> i64 {
    let visible_peak = window.iter().map(|value| (*value).max(0)).max();
    let latest = model
        .latest
        .get(channel)
        .map(|reading| current_milliamp_estimate(reading).max(0));
    let peak = visible_peak
        .into_iter()
        .chain(latest)
        .max()
        .unwrap_or(0)
        .max(1) as i64;
    peak.saturating_mul(5).saturating_add(3).saturating_div(4)
}

fn graph_column(model: &TuiModel, channel: &str, width: usize) -> (Vec<char>, i64) {
    let window: Vec<i32> = model
        .history
        .get(channel)
        .map(|series| {
            let start = series.len().saturating_sub(width);
            series[start..].to_vec()
        })
        .unwrap_or_default();
    let scale = channel_scale(model, channel, &window);
    let mut grid = vec![' '; GRAPH_ROWS * width];
    for (x, sample) in window.iter().enumerate() {
        let clamped = (*sample).max(0) as i64;
        let eighths = clamped * (GRAPH_ROWS as i64) * 8 / scale;
        let full = eighths / 8;
        let partial = eighths % 8;
        for row in 0..full.min(GRAPH_ROWS as i64) {
            grid[(GRAPH_ROWS - 1 - row as usize) * width + x] = '█';
        }
        if partial > 0 && full < GRAPH_ROWS as i64 {
            grid[(GRAPH_ROWS - 1 - full as usize) * width + x] = GLYPHS[(partial - 1) as usize];
        }
    }
    (grid, scale)
}

fn push_clipped(
    spans: &mut Vec<Span<'static>>,
    text: String,
    style: Style,
    budget: usize,
) -> usize {
    let clipped = clip_display(&text, budget);
    let used = display_width(&clipped);
    if used > 0 {
        spans.push(Span::styled(clipped, style));
    }
    budget - used
}

pub(super) fn render_telemetry(frame: &mut ratatui::Frame, area: Rect, model: &TuiModel) {
    let channels = live_channels(model);
    if channels.is_empty() || area.height == 0 {
        return;
    }
    let (widths, gutter_width) = column_widths(area.width as usize, channels.len());

    let mut header_spans = Vec::new();
    let mut grids = Vec::new();
    for (index, channel) in channels.iter().enumerate() {
        let width = widths[index];
        let reading = model.latest.get(channel);
        let (power_text, power_style) = reading.and_then(|reading| reading.power_enabled).map_or(
            ("?", Style::default()),
            |enabled| {
                if enabled {
                    ("on", Style::default().fg(Color::Green))
                } else {
                    ("off", Style::default().fg(Color::DarkGray))
                }
            },
        );
        let current_ma = reading.map(current_milliamp_estimate).unwrap_or(0);
        let voltage_text = reading
            .and_then(|reading| reading.voltage_uv)
            .map(|uv| format!(" {:.2}V", uv as f64 / 1_000_000.0))
            .unwrap_or_default();
        let (grid, scale) = graph_column(model, channel, width);
        grids.push(grid);

        let mut budget = width;
        budget = push_clipped(&mut header_spans, channel.clone(), Style::default(), budget);
        budget = push_clipped(
            &mut header_spans,
            format!(" {power_text}"),
            power_style,
            budget,
        );
        budget = push_clipped(
            &mut header_spans,
            format!(" {current_ma}mA"),
            Style::default(),
            budget,
        );
        budget = push_clipped(
            &mut header_spans,
            voltage_text,
            Style::default().fg(Color::Cyan),
            budget,
        );
        budget = push_clipped(
            &mut header_spans,
            format!(" max={scale}mA"),
            Style::default().fg(Color::DarkGray),
            budget,
        );
        header_spans.push(Span::raw(" ".repeat(budget)));
        if index + 1 < channels.len() && gutter_width > 0 {
            header_spans.push(Span::raw(" "));
        }
    }

    let mut lines: Vec<Line<'static>> = vec![Line::from(header_spans)];
    for row in 0..GRAPH_ROWS {
        let mut spans = Vec::new();
        for (index, grid) in grids.iter().enumerate() {
            let width = widths[index];
            let text: String = grid[row * width..(row + 1) * width].iter().collect();
            spans.push(Span::raw(text));
            if index + 1 < grids.len() && gutter_width > 0 {
                spans.push(Span::raw(" "));
            }
        }
        lines.push(Line::from(spans));
    }

    frame.render_widget(Paragraph::new(lines), area);
}
