use super::model::{current_milliamp_estimate, TuiModel};
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

fn column_widths(total: usize, count: usize) -> Vec<usize> {
    let base = total / count;
    let remainder = total % count;
    (0..count)
        .map(|index| if index < remainder { base + 1 } else { base })
        .collect()
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
    let take = budget.min(text.chars().count());
    if take > 0 {
        let clipped: String = text.chars().take(take).collect();
        spans.push(Span::styled(clipped, style));
    }
    budget - take
}

pub(super) fn render_telemetry(frame: &mut ratatui::Frame, area: Rect, model: &TuiModel) {
    let channels = live_channels(model);
    if channels.is_empty() || area.height == 0 {
        return;
    }
    let widths = column_widths(area.width as usize, channels.len());

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
            format!(" max={scale}mA"),
            Style::default().fg(Color::DarkGray),
            budget,
        );
        header_spans.push(Span::raw(" ".repeat(budget)));
    }

    let mut lines: Vec<Line<'static>> = vec![Line::from(header_spans)];
    for row in 0..GRAPH_ROWS {
        let spans: Vec<Span<'static>> = grids
            .iter()
            .enumerate()
            .map(|(index, grid)| {
                let width = widths[index];
                let text: String = grid[row * width..(row + 1) * width].iter().collect();
                Span::raw(text)
            })
            .collect();
        lines.push(Line::from(spans));
    }

    frame.render_widget(Paragraph::new(lines), area);
}
