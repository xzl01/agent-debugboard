use super::text_width::{clip_display, display_width};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};

const COLUMN_GAP: usize = 2;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum ColumnKind {
    Sel,
    Id,
    Kind,
    Current,
    Saved,
    Risk,
    Apply,
}

impl ColumnKind {
    const fn title(self) -> &'static str {
        match self {
            Self::Sel => "SEL",
            Self::Id => "ID",
            Self::Kind => "KIND",
            Self::Current => "CURRENT",
            Self::Saved => "SAVED",
            Self::Risk => "RISK",
            Self::Apply => "APPLY",
        }
    }

    const fn width(self) -> usize {
        match self {
            Self::Sel => 4,
            Self::Id => 20,
            Self::Kind => 8,
            Self::Current => 12,
            Self::Saved => 12,
            Self::Risk => 6,
            Self::Apply => 9,
        }
    }
}

#[derive(Debug, Clone, Copy)]
pub(super) struct Column {
    pub(super) kind: ColumnKind,
    pub(super) width: usize,
}

const ALL_COLUMNS: [ColumnKind; 7] = [
    ColumnKind::Sel,
    ColumnKind::Id,
    ColumnKind::Kind,
    ColumnKind::Current,
    ColumnKind::Saved,
    ColumnKind::Risk,
    ColumnKind::Apply,
];

const DROP_ORDER: [ColumnKind; 5] = [
    ColumnKind::Apply,
    ColumnKind::Risk,
    ColumnKind::Saved,
    ColumnKind::Current,
    ColumnKind::Kind,
];

fn plan_width(plan: &[Column]) -> usize {
    let columns: usize = plan.iter().map(|column| column.width).sum();
    columns + COLUMN_GAP * plan.len().saturating_sub(1)
}

pub(super) fn column_plan(width: usize) -> Vec<Column> {
    let mut plan: Vec<Column> = ALL_COLUMNS
        .iter()
        .map(|kind| Column {
            kind: *kind,
            width: kind.width(),
        })
        .collect();
    for dropped in DROP_ORDER {
        if plan_width(&plan) <= width {
            return plan;
        }
        plan.retain(|column| column.kind != dropped);
    }
    if width > ColumnKind::Sel.width() + COLUMN_GAP {
        for column in &mut plan {
            if column.kind == ColumnKind::Id {
                column.width = width - ColumnKind::Sel.width() - COLUMN_GAP;
            }
        }
    } else {
        for column in &mut plan {
            column.width = match column.kind {
                ColumnKind::Sel => width.min(ColumnKind::Sel.width()),
                _ => width.saturating_sub(ColumnKind::Sel.width()),
            };
        }
    }
    plan
}

pub(super) fn clip(text: &str, width: usize) -> String {
    let clipped = clip_display(text, width);
    let padding = width.saturating_sub(display_width(&clipped));
    format!("{clipped}{:padding$}", "", padding = padding)
}

fn header_style() -> Style {
    Style::default()
        .fg(Color::DarkGray)
        .add_modifier(Modifier::BOLD)
}

pub(super) fn saved_config_header_line(width: usize) -> Line<'static> {
    let plan = column_plan(width);
    let mut spans = Vec::new();
    for (index, column) in plan.iter().enumerate() {
        if index > 0 {
            spans.push(Span::styled("  ", header_style()));
        }
        spans.push(Span::styled(
            clip(column.kind.title(), column.width),
            header_style(),
        ));
    }
    Line::from(spans)
}
