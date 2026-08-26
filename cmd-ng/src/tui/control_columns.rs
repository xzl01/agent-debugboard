use super::control_rows::ControlRow;
use super::text_width::{clip_display, display_width};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};

const TYPE_WIDTH: usize = 6;
const NAME_WIDTH: usize = 12;
const STATE_WIDTH: usize = 14;
const LIVE_WIDTH: usize = 9;
const MODE_WIDTH: usize = 7;
const DESCRIPTION_MIN: usize = 8;
const COLUMN_GAP: usize = 2;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum ColumnKind {
    Type,
    Name,
    StateRoute,
    Live,
    Mode,
    Description,
}

impl ColumnKind {
    const fn title(self) -> &'static str {
        match self {
            Self::Type => "TYPE",
            Self::Name => "NAME",
            Self::StateRoute => "STATE-ROUTE",
            Self::Live => "LIVE",
            Self::Mode => "MODE",
            Self::Description => "DESCRIPTION",
        }
    }

    const fn right_aligned(self) -> bool {
        matches!(self, Self::Live)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct Column {
    pub(super) kind: ColumnKind,
    pub(super) width: usize,
}

impl Column {
    const fn new(kind: ColumnKind, width: usize) -> Self {
        Self { kind, width }
    }
}

fn plan_width(plan: &[Column]) -> usize {
    let columns: usize = plan.iter().map(|column| column.width).sum();
    columns + COLUMN_GAP * plan.len().saturating_sub(1)
}

pub(super) fn column_plan(width: usize) -> Vec<Column> {
    let mut plan = vec![
        Column::new(ColumnKind::Type, TYPE_WIDTH),
        Column::new(ColumnKind::Name, NAME_WIDTH),
        Column::new(ColumnKind::StateRoute, STATE_WIDTH),
        Column::new(ColumnKind::Live, LIVE_WIDTH),
        Column::new(ColumnKind::Mode, MODE_WIDTH),
        Column::new(ColumnKind::Description, DESCRIPTION_MIN),
    ];
    if plan_width(&plan) <= width {
        let description = width - (plan_width(&plan) - DESCRIPTION_MIN);
        if let Some(column) = plan.last_mut() {
            column.width = description;
        }
        return plan;
    }
    for dropped in [ColumnKind::Description, ColumnKind::Live, ColumnKind::Mode] {
        plan.retain(|column| column.kind != dropped);
        if plan_width(&plan) <= width {
            return plan;
        }
    }
    shrink_core_plan(plan, width)
}

fn shrink_core_plan(plan: Vec<Column>, width: usize) -> Vec<Column> {
    let prefix = TYPE_WIDTH + COLUMN_GAP + NAME_WIDTH + COLUMN_GAP;
    if width > prefix {
        return plan
            .into_iter()
            .map(|column| match column.kind {
                ColumnKind::StateRoute => Column::new(ColumnKind::StateRoute, width - prefix),
                _ => column,
            })
            .collect();
    }
    let mut core: Vec<Column> = plan
        .into_iter()
        .filter(|column| column.kind != ColumnKind::StateRoute)
        .collect();
    if width > TYPE_WIDTH + COLUMN_GAP {
        for column in &mut core {
            if column.kind == ColumnKind::Name {
                column.width = width - TYPE_WIDTH - COLUMN_GAP;
            }
        }
        return core;
    }
    core.retain(|column| column.kind != ColumnKind::Name);
    for column in &mut core {
        column.width = width;
    }
    core
}

pub(super) struct RowLayout {
    plan: Vec<Column>,
    width: usize,
}

impl RowLayout {
    pub(super) fn new(width: usize) -> Self {
        Self {
            plan: column_plan(width),
            width,
        }
    }
}

fn cell_text(row: &ControlRow, kind: ColumnKind) -> &str {
    match kind {
        ColumnKind::Type => row.kind,
        ColumnKind::Name => &row.name,
        ColumnKind::StateRoute => &row.state_route,
        ColumnKind::Live => &row.live,
        ColumnKind::Mode => &row.mode,
        ColumnKind::Description => &row.description,
    }
}

fn clip(text: &str, width: usize, right_aligned: bool) -> String {
    let clipped = clip_display(text, width);
    let padding = width.saturating_sub(display_width(&clipped));
    if right_aligned {
        format!("{:padding$}{clipped}", "", padding = padding)
    } else {
        format!("{clipped}{:padding$}", "", padding = padding)
    }
}

pub(super) fn row_line(row: &ControlRow, layout: &RowLayout, selected: bool) -> Line<'static> {
    let mut spans = Vec::new();
    for (index, column) in layout.plan.iter().enumerate() {
        if index > 0 {
            spans.push(Span::styled(
                "  ",
                selection_style(Style::default(), selected),
            ));
        }
        let text = clip(
            cell_text(row, column.kind),
            column.width,
            column.kind.right_aligned(),
        );
        let base = if column.kind == ColumnKind::StateRoute {
            row.tone.state_style()
        } else {
            Style::default()
        };
        spans.push(Span::styled(text, selection_style(base, selected)));
    }
    let used = plan_width(&layout.plan);
    if used < layout.width {
        spans.push(Span::styled(
            " ".repeat(layout.width - used),
            selection_style(Style::default(), selected),
        ));
    }
    Line::from(spans)
}

pub(super) fn header_line(layout: &RowLayout) -> Line<'static> {
    let header_style = || {
        Style::default()
            .fg(Color::DarkGray)
            .add_modifier(Modifier::BOLD)
    };
    let mut spans = Vec::new();
    for (index, column) in layout.plan.iter().enumerate() {
        if index > 0 {
            spans.push(Span::styled("  ", header_style()));
        }
        spans.push(Span::styled(
            clip(
                column.kind.title(),
                column.width,
                column.kind.right_aligned(),
            ),
            header_style(),
        ));
    }
    Line::from(spans)
}

pub(super) fn selection_style(base: Style, selected: bool) -> Style {
    if !selected {
        return base;
    }
    base.fg(base.fg.unwrap_or(Color::Black))
        .bg(Color::White)
        .add_modifier(Modifier::BOLD)
}
