/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

import { afterEach, describe, expect, it, vi } from "vitest";
import { LogicAnalyzerCard } from "./LogicAnalyzerCard";
import {
  LA_FILL_SELECTED,
  LA_FILL_TRIGGER,
  LA_FILL_UNSELECTED,
  click,
  contextMenu,
  keyDown,
  mount,
  visibleCircle,
  type View,
} from "./GpioPinoutSvg.testUtils";
import { physicalLayoutFixture } from "@/lib/gpioPhysicalLayout.testFixtures";

const sigrokMocks = vi.hoisted(() => ({
  ensureConnected: vi.fn(),
  close: vi.fn(),
  configure: vi.fn(),
  start: vi.fn(),
  stop: vi.fn(),
  getServerCapabilities: vi.fn(() => ({
    hello: null,
    caps: null,
    serverFlags: 0,
    supportsConfigV2: false,
    supportsGenericPackedBurst: false,
  })),
  readCaptureFrame: vi.fn(),
}));

vi.mock("@/hooks/useSigrokScope", () => ({
  useSigrokScope: () => sigrokMocks,
}));

function pin(host: HTMLElement, number: number): SVGGElement {
  const element = host.querySelector<SVGGElement>(`[data-logic-pin="${number}"]`);
  if (!element) throw new Error(`logic pin GP${number} not found`);
  return element;
}

function chip(host: HTMLElement, number: number): HTMLButtonElement {
  const element = host.querySelector<HTMLButtonElement>(`[data-logic-chip="${number}"]`);
  if (!element) throw new Error(`logic pin chip GP${number} not found`);
  return element;
}

function stateOf(element: Element): string | null {
  return element.getAttribute("data-logic-pin-state");
}

function renderCard(overrides: { readonly withLayout?: boolean } = {}): View {
  const { layout, gpios } = physicalLayoutFixture();
  return mount(
    <LogicAnalyzerCard
      boardGpios={gpios}
      physicalLayout={overrides.withLayout === false ? null : layout}
    />
  );
}

afterEach(() => {
  vi.clearAllMocks();
  document.body.replaceChildren();
  localStorage.clear();
});

