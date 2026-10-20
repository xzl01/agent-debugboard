// @vitest-environment jsdom
import { afterEach, describe, expect, it, vi } from "vitest";
import { mapBoardStatus } from "@/lib/boardSnapshot";
import { GPIO_FALLBACK_GROUP_ID } from "@/lib/gpioLayout";
import { x12GpioStatusFixture } from "@/lib/gpioLayout.testFixtures";
import {
  advanceTimers, click, contextMenu, gpio, gpioPin, laPin, pinCircle,
  pointerDown, pointerUp, renderSvg, rerenderSvg,
} from "./GpioPinoutSvg.testUtils";

afterEach(() => {
  vi.useRealTimers();
  document.body.replaceChildren();
  localStorage.clear();
});

describe("GpioPinoutSvg firmware-driven layout groups", () => {
  it("renders pins without valid layout metadata in a visible fallback group", () => {
    const view = renderSvg({
      variant: "gpio",
      gpios: [
        gpio(10),
        gpio(99, { layoutGroup: undefined, layoutRow: undefined, layoutColumn: undefined }),
        gpio(98, { layoutRow: -1 }),
        gpio(97, { layoutGroup: "J16", layoutRow: 0, layoutColumn: 0 }),
      ],
    });
    expect(gpioPin(view.host, 99)).toBeTruthy();
    expect(gpioPin(view.host, 98)).toBeTruthy();
    expect(view.host.textContent).toContain("Other");
    view.close();
  });

  it("keeps duplicate-cell colliding pins visible and actionable", () => {
    const view = renderSvg({
      variant: "gpio",
      gpios: [
        gpio(10, { layoutRow: 0, layoutColumn: 0 }),
        gpio(11, { layoutRow: 0, layoutColumn: 0 }),
      ],
    });
    expect(gpioPin(view.host, 10)).toBeTruthy();
    expect(gpioPin(view.host, 11)).toBeTruthy();
    view.close();
  });

  it("renders arbitrary firmware connector groups without host-side geometry", () => {
    const view = renderSvg({
      variant: "logic-analyzer",
      selectedPins: [1, 2, 3],
      gpios: [
        gpio(1, { layoutGroup: "HDR9", layoutRow: 0, layoutColumn: 0 }),
        gpio(2, { layoutGroup: "HDR9", layoutRow: 3, layoutColumn: 2 }),
        gpio(3, { layoutGroup: "X1", layoutRow: 0, layoutColumn: 0 }),
      ],
    });
    expect(laPin(view.host, "GP1")).toBeTruthy();
    expect(laPin(view.host, "GP2")).toBeTruthy();
    expect(laPin(view.host, "GP3")).toBeTruthy();
    expect(view.host.textContent).toContain("HDR9");
    expect(view.host.textContent).toContain("X1");
    view.close();
  });

  it("sorts by explicit metadata order rather than snapshot or connector name", () => {
    const view = renderSvg({
      variant: "gpio",
      gpios: [
        gpio(8, { layoutGroup: "J13", layoutRow: 0, layoutColumn: 1 }),
        gpio(15, { layoutGroup: "J8", layoutRow: 6, layoutColumn: 0 }),
      ],
      layoutGroups: [
        { name: "J13", edge: "right", order: 1 },
        { name: "J8", edge: "right", order: 0 },
      ],
    });
    expect(Array.from(view.host.querySelectorAll("[data-layout-group]")).map((group) =>
      group.getAttribute("data-layout-group")
    )).toEqual(["J8", "J13"]);
    expect(view.host.querySelectorAll('[data-layout-edge="right"]')).toHaveLength(2);
    view.close();
  });

  it.each(["gpio", "logic-analyzer"] as const)("renders the real x1.2 metadata in three intact, unmirrored %s groups", (variant) => {
    const snapshot = mapBoardStatus(x12GpioStatusFixture(), []);
    const view = renderSvg({ variant, gpios: snapshot.gpios, layoutGroups: snapshot.gpioLayoutGroups });
    const groups = Array.from(view.host.querySelectorAll("[data-layout-group]"));
    expect(groups.map((group) => group.getAttribute("data-layout-group"))).toEqual(["J8", "J13", GPIO_FALLBACK_GROUP_ID]);
    expect(groups.map((group) => group.getAttribute("data-layout-edge"))).toEqual(["right", "right", "generic"]);
    expect(groups.map((group) => group.querySelectorAll('circle[r="17"]').length)).toEqual([6, 3, 2]);

    for (const [number, label, cx, cy] of [
      [12, "GP12", 21, 341], [13, "GP13", 21, 307], [14, "GP14", 21, 273],
      [15, "GP15", 21, 239], [16, "GP16", 21, 205], [29, "ADC3", 21, 171],
      [9, "USER", 21, 35], [8, "RSET", 57, 35], [7, "MASKROM", 21, 69],
      [17, "GP17", 21, 35], [18, "GP18", 21, 69],
    ] as const) {
      const pin = variant === "gpio" ? gpioPin(view.host, number) : laPin(view.host, label);
      const target = pinCircle(pin, "17");
      expect([Number(target.getAttribute("cx")), Number(target.getAttribute("cy"))]).toEqual([cx, cy]);
      const svg = pin.closest("svg")!;
      const [, , width, height] = svg.getAttribute("viewBox")!.split(" ").map(Number);
      expect(cx - 17).toBeGreaterThanOrEqual(0);
      expect(cy - 17).toBeGreaterThanOrEqual(0);
      expect(cx + 17).toBeLessThanOrEqual(width!);
      expect(cy + 17).toBeLessThanOrEqual(height!);
      const scale = Math.min(Number.parseFloat(svg.style.width) / width!, Number.parseFloat(svg.style.height) / height!);
      expect(34 * scale).toBeGreaterThanOrEqual(44);
    }

    expect(groups[0]!.closest("svg")!.getAttribute("viewBox")).toBe("0 0 52 362");
    for (const group of groups.slice(0, 2)) {
      expect(group.querySelector('[data-pinout-part="board-interior"]')?.getAttribute("d")).not.toMatch(/[zZ]/);
      expect(group.querySelector('[data-pinout-part="generic-frame"]')).toBeNull();
      expect(group.querySelector('[data-pinout-part="decoration"]')?.getAttribute("pointer-events")).toBe("none");
    }
    expect(groups[2]!.querySelector('[data-pinout-part="contact"]')).toBeNull();
    expect(view.host.querySelectorAll('circle[r="17"]')).toHaveLength(snapshot.gpios.length);
    expect(view.host.querySelector("[transform]")).toBeNull();
    expect(view.host.textContent).not.toMatch(/GND|3\.3\s*V/);
    view.close();
  });

  it.each(["gpio", "logic-analyzer"] as const)("opens arbitrary right-edge groups without moving %s pins", (variant) => {
    const gpios = [
      gpio(1, { layoutGroup: "PORT", layoutRow: 0, layoutColumn: 0 }),
      gpio(2, { layoutGroup: "PORT", layoutRow: 1, layoutColumn: 1 }),
    ];
    const pin = (host: HTMLElement, number: number) => variant === "gpio"
      ? gpioPin(host, number) : laPin(host, `GP${number}`);
    const centers = (host: HTMLElement) => [1, 2].map((number) => {
      const target = pinCircle(pin(host, number), "17");
      return [target.getAttribute("cx"), target.getAttribute("cy"), target.getAttribute("r")];
    });
    const view = renderSvg({ variant, gpios });
    const before = centers(view.host);
    expect(before).toEqual([["21", "35", "17"], ["57", "69", "17"]]);
    rerenderSvg(view, { variant, gpios, layoutGroups: [{ name: "PORT", edge: "right", order: 0 }] });

    expect(centers(view.host)).toEqual(before);
    expect(view.host.querySelector('[data-pinout-part="generic-frame"]')).toBeNull();
    const outline = view.host.querySelector('[data-pinout-part="board-interior"]')!;
    expect(outline.getAttribute("d")).toBe("M 77.5 0.5 H 10.5 Q 0.5 0.5 0.5 10.5 V 79.5 Q 0.5 89.5 10.5 89.5 H 77.5");
    expect(outline.getAttribute("d")).not.toMatch(/[zZ]/);
    const contacts = view.host.querySelectorAll('[data-pinout-part="contact"]');
    expect(contacts).toHaveLength(2);
    for (const contact of contacts) {
      expect(Number(contact.getAttribute("x1"))).toBeLessThan(78);
      expect(Number(contact.getAttribute("x2"))).toBeGreaterThan(78);
      expect(contact.getAttribute("y1")).toBe(contact.getAttribute("y2"));
    }
    expect(view.host.querySelector('[data-pinout-part="decoration"] text')?.getAttribute("text-anchor")).toBe("start");
    expect(view.host.querySelector("[transform]")).toBeNull();
    expect(view.host.querySelectorAll('circle[r="17"]')).toHaveLength(2);
    expect(view.host.textContent).not.toMatch(/3\.3\s*V|GND/);
    view.close();
  });

  it.each([undefined, "unknown", "left"])("keeps groups generic for an unreported or unsupported edge (%s)", (edge) => {
    const view = renderSvg({
      variant: "gpio",
      gpios: [gpio(8, { layoutGroup: "J13", layoutRow: 0, layoutColumn: 1 }), gpio(15, { layoutGroup: "J8", layoutRow: 6, layoutColumn: 0 })],
      layoutGroups: edge === undefined ? undefined : [
        { name: "J13", edge, order: 0 }, { name: "J8", edge, order: 1 },
      ],
    });
    expect(view.host.querySelectorAll('[data-pinout-part="generic-frame"]')).toHaveLength(2);
    expect(view.host.querySelector('[data-pinout-part="contact"]')).toBeNull();
    expect(view.host.textContent).toContain("Generic layout");
    expect(view.host.textContent).not.toContain("Right-edge connectors");
    view.close();
  });

  it("keeps decoration and empty cells inert while side-exit pin gestures still work", () => {
    vi.useFakeTimers();
    const onGpioAction = vi.fn();
    const onSetTriggerPin = vi.fn();
    const onTogglePin = vi.fn();
    const view = renderSvg({
      variant: "gpio",
      gpios: [gpio(1, { layoutGroup: "PORT", layoutRow: 0, layoutColumn: 1 })],
      layoutGroups: [{ name: "PORT", edge: "right", order: 0 }],
      onGpioAction, onSetTriggerPin, onTogglePin,
    });
    const decoration = view.host.querySelector('[data-pinout-part="decoration"]')!;
    expect(decoration.getAttribute("pointer-events")).toBe("none");
    expect(decoration.getAttribute("aria-hidden")).toBe("true");
    expect(decoration.querySelector('[role="button"], [tabindex]')).toBeNull();
    expect(view.host.querySelectorAll('[role="button"]')).toHaveLength(1);
    for (const element of [decoration, ...decoration.children, view.host.querySelector("svg")!]) {
      pointerDown(element, { clientX: 21, clientY: 35 });
      pointerUp(element, { clientX: 21, clientY: 35 });
      click(element);
      contextMenu(element);
    }
    advanceTimers(1000);
    expect(onGpioAction).not.toHaveBeenCalled();
    expect(onSetTriggerPin).not.toHaveBeenCalled();
    expect(onTogglePin).not.toHaveBeenCalled();

    pointerDown(gpioPin(view.host, 1));
    pointerUp(gpioPin(view.host, 1));
    advanceTimers(220);
    expect(onGpioAction).toHaveBeenCalledExactlyOnceWith(1, { kind: "outputLow" });
    view.close();
  });
});
