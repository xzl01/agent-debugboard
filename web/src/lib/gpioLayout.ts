import type { SafeGpio } from "./types";

export interface GpioLayoutCell {
  pin: number;
  name: string;
  note: string;
  value: number;
  direction: string;
  layoutGroup: string;
  layoutLabel: string;
  layoutRow: number;
  layoutColumn: number;
}

export interface GpioLayoutGroup {
  group: string;
  label: string;
  rows: Map<number, (GpioLayoutCell | null)[]>;
  rowCount: number;
  columnCount: number;
  generic: boolean;
}

// Presentation-safety bound only: coordinates beyond this are treated as
// invalid firmware metadata, not as board geometry.
export const GPIO_LAYOUT_MAX_EXTENT = 64;

function toLayoutCell(gpio: SafeGpio, layoutGroup: string, layoutRow: number, layoutColumn: number): GpioLayoutCell {
  return {
    pin: gpio.pin,
    name: gpio.name,
    note: gpio.note,
    value: gpio.value,
    direction: gpio.direction,
    layoutGroup,
    layoutLabel: gpio.layoutLabel ?? gpio.name,
    layoutRow,
    layoutColumn,
  };
}

function toFallbackCell(gpio: SafeGpio): GpioLayoutCell {
  return {
    pin: gpio.pin,
    name: gpio.name,
    note: gpio.note,
    value: gpio.value,
    direction: gpio.direction,
    layoutGroup: (gpio.layoutGroup ?? "").toUpperCase(),
    layoutLabel: gpio.layoutLabel ?? gpio.name,
    layoutRow: 0,
    layoutColumn: 0,
  };
}

function isValidCoordinate(value: number | undefined): value is number {
  return (
    typeof value === "number" &&
    Number.isInteger(value) &&
    value >= 0 &&
    value < GPIO_LAYOUT_MAX_EXTENT
  );
}

function emptyRow(columns: number): (GpioLayoutCell | null)[] {
  return Array.from({ length: columns }, () => null);
}

export const GPIO_FALLBACK_GROUP_ID = "__gpio_fallback__";

export function groupGpioLayout(gpios: SafeGpio[]): {
  groups: GpioLayoutGroup[];
  fallback: GpioLayoutGroup | null;
} {
  const cellsByGroup = new Map<string, GpioLayoutCell[]>();
  const occupied = new Map<string, Set<string>>();
  const fallbackCells: GpioLayoutCell[] = [];

  for (const raw of gpios) {
    const layoutGroup = (raw.layoutGroup ?? "").trim().toUpperCase();
    if (!layoutGroup || !isValidCoordinate(raw.layoutRow) || !isValidCoordinate(raw.layoutColumn)) {
      fallbackCells.push(toFallbackCell(raw));
      continue;
    }
    const cellKey = `${raw.layoutRow}:${raw.layoutColumn}`;
    let occupiedCells = occupied.get(layoutGroup);
    if (!occupiedCells) {
      occupiedCells = new Set();
      occupied.set(layoutGroup, occupiedCells);
    }
    if (occupiedCells.has(cellKey)) {
      fallbackCells.push(toFallbackCell(raw));
      continue;
    }
    occupiedCells.add(cellKey);
    let cells = cellsByGroup.get(layoutGroup);
    if (!cells) {
      cells = [];
      cellsByGroup.set(layoutGroup, cells);
    }
    cells.push(toLayoutCell(raw, layoutGroup, raw.layoutRow, raw.layoutColumn));
  }

  const groups: GpioLayoutGroup[] = [];
  for (const [group, cells] of cellsByGroup) {
    let rowCount = 0;
    let columnCount = 0;
    for (const cell of cells) {
      rowCount = Math.max(rowCount, cell.layoutRow + 1);
      columnCount = Math.max(columnCount, cell.layoutColumn + 1);
    }
    const rows = new Map<number, (GpioLayoutCell | null)[]>();
    for (let r = 0; r < rowCount; r++) rows.set(r, emptyRow(columnCount));
    for (const cell of cells) {
      const row = rows.get(cell.layoutRow) ?? emptyRow(columnCount);
      rows.set(cell.layoutRow, row);
      row[cell.layoutColumn] = cell;
    }
    groups.push({ group, label: group, rows, rowCount, columnCount, generic: false });
  }

  const fallback = fallbackCells.length
    ? {
        group: GPIO_FALLBACK_GROUP_ID,
        label: GPIO_FALLBACK_GROUP_ID,
        rows: new Map(fallbackCells.map((cell, index) => [index, [cell]])),
        rowCount: fallbackCells.length,
        columnCount: 1,
        generic: true,
      }
    : null;

  return { groups, fallback };
}