describe("LogicAnalyzerCard physical pin selection", () => {
  it("keeps sampling settings before the pin selection block in one consistent order", () => {
    const view = renderCard();
    const settings = view.host.querySelector('[data-testid="logic-analyzer-sampling-settings"]')!;
    const selection = view.host.querySelector('[data-testid="logic-analyzer-pin-select"]')!;
    expect(settings).not.toBeNull();
    expect(selection).not.toBeNull();
    const grid = settings.parentElement!;
    expect(grid).toBe(selection.parentElement);
    const children = Array.from(grid.children);
    // Sampling settings come first everywhere: left column on desktop, first
    // block on mobile, so no breakpoint reorders the two blocks.
    expect(children.indexOf(settings)).toBeLessThan(children.indexOf(selection));
    expect(grid.className).toContain("lg:grid-cols-[minmax(0,320px)_minmax(0,1fr)]");
    expect(settings.textContent).toContain("Sample rate");
    expect(selection.textContent).toContain("Select Pins");
    view.close();
  });

  it("renders the J8/J13 physical assembly with board pin numbers and read-only context contacts", () => {
    const view = renderCard();
    expect(view.host.querySelector('[data-physical-assembly="pins-down"]')).not.toBeNull();
    const j8 = view.host.querySelector('[data-layout-group="J8"]');
    const j13 = view.host.querySelector('[data-layout-group="J13"]');
    expect(j8).not.toBeNull();
    expect(j13).not.toBeNull();
    expect(view.host.querySelectorAll("[data-physical-cell]")).toHaveLength(24);
    expect(view.host.querySelectorAll("[data-physical-number]")).toHaveLength(24);

    // The capture controls sit exactly on their physical contacts.
    const gp13Cell = view.host.querySelector('[data-layout-group="J8"] [data-physical-cell="2"]')!;
    expect(gp13Cell.querySelector("[data-logic-pin='13']")).not.toBeNull();
    expect(gp13Cell.querySelector("[data-physical-number]")?.textContent).toBe("2");
    expect(gp13Cell.textContent).toContain("GP13");

    // J13 recovery contacts are disabled capture pins, not GPIO controls.
    const gp9Cell = view.host.querySelector('[data-layout-group="J13"] [data-physical-cell="2"]')!;
    expect(gp9Cell.querySelector("[data-logic-pin='9']")).not.toBeNull();

    // GP29/ADC3 is not a capture pin: it stays read-only context.
    const gp29Cell = view.host.querySelector('[data-layout-group="J8"] [data-physical-cell="6"]')!;
    expect(gp29Cell.querySelector("[data-logic-pin]")).toBeNull();
    expect(gp29Cell.querySelector("[data-physical-inert]")).not.toBeNull();

    // No live GPIO control gestures are bound in selection mode.
    expect(view.host.querySelector('[aria-keyshortcuts="Enter Space 0 1 I"]')).toBeNull();
    view.close();
  });

  it("toggles capture selection with right click and C and keeps selection colors", () => {
    const view = renderCard();
    const gp13 = pin(view.host, 13);
    expect(stateOf(gp13)).toBe("selected");
    expect(gp13.getAttribute("aria-pressed")).toBe("true");
    expect(gp13.getAttribute("aria-keyshortcuts")).toBe("Enter Space C");
    expect(visibleCircle(gp13).getAttribute("fill")).toBe(LA_FILL_SELECTED);
    expect(view.host.textContent).toContain("Left click · Trigger");
    expect(view.host.textContent).toContain("Right click · Capture");

    contextMenu(gp13);
    expect(stateOf(pin(view.host, 13))).toBe("unselected");
    expect(pin(view.host, 13).getAttribute("aria-pressed")).toBe("false");
    expect(visibleCircle(pin(view.host, 13)).getAttribute("fill")).toBe(LA_FILL_UNSELECTED);

    contextMenu(pin(view.host, 13));
    expect(stateOf(pin(view.host, 13))).toBe("selected");

    keyDown(pin(view.host, 13), "c");
    expect(stateOf(pin(view.host, 13))).toBe("unselected");
    keyDown(pin(view.host, 13), "C");
    expect(stateOf(pin(view.host, 13))).toBe("selected");

    contextMenu(pin(view.host, 12));
    expect(stateOf(pin(view.host, 12))).toBe("selected");
    view.close();
  });

  it("sets and clears the trigger pin with left click and Enter/Space", () => {
    const view = renderCard();
    const triggerSelect = Array.from(view.host.querySelectorAll("label")).find((label) => {
      const text = label.textContent?.trim() ?? "";
      return text.startsWith("Trigger") && !text.startsWith("Trigger pin");
    })?.querySelector("select");
    expect(triggerSelect?.value).toBe("none");

    click(pin(view.host, 13));
    expect(stateOf(pin(view.host, 13))).toBe("trigger");
    expect(visibleCircle(pin(view.host, 13)).getAttribute("fill")).toBe(LA_FILL_TRIGGER);
    expect(pin(view.host, 13).getAttribute("aria-label")).toContain("Trigger pin");
    expect(triggerSelect?.value).toBe("either");

    click(pin(view.host, 13));
    expect(stateOf(pin(view.host, 13))).toBe("selected");
    expect(triggerSelect?.value).toBe("none");

    // Left click on an unselected pin brings it into the capture selection as trigger.
    click(pin(view.host, 12));
    expect(stateOf(pin(view.host, 12))).toBe("trigger");
    expect(stateOf(pin(view.host, 13))).toBe("selected");
    expect(view.host.textContent).toContain("2/11");

    keyDown(pin(view.host, 12), "Enter");
    expect(stateOf(pin(view.host, 12))).toBe("selected");
    keyDown(pin(view.host, 12), " ");
    expect(stateOf(pin(view.host, 12))).toBe("trigger");
    keyDown(pin(view.host, 12), "Enter");
    expect(stateOf(pin(view.host, 12))).toBe("selected");
    view.close();
  });

  it("clears the trigger when right click removes its pin instead of leaving a dangling trigger", () => {
    const view = renderCard();
    const triggerSelect = Array.from(view.host.querySelectorAll("label")).find((label) => {
      const text = label.textContent?.trim() ?? "";
      return text.startsWith("Trigger") && !text.startsWith("Trigger pin");
    })?.querySelector("select");

    click(pin(view.host, 13));
    expect(stateOf(pin(view.host, 13))).toBe("trigger");
    expect(triggerSelect?.value).toBe("either");
    expect(view.host.textContent).toContain("1/11");

    // Right click on the trigger pin only removes it from the capture
    // selection; the trigger must die with it, never move to another pin.
    contextMenu(pin(view.host, 13));
    expect(stateOf(pin(view.host, 13))).toBe("unselected");
    expect(triggerSelect?.value).toBe("none");
    expect(view.host.textContent).toContain("0/11");
    expect(view.host.querySelector('[data-logic-pin-state="trigger"]')).toBeNull();

    keyDown(pin(view.host, 13), "c");
    expect(view.host.textContent).toContain("1/11");
    expect(triggerSelect?.value).toBe("none");
    view.close();
  });

  it("keeps the trigger on its own pin across sorting and non-trigger removals", () => {
    const view = renderCard();
    const triggerSelect = Array.from(view.host.querySelectorAll("label")).find((label) => {
      const text = label.textContent?.trim() ?? "";
      return text.startsWith("Trigger") && !text.startsWith("Trigger pin");
    })?.querySelector("select");

    // GP12 is added behind GP13 but sorts before it: the trigger index must
    // follow the pin identity, not the pre-sort position.
    click(pin(view.host, 12));
    expect(stateOf(pin(view.host, 12))).toBe("trigger");
    expect(stateOf(pin(view.host, 13))).toBe("selected");
    expect(triggerSelect?.value).toBe("either");
    expect(view.host.textContent).toContain("2/11");

    // Removing a non-trigger pin only affects capture selection.
    contextMenu(pin(view.host, 13));
    expect(stateOf(pin(view.host, 12))).toBe("trigger");
    expect(stateOf(pin(view.host, 13))).toBe("unselected");
    expect(triggerSelect?.value).toBe("either");
    expect(view.host.textContent).toContain("1/11");

    // Removing the last pin clears the trigger too.
    contextMenu(pin(view.host, 12));
    expect(stateOf(pin(view.host, 12))).toBe("unselected");
    expect(triggerSelect?.value).toBe("none");
    expect(view.host.textContent).toContain("0/11");
    expect(view.host.querySelector('[data-logic-pin-state="trigger"]')).toBeNull();
    view.close();
  });

  it("keeps capture pins without a physical contact selectable below the diagram", () => {
    const view = renderCard();
    for (const number of [10, 11, 17, 18, 19, 20]) {
      expect(view.host.querySelector(`[data-logic-chip="${number}"]`)).not.toBeNull();
    }
    // Pins on the physical diagram are not duplicated in the extra area.
    for (const number of [12, 13, 14, 15, 16]) {
      expect(view.host.querySelector(`[data-logic-chip="${number}"]`)).toBeNull();
      expect(view.host.querySelector(`[data-logic-pin="${number}"]`)).not.toBeNull();
    }
    expect(view.host.textContent).toContain("1/11");

    // Right click (and C) add an off-board pin to the capture selection.
    const gp10 = chip(view.host, 10);
    expect(stateOf(gp10)).toBe("unselected");
    contextMenu(gp10);
    expect(stateOf(chip(view.host, 10))).toBe("selected");
    expect(chip(view.host, 10).getAttribute("aria-pressed")).toBe("true");
    expect(view.host.textContent).toContain("2/11");

    // Left click sets the trigger on the chip as well.
    click(chip(view.host, 10));
    expect(stateOf(chip(view.host, 10))).toBe("trigger");
    click(chip(view.host, 10));
    expect(stateOf(chip(view.host, 10))).toBe("selected");

    keyDown(chip(view.host, 10), "c");
    expect(stateOf(chip(view.host, 10))).toBe("unselected");
    keyDown(chip(view.host, 17), "C");
    expect(stateOf(chip(view.host, 17))).toBe("selected");
    expect(view.host.textContent).toContain("2/11");
    view.close();
  });

  it("marks disabled pins inert to selection and keeps non-capture contacts read-only", () => {
    const view = renderCard();
    const gp9 = pin(view.host, 9);
    expect(stateOf(gp9)).toBe("disabled");
    expect(gp9.getAttribute("aria-disabled")).toBe("true");

    click(gp9);
    contextMenu(gp9);
    keyDown(gp9, "Enter");
    expect(stateOf(pin(view.host, 9))).toBe("disabled");
    expect(view.host.textContent).toContain("1/11");
    expect(view.host.querySelector('[data-logic-pin-state="trigger"]')).toBeNull();

    for (const contact of view.host.querySelectorAll("[data-physical-inert]")) {
      click(contact);
      contextMenu(contact);
    }
    expect(view.host.textContent).toContain("1/11");
    expect(view.host.querySelector('[data-logic-pin-state="trigger"]')).toBeNull();
    view.close();
  });

  it("falls back to the full capture pin list when no physical layout is available", () => {
    const view = renderCard({ withLayout: false });
    expect(view.host.querySelector("[data-physical-assembly]")).toBeNull();
    expect(view.host.textContent).toContain("connector layout");
    for (let number = 10; number <= 20; number += 1) {
      expect(view.host.querySelector(`[data-logic-chip="${number}"]`)).not.toBeNull();
    }
    click(chip(view.host, 12));
    expect(stateOf(chip(view.host, 12))).toBe("trigger");
    click(chip(view.host, 12));
    expect(stateOf(chip(view.host, 12))).toBe("selected");
    keyDown(chip(view.host, 12), "c");
    expect(stateOf(chip(view.host, 12))).toBe("unselected");
    view.close();
  });
});
