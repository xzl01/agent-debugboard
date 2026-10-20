import type { GpioPhysicalCell, GpioPhysicalLayout, SafeGpio } from "./types.ts";

/** TEST ONLY. Illustrative inert signals are NOT a production board pin map. */
export function physicalLayoutFixture(): { layout: GpioPhysicalLayout; gpios: SafeGpio[] } {
  const gpios: SafeGpio[] = [];
  function controlled(pinNumber: number, row: number, column: number, pin: number, label: string): GpioPhysicalCell {
    const name = `GP${pin}`;
    gpios.push({ name, pin, note: "test fixture", value: pin === 14 ? 1 : 0, direction: pin === 14 ? "output" : "input" });
    return { pinNumber, row, column, label, signal: `TEST_${label}`, role: pin === 29 ? "adc" : "gpio", gpioName: name, controllable: true, outputCapable: pin !== 29 };
  }
  const j8: GpioPhysicalCell[] = Array.from({ length: 20 }, (_, index) => {
    const row = index % 10;
    const column = Math.floor(index / 10);
    const pinNumber = (column ? 20 : 10) - row;
    if (column === 0 && row >= 4) return controlled(pinNumber, row, column, row === 4 ? 29 : 21 - row, row === 4 ? "ADC3" : `GP${21 - row}`);
    const role = ["power", "ground", "uart", "swd", "reserved"][index % 5];
    return { pinNumber, row, column, label: `TEST${pinNumber}`, signal: `TEST_${role.toUpperCase()}`, role, controllable: false, outputCapable: false };
  });
  const j13: GpioPhysicalCell[] = [
    controlled(2, 0, 0, 9, "USER"), controlled(3, 0, 1, 8, "RSET"), controlled(1, 1, 0, 7, "MASKROM"),
    { pinNumber: 4, row: 1, column: 1, label: "GND", signal: "GND", role: "ground", controllable: false, outputCapable: false },
  ];
  return { layout: { view: "component-top", groups: [
    { name: "J13", edge: "right", order: 1, rows: 2, columns: 2, cells: j13 },
    { name: "J8", edge: "right", order: 0, rows: 10, columns: 2, cells: j8 },
  ] }, gpios };
}

/** Synthetic fixture for pre-existing gesture tests; not a board map/fallback. */
export function layoutForTestGpios(gpios: readonly SafeGpio[]): GpioPhysicalLayout {
  return { view: "component-top", groups: gpios.length ? [{
    name: "TEST", edge: "right", order: 0, rows: gpios.length, columns: 1,
    cells: gpios.map((gpio, row) => ({ pinNumber: row + 1, row, column: 0, label: gpio.layoutLabel ?? `GP${gpio.pin}`, signal: gpio.name, role: "gpio", controllable: true, outputCapable: true, gpioName: gpio.name })),
  }] : [] };
}

export function layoutEnvelope(layout: GpioPhysicalLayout): unknown {
  return { schema: "radxa-linkr-debugger.v1", ok: true, command: "gpio", action: "layout", layout };
}
