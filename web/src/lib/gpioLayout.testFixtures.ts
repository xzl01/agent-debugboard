import type { GpioLayoutGroupMetadata, SafeGpio } from "./types.ts";
import { physicalLayoutFixture } from "./gpioPhysicalLayout.testFixtures.ts";

/** Test-only dynamic GPIO subset derived from the same current x1.2 fixture. */
export function x12GpioStatusFixture(): { gpio_layout_groups: GpioLayoutGroupMetadata[]; gpios: SafeGpio[] } {
  const { layout, gpios } = physicalLayoutFixture();
  const placements = layout.groups.flatMap((group) => group.cells.filter((cell) => cell.controllable)
    .map((cell) => ({ cell, group })));
  return {
    gpio_layout_groups: layout.groups.map(({ name, edge, order }) => ({ name, edge, order })),
    gpios: gpios.map((gpio) => {
      const { cell, group } = placements.find(({ cell }) => cell.gpioName === gpio.name)!;
      return { ...gpio, value: 0, direction: "input", note: `${group.name}_PIN${cell.pinNumber}`,
        layoutGroup: group.name, layoutLabel: cell.label, layoutRow: cell.row, layoutColumn: cell.column };
    }),
  };
}
