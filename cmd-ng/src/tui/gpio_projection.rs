use super::controls::{control_items, ControlItem};
use super::model::TuiModel;

pub(super) const FALLBACK_GROUP: &str = "GPIO";
pub(super) const PAIR_MIN_WIDTH: usize = 48;

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct GpioCell {
    pub(super) item_index: usize,
    pub(super) name: String,
    pub(super) group: String,
    pub(super) label: String,
    pub(super) note: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct GpioVisualRow {
    pub(super) cells: Vec<GpioCell>,
}

#[derive(Debug, Clone)]
struct PendingPin {
    item_index: usize,
    name: String,
    group: Option<String>,
    label: Option<String>,
    row: Option<u32>,
    column: Option<u32>,
    note: String,
}

impl PendingPin {
    fn is_complete(&self) -> bool {
        self.group.is_some() && self.label.is_some() && self.row.is_some() && self.column.is_some()
    }
}

fn pending_pins(model: &TuiModel) -> Vec<PendingPin> {
    let items = control_items(model);
    model
        .gpio_names
        .iter()
        .filter_map(|name| {
            let item_index = items
                .iter()
                .position(|item| matches!(item, ControlItem::Gpio(gpio) if gpio == name))?;
            let layout = model.gpio_layouts.get(name);
            Some(PendingPin {
                item_index,
                name: name.clone(),
                group: layout.and_then(|layout| layout.group.clone()),
                label: layout.and_then(|layout| layout.label.clone()),
                row: layout.and_then(|layout| layout.row),
                column: layout.and_then(|layout| layout.column),
                note: model.gpio_notes.get(name).cloned().unwrap_or_default(),
            })
        })
        .collect()
}

fn to_cell(pin: &PendingPin, group: &str, label: &str) -> GpioCell {
    GpioCell {
        item_index: pin.item_index,
        name: pin.name.clone(),
        group: group.to_string(),
        label: label.to_string(),
        note: pin.note.clone(),
    }
}

fn chunk_row(cells: Vec<GpioCell>, rows: &mut Vec<GpioVisualRow>) {
    for chunk in cells.chunks(2) {
        rows.push(GpioVisualRow {
            cells: chunk.to_vec(),
        });
    }
}

fn project_complete(
    pins: &[PendingPin],
    rows: &mut Vec<GpioVisualRow>,
    fallback: &mut Vec<PendingPin>,
) {
    let mut group_order: Vec<String> = Vec::new();
    for pin in pins {
        if let Some(group) = &pin.group {
            if !group_order.contains(group) {
                group_order.push(group.clone());
            }
        }
    }
    for group in &group_order {
        let members: Vec<&PendingPin> = pins
            .iter()
            .filter(|pin| pin.group.as_ref() == Some(group))
            .collect();
        let mut seen: Vec<(u32, u32)> = Vec::new();
        let mut kept = Vec::new();
        for pin in members {
            let coordinate = match (pin.row, pin.column) {
                (Some(row), Some(column)) => (row, column),
                _ => continue,
            };
            if seen.contains(&coordinate) {
                fallback.push(pin.clone());
            } else {
                seen.push(coordinate);
                kept.push(pin);
            }
        }
        let mut distinct: Vec<u32> = kept.iter().filter_map(|pin| pin.row).collect();
        distinct.sort_unstable();
        distinct.dedup();
        for firmware_row in distinct {
            let mut row_pins: Vec<&PendingPin> = kept
                .iter()
                .copied()
                .filter(|pin| pin.row == Some(firmware_row))
                .collect();
            row_pins.sort_by_key(|pin| (pin.column.unwrap_or(0), pin.item_index));
            let cells: Vec<GpioCell> = row_pins
                .iter()
                .map(|pin| {
                    let label = pin.label.as_deref().unwrap_or(&pin.name);
                    to_cell(pin, group, label)
                })
                .collect();
            chunk_row(cells, rows);
        }
    }
}

fn paired_rows(model: &TuiModel) -> Vec<GpioVisualRow> {
    let pins = pending_pins(model);
    let mut rows = Vec::new();
    let mut fallback: Vec<PendingPin> = Vec::new();
    let complete: Vec<PendingPin> = pins
        .iter()
        .filter(|pin| pin.is_complete())
        .cloned()
        .collect();
    fallback.extend(pins.iter().filter(|pin| !pin.is_complete()).cloned());
    project_complete(&complete, &mut rows, &mut fallback);
    fallback.sort_by_key(|pin| pin.item_index);
    let cells: Vec<GpioCell> = fallback
        .iter()
        .map(|pin| to_cell(pin, FALLBACK_GROUP, &pin.name))
        .collect();
    chunk_row(cells, &mut rows);
    rows
}

pub(super) fn gpio_visual_rows(model: &TuiModel) -> Vec<GpioVisualRow> {
    let paired = paired_rows(model);
    if model.width >= PAIR_MIN_WIDTH {
        return paired;
    }
    paired
        .into_iter()
        .flat_map(|row| {
            row.cells
                .into_iter()
                .map(|cell| GpioVisualRow { cells: vec![cell] })
                .collect::<Vec<_>>()
        })
        .collect()
}

pub(super) fn projected_lines(model: &TuiModel) -> Vec<Vec<usize>> {
    let mut lines: Vec<Vec<usize>> = control_items(model)
        .iter()
        .enumerate()
        .filter(|(_, item)| !matches!(item, ControlItem::Gpio(_)))
        .map(|(index, _)| vec![index])
        .collect();
    for row in gpio_visual_rows(model) {
        lines.push(row.cells.iter().map(|cell| cell.item_index).collect());
    }
    lines
}

pub(super) fn gpio_line_start(model: &TuiModel) -> usize {
    control_items(model)
        .iter()
        .filter(|item| !matches!(item, ControlItem::Gpio(_)))
        .count()
}
