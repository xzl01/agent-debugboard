use super::config_rows::build_saved_config_content;
use super::control_columns::{row_line, selection_style, RowLayout};
use super::control_rows::control_rows;
use super::controls::ControlItem;
use super::gpio_projection::{gpio_visual_rows, GpioCell, PAIR_MIN_WIDTH};
use super::model::TuiModel;
use super::pages::{clamp_scroll, ensure_visible, ActivePage};
use super::status_page::status_lines;
use ratatui::layout::Rect;
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span, Text};
use ratatui::widgets::Paragraph;

#[derive(Debug, Clone)]
pub(super) struct RowMark {
    pub(super) item: ControlItem,
    pub(super) line: usize,
    pub(super) x_start: usize,
    pub(super) x_end: usize,
}

#[derive(Debug, Default)]
pub(super) struct BodyContent {
    pub(super) lines: Vec<Line<'static>>,
    pub(super) marks: Vec<RowMark>,
    pub(super) selection_line: Option<usize>,
}

pub(super) fn build_body_content(model: &TuiModel, section_width: usize) -> BodyContent {
    match model.active_page {
        ActivePage::Controls => build_controls_content(model, section_width),
        ActivePage::SavedConfig => build_saved_config_page(model, section_width),
        ActivePage::Status => BodyContent {
            lines: status_lines(model, section_width),
            marks: Vec::new(),
            selection_line: None,
        },
    }
}

fn build_controls_content(model: &TuiModel, section_width: usize) -> BodyContent {
    let layout = RowLayout::new(section_width);
    let mut content = BodyContent::default();
    let mut index = 0usize;
    for row in control_rows(model).iter() {
        if matches!(row.item, ControlItem::Gpio(_)) {
            continue;
        }
        let selected = index == model.control_idx;
        push_row(
            &mut content,
            row_line(row, &layout, selected),
            row.item.clone(),
            0,
            section_width,
            selected,
        );
        index += 1;
    }
    for visual_row in gpio_visual_rows(model) {
        push_gpio_row(&mut content, model, &visual_row.cells, section_width);
    }
    content
}

fn push_row(
    content: &mut BodyContent,
    line: Line<'static>,
    item: ControlItem,
    x_start: usize,
    x_end: usize,
    selected: bool,
) {
    if selected {
        content.selection_line = Some(content.lines.len());
    }
    content.marks.push(RowMark {
        item,
        line: content.lines.len(),
        x_start,
        x_end,
    });
    content.lines.push(line);
}

fn push_gpio_row(content: &mut BodyContent, model: &TuiModel, cells: &[GpioCell], width: usize) {
    let paired = model.width >= PAIR_MIN_WIDTH;
    let half = width / 2;
    let mut spans = Vec::new();
    for (position, cell) in cells.iter().enumerate() {
        let selected = cell.item_index == model.control_idx;
        if selected {
            content.selection_line = Some(content.lines.len());
        }
        let (x_start, cell_width) = match (paired, position) {
            (true, 1) => (half, width - half),
            (true, _) => (0, half),
            (false, _) => (0, width),
        };
        spans.extend(cell_spans(cell, model, cell_width, selected));
        content.marks.push(RowMark {
            item: ControlItem::Gpio(cell.name.clone()),
            line: content.lines.len(),
            x_start,
            x_end: x_start + cell_width,
        });
    }
    content.lines.push(Line::from(spans));
}

fn cell_spans(
    cell: &GpioCell,
    model: &TuiModel,
    width: usize,
    selected: bool,
) -> Vec<Span<'static>> {
    let level = *model.gpio_levels.get(&cell.name).unwrap_or(&false);
    let is_input = *model.gpio_is_input.get(&cell.name).unwrap_or(&true);
    let (marker, direction, level_word) = match (is_input, level) {
        (true, false) => ("◌", "IN", "LOW"),
        (true, true) => ("◌", "IN", "HIGH"),
        (false, false) => ("○", "OUT", "LOW"),
        (false, true) => ("●", "OUT", "HIGH"),
    };
    let suffix = format!("{marker} {direction} {level_word}");
    let suffix_width = suffix.chars().count();
    let state_style = if level {
        Style::default().fg(Color::Red).add_modifier(Modifier::BOLD)
    } else {
        Style::default().fg(Color::DarkGray)
    };

    let core = format!("{} {}", cell.group, cell.label);
    let core_width = core.chars().count();
    let plain = selection_style(Style::default(), selected);
    let state = selection_style(state_style, selected);
    let muted = selection_style(Style::default().fg(Color::DarkGray), selected);

    let mut spans = Vec::new();
    if width > suffix_width {
        let core_budget = width - suffix_width - 1;
        let clipped_core: String = core.chars().take(core_budget).collect();
        let core_used = clipped_core.chars().count();
        spans.push(Span::styled(clipped_core, plain));
        spans.push(Span::styled(" ", plain));
        spans.push(Span::styled(suffix, state));
        let mut used = core_used + 1 + suffix_width;
        if core_used == core_width && !cell.note.is_empty() {
            let note = format!(" {}", cell.note);
            let take = note.chars().count().min(width - used);
            spans.push(Span::styled(
                note.chars().take(take).collect::<String>(),
                muted,
            ));
            used += take;
        }
        spans.push(Span::styled(" ".repeat(width - used), plain));
    } else {
        let clipped: String = format!("{core} {suffix}").chars().take(width).collect();
        spans.push(Span::styled(clipped, plain));
    }
    spans
}

fn build_saved_config_page(model: &TuiModel, section_width: usize) -> BodyContent {
    let config = build_saved_config_content(&model.saved_config, section_width);
    let selection_line = if model.saved_config.focused {
        config.item_anchors.get(model.saved_config.cursor).copied()
    } else {
        None
    };
    BodyContent {
        lines: config.lines,
        marks: Vec::new(),
        selection_line,
    }
}

pub(super) fn render_body(frame: &mut ratatui::Frame, area: Rect, model: &mut TuiModel) {
    let viewport = area.height as usize;
    let content = build_body_content(model, area.width as usize);

    let mut offset = clamp_scroll(model.page_scroll(), content.lines.len(), viewport);
    if let Some(selection) = content.selection_line {
        offset = ensure_visible(selection, offset, viewport, content.lines.len());
    }
    model.set_page_scroll(offset);

    for mark in &content.marks {
        if mark.line < offset {
            continue;
        }
        let visible_line = mark.line - offset;
        if visible_line >= viewport || mark.x_start >= area.width as usize {
            continue;
        }
        let x_end = mark.x_end.min(area.width as usize);
        let rect = Rect::new(
            area.x + mark.x_start as u16,
            area.y + visible_line as u16,
            (x_end - mark.x_start) as u16,
            1,
        );
        model.hit_map.push_control(rect, mark.item.clone());
    }

    let paragraph = Paragraph::new(Text::from(content.lines)).scroll((offset as u16, 0));
    frame.render_widget(paragraph, area);
}
