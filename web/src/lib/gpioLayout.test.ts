import assert from "node:assert/strict";
import test from "node:test";
import type { SafeGpio } from "./types.ts";
import { groupGpioLayout } from "./gpioLayout.ts";

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

test("groupGpioLayout preserves firmware physical layout ordering", () => {
  const gpios: SafeGpio[] = [
    pin(8, "J13", 0, 0),
    pin(9, "J13", 0, 1),
    pin(7, "J13", 1, 1),
    pin(15, "J16", 0, 0),
    pin(29, "J16", 0, 1),
    pin(10, "J16", 5, 0),
    pin(16, "J16", 5, 1),
  ];
  const result = groupGpioLayout(gpios);
  assert.ok(result.j13);
  assert.ok(result.j16);
  assert.equal(result.j13?.rows.get(0)?.[0]?.pin, 8);
  assert.equal(result.j13?.rows.get(0)?.[1]?.pin, 9);
  assert.equal(result.j13?.rows.get(1)?.[1]?.pin, 7);
  assert.equal(result.j13?.rows.get(1)?.[0], null);
  assert.equal(result.j16?.rows.get(0)?.[0]?.pin, 29);
  assert.equal(result.j16?.rows.get(0)?.[1]?.pin, 15);
  assert.equal(result.j16?.rows.get(5)?.[0]?.pin, 16);
  assert.equal(result.j16?.rows.get(5)?.[1]?.pin, 10);
  assert.equal(result.fallback.length, 0);
});

test("groupGpioLayout separates pins without layout metadata into fallback", () => {
  const gpios: SafeGpio[] = [
    pin(13, "J16", 2, 0),
    { name: "GP99", pin: 99, note: "other", value: 0, direction: "input" },
  ];
  const result = groupGpioLayout(gpios);
  assert.equal(result.j16?.rows.get(2)?.[1]?.pin, 13);
  assert.equal(result.fallback.length, 1);
  assert.equal(result.fallback[0]?.pin, 99);
});

test("groupGpioLayout returns null groups when layoutGroup is missing", () => {
  const gpios: SafeGpio[] = [
    { name: "GP13", pin: 13, note: "n", value: 0, direction: "input" },
  ];
  const result = groupGpioLayout(gpios);
  assert.equal(result.j13, null);
  assert.equal(result.j16, null);
  assert.equal(result.fallback.length, 1);
});