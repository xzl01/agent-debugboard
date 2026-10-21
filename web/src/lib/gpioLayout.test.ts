import assert from "node:assert/strict";
import test from "node:test";
import type { SafeGpio } from "./types.ts";
import { GPIO_LAYOUT_MAX_EXTENT, groupGpioLayout } from "./gpioLayout.ts";
import { mapBoardStatus } from "./boardSnapshot.ts";
import { x12GpioStatusFixture } from "./gpioLayout.testFixtures.ts";

function pin(pin: number, group: string, row: number, col: number): SafeGpio {
  return {
    name: `GP${pin}`,
    pin,
    note: `${group}_${row}_${col}`,
    value: 0,
    direction: "input",
    layoutGroup: group,
    layoutLabel: `L${row}${col}`,
    layoutRow: row,
    layoutColumn: col,
  };
}

function barePin(pin: number, extra?: Partial<SafeGpio>): SafeGpio {
  return { name: `GP${pin}`, pin, note: "n", value: 0, direction: "input", ...extra };
}

function memberPins(result: ReturnType<typeof groupGpioLayout>): number[] {
  const pins: number[] = [];
  const all = result.fallback ? [...result.groups, result.fallback] : result.groups;
  for (const group of all) {
    for (const row of group.rows.values()) {
      for (const cell of row) {
        if (cell) pins.push(cell.pin);
      }
    }
  }
  return pins;
}

test("groupGpioLayout preserves all eleven current x1.2 J8/J13 GPIOs without mirroring or obsolete fallback pins", () => {
  const snapshot = mapBoardStatus(x12GpioStatusFixture(), []);
  const result = groupGpioLayout(snapshot.gpios, snapshot.gpioLayoutGroups);
  assert.equal(result.groups.length, 2);
  const j8 = result.groups[0]!;
  const j13 = result.groups[1]!;
  assert.equal(j8.group, "J8");
  assert.equal(j13.group, "J13");
  assert.equal(j8.edge, "right");
  assert.equal(j13.edge, "right");
  assert.equal(j8.rowCount, 10);
  assert.equal(j8.columnCount, 2, "the advertised direct GPIOs populate both columns");
  for (const [row, pin, note] of [
    [9, 12, "J8_PIN1"], [8, 13, "J8_PIN2"], [7, 14, "J8_PIN3"],
    [6, 15, "J8_PIN4"], [5, 16, "J8_PIN5"], [4, 29, "J8_PIN6"],
  ] as const) {
    assert.equal(j8.rows.get(row)?.[0]?.pin, pin);
    assert.equal(j8.rows.get(row)?.[0]?.note, note);
  }
  for (let row = 0; row < 4; row++) assert.deepEqual(j8.rows.get(row), [null, null]);
  assert.equal(j8.rows.get(4)?.[1]?.pin, 5);
  assert.equal(j8.rows.get(5)?.[1]?.pin, 6);
  assert.equal(j13.rows.get(0)?.[0]?.pin, 9);
  assert.equal(j13.rows.get(0)?.[1]?.pin, 8);
  assert.equal(j13.rows.get(1)?.[0]?.pin, 7);
  assert.equal(j13.rows.get(1)?.[1], null, "J13 pin4 GND is not a GPIO control");
  assert.equal(result.fallback, null);
  assert.equal(snapshot.gpios.length, 11);
  assert.equal(snapshot.gpios.some((gpio) => gpio.pin === 17 || gpio.pin === 18), false);
  assert.deepEqual(memberPins(result).sort((a, b) => a - b), snapshot.gpios.map((pin) => pin.pin).sort((a, b) => a - b));
});

test("groupGpioLayout derives group order from first firmware occurrence", () => {
  const gpios: SafeGpio[] = [pin(10, "J16", 0, 0), pin(8, "J13", 0, 1), pin(11, "J16", 1, 0)];
  const result = groupGpioLayout(gpios);
  assert.deepEqual(
    result.groups.map((g) => g.group),
    ["J16", "J13"]
  );
});

test("groupGpioLayout applies explicit group order and edge to arbitrary names", () => {
  const gpios = [pin(1, "UNLISTED", 0, 0), pin(2, "J13", 0, 0), pin(3, "J8", 0, 1), pin(4, "PORT", 1, 0)];
  const result = groupGpioLayout(gpios, [
    { name: "J13", edge: "right", order: 1 },
    { name: "J8", edge: "right", order: 0 },
    { name: " port ", edge: "right", order: 2 },
    { name: "ABSENT", edge: "right", order: 3 },
  ]);
  assert.deepEqual(result.groups.map((group) => group.group), ["J8", "J13", "PORT", "UNLISTED"]);
  assert.deepEqual(result.groups.map((group) => group.edge), ["right", "right", "right", undefined]);
  assert.equal(result.groups[0]?.rows.get(0)?.[0], null);
  assert.equal(result.groups[0]?.rows.get(0)?.[1]?.pin, 3);
  assert.equal(result.groups[2]?.rowCount, 2, "metadata never supplies full connector dimensions");
});

