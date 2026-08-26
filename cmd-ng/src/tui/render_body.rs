use super::config_rows::build_saved_config_content;
use super::control_columns::{row_line, selection_style, RowLayout};
use super::control_rows::{control_rows, RowTone};
use super::controls::ControlItem;
use super::gpio_io::GpioAction;
use super::gpio_projection::{gpio_visual_rows, GpioCell, PAIR_MIN_WIDTH};
use super::hit_types::SavedConfigRowTarget;
use super::model::TuiModel;
use super::pages::{clamp_scroll, ensure_visible, ActivePage};
use super::render_body_hits::{register_body_hits, BodyViewport};
use super::status_page::status_lines;
use super::text_width::{clip_display, display_width};
use ratatui::layout::Rect;
use ratatui::style::{Color, Style};
use ratatui::text::{Line, Span, Text};
use ratatui::widgets::Paragraph;

#[derive(Debug, Clone)]
pub(super) enum BodyTarget {
    Control(ControlItem),
    SavedConfig(SavedConfigRowTarget),
}

#[derive(Debug, Clone)]
pub(super) struct RowMark {
    pub(super) target: BodyTarget,
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
        target: BodyTarget::Control(item),
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
            target: BodyTarget::Control(ControlItem::Gpio(cell.name.clone())),
            line: content.lines.len(),
            x_start,
            x_end: x_start + cell_width,
        });
    }
    content.lines.push(Line::from(spans));
}

fn pending_tag(model: &TuiModel, name: &str) -> Option<&'static str> {
    let pending = model.gpio_pending.as_ref()?;
    if pending.target != name {
        return None;
    }
    Some(match pending.action {
        GpioAction::DriveLow => "[LOW…]",
        GpioAction::DriveHigh => "[HIGH…]",
        GpioAction::SetInput => "[INPUT…]",
    })
}

fn cell_tag(model: &TuiModel, name: &str) -> Option<&'static str> {
    if let Some(tag) = pending_tag(model, name) {
        return Some(tag);
    }
    if model.gpio_gesture.holding_pin() == Some(name) {
        return Some("[HOLD…]");
    }
    None
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
    let suffix_width = display_width(&suffix);
    let tag = cell_tag(model, &cell.name);
    let tag_width = tag.map_or(0, |text| display_width(text) + 1);
    // Contract: state suffix > pending action tag > hold tag > group/label >
    // note. A clipped pending action remains in the status line; HOLD drops.
    let tag = tag.filter(|_| width >= suffix_width + tag_width);
    let tail_width = suffix_width + tag.map_or(0, |text| display_width(text) + 1);
    let state_style = if level {
        RowTone::GpioHigh.state_style()
    } else {
        RowTone::GpioLow.state_style()
    };

    let core = format!("{} {}", cell.group, cell.label);
    let core_width = display_width(&core);
    let plain = selection_style(Style::default(), selected);
    let state = selection_style(state_style, selected);
    let pending = selection_style(RowTone::GpioPending.state_style(), selected);
    let muted = selection_style(Style::default().fg(Color::DarkGray), selected);

    let mut spans = Vec::new();
    if width >= tail_width {
        let mut used = 0usize;
        let mut core_used = 0usize;
        if width > tail_width {
            let core_budget = width - tail_width - 1;
            let clipped_core = clip_display(&core, core_budget);
            core_used = display_width(&clipped_core);
            spans.push(Span::styled(clipped_core, plain));
            spans.push(Span::styled(" ", plain));
            used += core_used + 1;
        }
        spans.push(Span::styled(suffix, state));
        used += suffix_width;
        if let Some(tag) = tag {
            spans.push(Span::styled(format!(" {tag}"), pending));
            used += display_width(tag) + 1;
        }
        if core_used == core_width && !cell.note.is_empty() {
            let note = format!(" {}", cell.note);
            let clipped_note = clip_display(&note, width - used);
            let note_used = display_width(&clipped_note);
            if note_used > 0 {
                spans.push(Span::styled(clipped_note, muted));
                used += note_used;
            }
        }
        spans.push(Span::styled(" ".repeat(width - used), plain));
    } else {
        let clipped = clip_display(&format!("{core} {suffix}"), width);
        let used = display_width(&clipped);
        spans.push(Span::styled(clipped, plain));
        spans.push(Span::styled(" ".repeat(width - used), plain));
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
    let marks = config
        .item_anchors
        .iter()
        .zip(&model.saved_config.items)
        .map(|(line, item)| RowMark {
            target: BodyTarget::SavedConfig(SavedConfigRowTarget(item.id.clone())),
            line: *line,
            x_start: 0,
            x_end: section_width,
        })
        .collect();
    BodyContent {
        lines: config.lines,
        marks,
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

    register_body_hits(BodyViewport { area, offset }, &content, model);

    let paragraph = Paragraph::new(Text::from(content.lines)).scroll((offset as u16, 0));
    frame.render_widget(paragraph, area);
}
