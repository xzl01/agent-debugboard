import type { GpioLayoutGroupMetadata, SafeGpio } from "./types.ts";

// Test-only x1.2 metadata, confirmed against the board pin numbering. These
// coordinates are inputs to the renderer, never a host-side board map.
export function x12GpioStatusFixture(): {
  gpio_layout_groups: GpioLayoutGroupMetadata[];
  gpios: SafeGpio[];
} {
  function pin(pin: number, label: string, note: string, group: string, row: number, column: number): SafeGpio {
    return {
      name: `GP${pin}`, pin, note, value: 0, direction: "input",
      layoutGroup: group, layoutLabel: label, layoutRow: row, layoutColumn: column,
    };
  }

  return {
    // Deliberately not display-sorted, so tests must consume explicit order.
    gpio_layout_groups: [
      { name: "J13", edge: "right", order: 1 },
      { name: "J8", edge: "right", order: 0 },
    ],
    gpios: [
      pin(9, "USER", "CON_USER", "J13", 0, 0),
      pin(8, "RSET", "CON_REST", "J13", 0, 1),
      pin(7, "MASKROM", "CON_MAS", "J13", 1, 0),
      // J8: left top-to-bottom 10..1, right 20..11. Only the reported
      // controllable left-column subset belongs in this illustration.
      pin(12, "GP12", "J8_PIN1", "J8", 9, 0),
      pin(13, "GP13", "J8_PIN2", "J8", 8, 0),
      pin(14, "GP14", "J8_PIN3", "J8", 7, 0),
      pin(15, "GP15", "J8_PIN4", "J8", 6, 0),
      pin(16, "GP16", "J8_PIN5", "J8", 5, 0),
      pin(29, "ADC3", "J8_PIN6", "J8", 4, 0),
      pin(17, "GP17", "", "", 0, 0),
      pin(18, "GP18", "", "", 0, 0),
    ],
  };
}