test("groupGpioLayout keeps missing or unknown edges generic and ties stable", () => {
  const gpios = [pin(1, "J13", 0, 0), pin(2, "J8", 0, 0), pin(3, "OTHER", 0, 0)];
  assert.ok(groupGpioLayout(gpios).groups.every((group) => group.edge === undefined));
  const result = groupGpioLayout(gpios, [
    { name: "J13", edge: "future", order: 0 },
    { name: "J8", edge: "", order: 0 },
    { name: "OTHER", edge: "right", order: -1 },
  ]);
  assert.deepEqual(result.groups.map((group) => group.group), ["J13", "J8", "OTHER"]);
  assert.ok(result.groups.every((group) => group.edge === undefined));
  assert.deepEqual(memberPins(result), [1, 2, 3]);
});

test("groupGpioLayout accepts arbitrary connector names and dimensions", () => {
  const gpios: SafeGpio[] = [pin(1, "HDR9", 0, 0), pin(2, "HDR9", 3, 2), pin(3, "hdr9", 1, 1)];
  const result = groupGpioLayout(gpios);
  assert.equal(result.groups.length, 1);
  const hdr = result.groups[0]!;
  assert.equal(hdr.group, "HDR9");
  assert.equal(hdr.rowCount, 4);
  assert.equal(hdr.columnCount, 3);
  assert.equal(hdr.rows.get(3)?.[2]?.pin, 2);
  assert.equal(hdr.rows.get(1)?.[1]?.pin, 3);
  assert.equal(hdr.rows.get(2)?.[0], null);
  assert.equal(result.fallback, null);
});

test("groupGpioLayout routes missing or invalid metadata to a generic fallback", () => {
  const gpios: SafeGpio[] = [
    pin(13, "J16", 2, 0),
    barePin(99),
    barePin(98, { layoutGroup: "J16", layoutRow: 0 }),
    barePin(97, { layoutGroup: "J16", layoutRow: -1, layoutColumn: 0 }),
    barePin(96, { layoutGroup: "J16", layoutRow: 0.5, layoutColumn: 0 }),
    barePin(95, { layoutGroup: "J16", layoutRow: 0, layoutColumn: GPIO_LAYOUT_MAX_EXTENT }),
    barePin(94, { layoutGroup: "  ", layoutRow: 0, layoutColumn: 0 }),
  ];
  const result = groupGpioLayout(gpios);
  assert.equal(result.groups.length, 1);
  assert.equal(result.groups[0]?.rows.get(2)?.[0]?.pin, 13);
  assert.ok(result.fallback);
  assert.equal(result.fallback.generic, true);
  assert.equal(result.fallback.columnCount, 1);
  assert.equal(result.fallback.rowCount, 6);
  assert.deepEqual(
    memberPins({ groups: [], fallback: result.fallback }),
    [99, 98, 97, 96, 95, 94]
  );
});

test("groupGpioLayout keeps duplicate-cell colliding pins visible in fallback", () => {
  const gpios: SafeGpio[] = [pin(10, "J16", 0, 0), pin(11, "J16", 0, 0), pin(12, "J16", 0, 1)];
  const result = groupGpioLayout(gpios);
  assert.equal(result.groups[0]?.rows.get(0)?.[0]?.pin, 10);
  assert.ok(result.fallback);
  assert.deepEqual(memberPins({ groups: [], fallback: result.fallback }), [11]);
});

test("groupGpioLayout never drops a firmware-reported pin", () => {
  const gpios: SafeGpio[] = [
    pin(7, "J13", 1, 0),
    pin(15, "J16", 0, 0),
    pin(16, "J16", 0, 0),
    barePin(29),
    pin(40, "X1", 9, 4),
    barePin(41, { layoutGroup: "X1", layoutRow: -3, layoutColumn: 0 }),
  ];
  const result = groupGpioLayout(gpios);
  assert.deepEqual([...memberPins(result)].sort((a, b) => a - b), [7, 15, 16, 29, 40, 41]);
});

test("groupGpioLayout returns no groups and no fallback for empty input", () => {
  const result = groupGpioLayout([]);
  assert.deepEqual(result.groups, []);
  assert.equal(result.fallback, null);
});
