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
}

const J13_ROWS = 2;
const J13_COLS = 2;
const J16_ROWS = 6;
const J16_COLS = 2;

function toLayoutCell(gpio: SafeGpio): GpioLayoutCell {
  return {
    pin: gpio.pin,
    name: gpio.name,
    note: gpio.note,
    value: gpio.value,
    direction: gpio.direction,
    layoutGroup: (gpio.layoutGroup ?? "").toUpperCase(),
    layoutLabel: gpio.layoutLabel ?? gpio.name,
    layoutRow: gpio.layoutRow ?? 0,
    layoutColumn: gpio.layoutColumn ?? 0,
  };
}

function emptyRow(columns: number): (GpioLayoutCell | null)[] {
  return Array.from({ length: columns }, () => null);
}

export function groupGpioLayout(gpios: SafeGpio[]): {
  j13: GpioLayoutGroup | null;
  j16: GpioLayoutGroup | null;
  fallback: SafeGpio[];
} {
  const j13Rows = new Map<number, (GpioLayoutCell | null)[]>();
  for (let r = 0; r < J13_ROWS; r++) j13Rows.set(r, emptyRow(J13_COLS));
  let j13Has = false;
  const j16Rows = new Map<number, (GpioLayoutCell | null)[]>();
  for (let r = 0; r < J16_ROWS; r++) j16Rows.set(r, emptyRow(J16_COLS));
  let j16Has = false;
  const fallback: SafeGpio[] = [];

  for (const raw of gpios) {
    const cell = toLayoutCell(raw);
    if (cell.layoutGroup === "J13" && cell.layoutRow < J13_ROWS && cell.layoutColumn < J13_COLS) {
      j13Rows.get(cell.layoutRow)![cell.layoutColumn] = cell;
      j13Has = true;
    } else if (cell.layoutGroup === "J16" && cell.layoutRow < J16_ROWS && cell.layoutColumn < J16_COLS) {
      const mirrored = { ...cell, layoutColumn: J16_COLS - 1 - cell.layoutColumn };
      j16Rows.get(mirrored.layoutRow)![mirrored.layoutColumn] = mirrored;
      j16Has = true;
    } else {
      fallback.push(raw);
    }
  }

  return {
    j13: j13Has
      ? {
          group: "J13",
          label: "J13",
          rows: j13Rows,
          rowCount: J13_ROWS,
          columnCount: J13_COLS,
        }
      : null,
    j16: j16Has
      ? {
          group: "J16",
          label: "J16",
          rows: j16Rows,
          rowCount: J16_ROWS,
          columnCount: J16_COLS,
        }
      : null,
    fallback,
  };
}

export const GPIO_PINOUT_CONSTANTS = {
  J13_ROWS,
  J13_COLS,
  J16_ROWS,
  J16_COLS,
};