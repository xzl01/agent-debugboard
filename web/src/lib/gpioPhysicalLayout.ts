import type { GpioPhysicalCell, GpioPhysicalLayout, SafeGpio } from "./types.ts";

// Presentation safety limits, not connector dimensions or board pin maps.
const MAX_EXTENT = 64;
const MAX_GROUPS = 64;

function invalid(detail: string): never {
  throw new Error(`Invalid GPIO physical layout: ${detail}`);
}

function record(value: unknown): Record<string, unknown> {
  if (typeof value !== "object" || value === null || Array.isArray(value)) return invalid("expected an object");
  return value as Record<string, unknown>;
}

function text(value: unknown, field: string, empty = false): string {
  if (typeof value !== "string" || (!empty && !value.trim())) return invalid(field);
  return value;
}

function integer(value: unknown, field: string, min = 0, max = Number.MAX_SAFE_INTEGER): number {
  if (typeof value !== "number" || !Number.isSafeInteger(value) || value < min || value > max) return invalid(field);
  return value;
}

export function parseGpioPhysicalLayout(response: unknown): GpioPhysicalLayout {
  const envelope = record(response);
  if (envelope.schema !== "radxa-linkr-debugger.v1" || envelope.ok !== true || envelope.command !== "gpio" || envelope.action !== "layout") {
    return invalid("response envelope");
  }
  const layout = record(envelope.layout);
  if (layout.view !== "component-top") return invalid("view");
  if (!Array.isArray(layout.groups) || layout.groups.length > MAX_GROUPS) return invalid("groups");
  const names = new Set<string>();
  const orders = new Set<number>();
  const gpioNames = new Set<string>();
  const groups = layout.groups.map((raw) => {
    const group = record(raw);
    const name = text(group.name, "group name");
    const order = integer(group.order, "group order");
    if (names.has(name.trim().toUpperCase()) || orders.has(order)) return invalid("duplicate group name/order");
    names.add(name.trim().toUpperCase());
    orders.add(order);
    const rows = integer(group.rows, "rows", 1, MAX_EXTENT);
    const columns = integer(group.columns, "columns", 1, MAX_EXTENT);
    if (!Array.isArray(group.cells) || group.cells.length !== rows * columns) return invalid("incomplete matrix");
    const coordinates = new Set<string>();
    const pins = new Set<number>();
    const cells: GpioPhysicalCell[] = group.cells.map((rawCell) => {
      const cell = record(rawCell);
      const row = integer(cell.row, "row", 0, rows - 1);
      const column = integer(cell.column, "column", 0, columns - 1);
      const pinNumber = integer(cell.pinNumber, "pinNumber", 1);
      const coordinate = `${row}:${column}`;
      if (coordinates.has(coordinate) || pins.has(pinNumber)) return invalid("duplicate coordinate/pinNumber");
      coordinates.add(coordinate);
      pins.add(pinNumber);
      if (typeof cell.controllable !== "boolean" || typeof cell.outputCapable !== "boolean") return invalid("capability flags");
      if (cell.outputCapable && !cell.controllable) return invalid("output without control");
      const gpioName = cell.gpioName === undefined ? undefined : text(cell.gpioName, "gpioName");
      if (cell.controllable) {
        if (!gpioName || gpioNames.has(gpioName)) return invalid("missing/duplicate controllable gpioName");
        gpioNames.add(gpioName);
      }
      return {
        pinNumber, row, column,
        label: text(cell.label, "label"),
        signal: text(cell.signal, "signal", true),
        role: text(cell.role, "role"),
        controllable: cell.controllable,
        outputCapable: cell.outputCapable,
        ...(gpioName === undefined ? {} : { gpioName }),
      };
    });
    return { name, order, rows, columns, edge: text(group.edge, "edge"), cells };
  });
  return { view: "component-top", groups: groups.sort((a, b) => a.order - b.order) };
}

/** A bijection by firmware name, never by MCU number or physical pin number. */
export function validateGpioPhysicalJoin(layout: GpioPhysicalLayout, gpios: readonly SafeGpio[]): void {
  const dynamic = new Set<string>();
  for (const gpio of gpios) {
    if (!gpio.name || dynamic.has(gpio.name)) return invalid("duplicate/empty live GPIO name");
    dynamic.add(gpio.name);
  }
  const controls = layout.groups.flatMap((group) => group.cells.filter((cell) => cell.controllable));
  const matched = new Set<string>();
  for (const cell of controls) {
    if (!cell.gpioName || !dynamic.has(cell.gpioName) || matched.has(cell.gpioName)) return invalid("unmatched controllable cell");
    matched.add(cell.gpioName);
  }
  if (dynamic.size !== matched.size) return invalid("unmatched live GPIO");
}
