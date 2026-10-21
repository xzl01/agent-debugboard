import { afterEach, describe, expect, it, vi } from "vitest";
import { directGpioFixtureCases, physicalLayoutFixture } from "@/lib/gpioPhysicalLayout.testFixtures";
import { physicalPinPurpose, placePhysicalAssembly, GpioPhysicalPinoutSvg } from "./GpioPhysicalPinoutSvg";
import type { GpioPhysicalCell } from "@/lib/types";
import { GpioCard } from "./GpioCard";
import { mount } from "./GpioCard.testUtils";
import { click, keyDown } from "./GpioPinoutSvg.testUtils";

afterEach(() => { document.body.replaceChildren(); localStorage.clear(); });

describe("physical assembly: J13 left, J8 right, solder pins down", () => {
  it("places J13 left and horizontal 2×10 J8 right with the exact non-mirrored pin order", () => {
    const { layout } = physicalLayoutFixture();
    const assembly = placePhysicalAssembly(layout);
    expect(assembly.columns).toBe(13);
    expect(assembly.rows).toBe(2);
    const j8 = assembly.groups.find(({ group }) => group.name === "J8")!;
    const j13 = assembly.groups.find(({ group }) => group.name === "J13")!;
    const at = (group: typeof j8, row: number) => group.cells.filter((cell) => cell.row === row).map(({ cell }) => cell.pinNumber);
    expect(at(j8, 0)).toEqual([1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);
    expect(at(j8, 1)).toEqual([11, 12, 13, 14, 15, 16, 17, 18, 19, 20]);
    expect(at(j13, 0)).toEqual([1, 2]);
    expect(at(j13, 1)).toEqual([4, 3]);
    expect(j13.row).toBe(j8.row);
    expect(Math.max(...j13.cells.map((cell) => cell.column))).toBeLessThan(Math.min(...j8.cells.map((cell) => cell.column)));
    // Source right/down becomes display down/left: the signed orientation is
    // preserved. A horizontal reflection reverses this sign and fails.
    const origin = j8.cells.find(({ cell }) => cell.row === 0 && cell.column === 0)!;
    const right = j8.cells.find(({ cell }) => cell.row === 0 && cell.column === 1)!;
    const down = j8.cells.find(({ cell }) => cell.row === 1 && cell.column === 0)!;
    expect([right.column - origin.column, right.row - origin.row]).toEqual([0, 1]);
    expect([down.column - origin.column, down.row - origin.row]).toEqual([-1, 0]);
    expect((right.column - origin.column) * (down.row - origin.row) - (right.row - origin.row) * (down.column - origin.column)).toBe(1);
  });

  it("preserves adjacency of every physical pair and connector order from arbitrary metadata", () => {
    const { layout } = physicalLayoutFixture();
    const assembly = placePhysicalAssembly(layout);
    for (const group of assembly.groups) for (const a of group.cells) for (const b of group.cells) {
      const before = Math.abs(a.cell.row - b.cell.row) + Math.abs(a.cell.column - b.cell.column);
      const after = Math.abs(a.row - b.row) + Math.abs(a.column - b.column);
      expect(after).toBe(before);
    }
  });

  it("renders exactly 24 positions, 11 physical controls and 13 inert sockets without rotated glyphs", () => {
    const { layout, gpios } = physicalLayoutFixture();
    const view = mount(<GpioPhysicalPinoutSvg layout={layout} gpios={gpios} />);
    expect(view.host.querySelectorAll("[data-physical-cell]")).toHaveLength(24);
    expect(view.host.querySelectorAll('[role="button"]')).toHaveLength(11);
    expect(view.host.querySelectorAll("[data-physical-inert]")).toHaveLength(13);
    expect(view.host.querySelectorAll('circle[fill="transparent"]')).toHaveLength(11);
    expect(view.host.querySelector('[transform]')).toBeNull();
    expect(view.host.querySelector('svg[role="group"]')?.getAttribute("data-physical-assembly")).toBe("pins-down");
    expect(view.host.textContent).toContain("Solder pins facing down");
    for (const contact of view.host.querySelectorAll("[data-physical-contact]")) {
      expect(contact.getAttribute("x1")).toBe(contact.getAttribute("x2"));
      expect(Number(contact.getAttribute("y2"))).toBeGreaterThan(Number(contact.getAttribute("y1")));
    }
    expect(view.host.innerHTML).not.toMatch(/GP17|GP18/);
    expect(view.host.querySelector('[data-layout-group="J8"] [data-physical-cell="20"]')?.getAttribute("data-display-row")).toBe("1");
    expect(view.host.querySelector('[data-layout-group="J8"] [data-physical-cell="10"]')?.getAttribute("data-display-column")).toBe("12");
    // The scroll viewport contains the full-size diagram without shrinking targets.
    const svg = view.host.querySelector("svg")!;
    expect(svg.parentElement?.classList.contains("overflow-x-auto")).toBe(true);
    const width = Number(svg.getAttribute("viewBox")!.split(" ")[2]);
    expect(width).toBe(618);
    expect(Number.parseFloat(svg.style.width)).toBe(width);
    view.close();
  });

  it("shows purposes only inside physical circles, without duplicate static tables or connector labels", () => {
    const { layout, gpios } = physicalLayoutFixture();
    const view = mount(<GpioPhysicalPinoutSvg layout={layout} gpios={gpios} />);
    expect(view.host.querySelector("dl, table, h3")).toBeNull();
    expect(view.host.querySelectorAll("[data-layout-group] > text")).toHaveLength(2);
    expect(view.host.querySelectorAll("[data-physical-number]")).toHaveLength(24);
    const reference = view.host.querySelector("[data-physical-reference]")!;
    expect(reference.children).toHaveLength(2);
    expect(reference.lastElementChild?.hasAttribute("data-physical-viewport")).toBe(true);
    for (const cell of view.host.querySelectorAll("[data-physical-inert]")) {
      expect(cell.getAttribute("aria-label")).not.toMatch(/, (high|low)/);
    }
    view.close();
  });

  it("preserves advertised USB D+/D− and hub contacts as inert physical signals, not switches", () => {
    const { layout, gpios } = physicalLayoutFixture();
    const action = vi.fn();
    const view = mount(<GpioPhysicalPinoutSvg layout={layout} gpios={gpios} onGpioAction={action} />);
    const contacts = view.host.querySelectorAll('[data-physical-role="usb"]');
    expect(contacts).toHaveLength(4);
    for (const contact of contacts) {
      expect(contact.hasAttribute("data-physical-inert")).toBe(true);
      expect(contact.querySelector('[role="button"], [tabindex], circle[fill="transparent"]')).toBeNull();
      click(contact); keyDown(contact, "Enter"); keyDown(contact, "1");
    }
    expect(action).not.toHaveBeenCalled();
    view.close();
  });

  it("keeps all eleven small circular controls interactive, with independent 44px targets", () => {
    const { layout, gpios } = physicalLayoutFixture();
    const onSet = vi.fn();
    const view = mount(<GpioCard gpios={gpios} physicalLayout={layout} onSet={onSet} />);
    const controls = view.host.querySelectorAll('[role="button"]');
    expect(controls).toHaveLength(11);
    expect([...controls].filter((pin) => pin.getAttribute("aria-keyshortcuts") === "Enter Space 0 1 I")).toHaveLength(10);
    expect([...controls].filter((pin) => pin.getAttribute("aria-keyshortcuts") === "I")).toHaveLength(1);
    for (const pin of controls) {
      expect(pin.querySelector('circle[r="11.5"]')).not.toBeNull();
      expect(pin.querySelector('circle[r="14"]')).not.toBeNull();
      expect(pin.querySelector('circle[r="16"]')).not.toBeNull();
      expect(pin.querySelector('circle[fill="transparent"]')?.getAttribute("r")).toBe("22");
      expect(pin.getAttribute("tabindex")).toBe("0");
      expect(pin.getAttribute("aria-describedby")).toBeTruthy();
    }
    for (const pin of view.host.querySelectorAll("[data-physical-inert]")) {
      click(pin); keyDown(pin, "Enter"); keyDown(pin, "1");
    }
    expect(onSet).not.toHaveBeenCalled();
    expect(view.host.querySelector("svg[data-gpio-control-group]")).toBeNull();
    view.close();
  });

  it.each(directGpioFixtureCases)("renders $gpioName as a normal GPIO circle, never a dedicated route contact", (entry) => {
    const { layout, gpios } = physicalLayoutFixture();
    const view = mount(<GpioCard gpios={gpios} physicalLayout={layout} onSet={vi.fn()} />);
    const cell = view.host.querySelector(`[data-layout-group="J8"] [data-physical-cell="${entry.pinNumber}"]`)!;
    const pin = cell.querySelector('[role="button"]')!;
    expect(pin.querySelector("text")?.textContent).toBe(entry.gpioName);
    expect(pin.getAttribute("aria-label")).toContain(entry.signal);
    expect(pin.getAttribute("aria-label")).not.toContain("Owned by route controls");
    expect(pin.getAttribute("aria-keyshortcuts")).toBe("Enter Space 0 1 I");
    expect(pin.querySelector('circle[fill="transparent"]')?.getAttribute("r")).toBe("22");
    expect(cell.querySelector('[data-physical-inert], [data-physical-role="dedicated"]')).toBeNull();
    expect(view.host.querySelector('[data-physical-role="dedicated"]')).toBeNull();
    expect(view.host.textContent).not.toMatch(/USB MUX|TF SW/u);
    view.close();
  });

  it("handles a future connector and unknown inert role without inventing controls", () => {
    const view = mount(<GpioPhysicalPinoutSvg gpios={[]} layout={{ view: "component-top", groups: [{ name: "NEXT", order: 12, edge: "future", rows: 1, columns: 3, cells: [0, 1, 2].map((column) => ({ pinNumber: 100 + column, row: 0, column, label: `REF${column}`, signal: "FUTURE", role: "future-role", controllable: false, outputCapable: false })) }] }} />);
    expect(view.host.textContent).toContain("NEXT");
    expect(view.host.textContent).toContain("future-role");
    expect(view.host.querySelectorAll("[data-physical-inert]")).toHaveLength(3);
    expect(view.host.querySelector('[role="button"]')).toBeNull();
    view.close();
  });

  it("fails closed in the control card when the dynamic join is invalid", () => {
    const { layout, gpios } = physicalLayoutFixture();
    const view = mount(<GpioCard physicalLayout={layout} gpios={gpios.slice(1)} onSet={vi.fn()} />);
    expect(view.host.querySelector('[role="alert"]')).not.toBeNull();
    expect(view.host.querySelector("[data-physical-cell]")).toBeNull();
    view.close();
  });

  it.each([
    ["adc", "GP26 / ADC0", "ADC0"], ["adc", "GP27 / ADC1", "ADC1"],
    ["adc", "GP28 / ADC2", "ADC2"], ["adc", "GP29 / ADC3", "ADC3"],
    ["i2c", "GP20 / SCL", "SCL"], ["i2c", "GP21 / SDA", "SDA"],
    ["usb", "USB DP", "USB D+"], ["usb", "USB DM (hub)", "HUB D−"],
    ["dedicated", "USB MUX", "USB MUX"], ["dedicated", "TF switch", "TF SW"],
    ["dedicated", "NET_NAME / USB mux", "USB MUX"], ["ground", "GND", "GND"],
    ["power", "5V USB HUB", "5V HUB"],
  ])("uses the %s purpose from label %s", (role, label, expected) => {
    const cell: GpioPhysicalCell = { pinNumber: 1, row: 0, column: 0, label, signal: "fixture", role, controllable: false, outputCapable: false };
    expect(physicalPinPurpose(cell)).toBe(expected);
  });

  it("supports generic future dedicated contacts without treating current GPIO nets as routes", () => {
    const cells = [
      { label: "USB MUX", signal: "NET_USB_ROUTE", role: "dedicated" },
      { label: "TF SW", signal: "NET_TF_ROUTE", role: "dedicated" },
      { label: "ADC1", signal: "TEST_ADC1", role: "adc" },
      { label: "SDA", signal: "TEST_SDA", role: "i2c" },
    ].map((cell, row) => ({ ...cell, row, column: 0, pinNumber: row + 1, controllable: false, outputCapable: false }));
    const onGpioAction = vi.fn();
    const view = mount(<GpioPhysicalPinoutSvg gpios={[]} onGpioAction={onGpioAction} layout={{ view: "component-top", groups: [{ name: "TEST", rows: 4, columns: 1, order: 0, edge: "right", cells }] }} />);
    const dedicated = view.host.querySelectorAll('[data-physical-role="dedicated"]');
    expect(dedicated).toHaveLength(2);
    expect([...view.host.querySelectorAll('[data-physical-cell="1"] [data-physical-inert] text tspan')].map((line) => line.textContent)).toEqual(["USB", "MUX"]);
    expect([...view.host.querySelectorAll('[data-physical-cell="2"] [data-physical-inert] text tspan')].map((line) => line.textContent)).toEqual(["TF", "SW"]);
    for (const pin of dedicated) {
      expect(pin.getAttribute("aria-label")).toContain("Owned by route controls");
      expect(pin.getAttribute("aria-label")).toContain("Net name:");
      expect(pin.querySelector("text")?.textContent).not.toMatch(/GP[56]/);
      expect(pin.querySelector('circle[r="14"]')?.getAttribute("stroke")).toContain("--c-brand");
    }
    expect(view.host.querySelector('[data-physical-cell="1"] [data-physical-inert]')?.getAttribute("aria-label")).toContain("Net name: NET_USB_ROUTE");
    expect(view.host.querySelector('[data-physical-cell="2"] [data-physical-inert]')?.getAttribute("aria-label")).toContain("Net name: NET_TF_ROUTE");
    expect(view.host.querySelector('[role="button"], [tabindex], circle[fill="transparent"]')).toBeNull();
    expect(view.host.querySelectorAll("[data-physical-number]")).toHaveLength(4);
    for (const pin of view.host.querySelectorAll("[data-physical-inert]")) { click(pin); keyDown(pin, "Enter"); keyDown(pin, "1"); }
    expect(onGpioAction).not.toHaveBeenCalled();
    view.close();
  });
});
