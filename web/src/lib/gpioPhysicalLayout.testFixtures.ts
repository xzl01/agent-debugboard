import type { GpioPhysicalCell, GpioPhysicalLayout, SafeGpio } from "./types.ts";

/** TEST ONLY: corrected direct connector nets, never production host mappings. */
export const directGpioFixtureCases = [
  { pinNumber: 16, pin: 5, gpioName: "GP5", signal: "GPIO5_CON" },
  { pinNumber: 15, pin: 6, gpioName: "GP6", signal: "GPIO6_CON" },
] as const;

/** TEST ONLY. API-shaped connector data, never a production map or live state. */
export function physicalLayoutFixture(): { layout: GpioPhysicalLayout; gpios: SafeGpio[] } {
  const gpios: SafeGpio[] = [];
  function controlled(pinNumber: number, row: number, column: number, pin: number, label: string, signal = `TEST_${label}`): GpioPhysicalCell {
    const name = `GP${pin}`;
    gpios.push({ name, pin, note: "test fixture", value: pin === 14 ? 1 : 0, direction: pin === 14 ? "output" : "input" });
    return { pinNumber, row, column, label, signal, role: pin === 29 ? "adc" : "gpio", gpioName: name, controllable: true, outputCapable: pin !== 29 };
  }
  const inert: Record<number, Pick<GpioPhysicalCell, "label" | "signal" | "role">> = {
    10: { label: "GP21 / SDA", signal: "RP2354_SDA", role: "i2c" },
    9: { label: "GP20 / SCL", signal: "RP2354_SCL", role: "i2c" },
    8: { label: "GP27 / ADC1", signal: "GPIO27_ADC1", role: "adc" },
    7: { label: "GP28 / ADC2", signal: "GPIO28_ADC2", role: "adc" },
    20: { label: "5V USB HUB", signal: "5V_USB_HUB", role: "power" },
    19: { label: "USB DM (hub)", signal: "U3_D3M_HUB_CON", role: "usb" },
    18: { label: "USB DP (hub)", signal: "U3_D3P_HUB_CON", role: "usb" },
    17: { label: "GND", signal: "GND", role: "ground" },
    14: { label: "GND", signal: "GND", role: "ground" },
    13: { label: "USB DM", signal: "MTT_USB2_DM", role: "usb" },
    12: { label: "USB DP", signal: "MTT_USB2_DP", role: "usb" },
    11: { label: "GND", signal: "GND", role: "ground" },
  };
  const j8: GpioPhysicalCell[] = Array.from({ length: 20 }, (_, index) => {
    const row = index % 10;
    const column = Math.floor(index / 10);
    const pinNumber = (column ? 20 : 10) - row;
    if (column === 0 && row >= 4) return controlled(pinNumber, row, column, row === 4 ? 29 : 21 - row, row === 4 ? "ADC3" : `GP${21 - row}`);
    const direct = column === 1 ? directGpioFixtureCases.find((entry) => entry.pinNumber === pinNumber) : undefined;
    if (direct) return controlled(pinNumber, row, column, direct.pin, direct.gpioName, direct.signal);
    return { pinNumber, row, column, ...inert[pinNumber], controllable: false, outputCapable: false };
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
