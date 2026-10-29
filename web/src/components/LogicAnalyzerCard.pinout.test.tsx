/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

import { act } from "react";
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
import type { GpioPhysicalLayout } from "@/lib/types";

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

function triggerTypeSelect(host: HTMLElement): HTMLSelectElement | undefined {
  return Array.from(host.querySelectorAll("label")).find((label) => {
    const text = label.textContent?.trim() ?? "";
    return text.startsWith("Trigger") && !text.startsWith("Trigger pin");
  })?.querySelector("select") ?? undefined;
}

/** Dispatch a raw key event so tests can inspect `defaultPrevented`. */
function keyEvent(element: Element, init: KeyboardEventInit): KeyboardEvent {
  const event = new KeyboardEvent("keydown", { bubbles: true, cancelable: true, ...init });
  act(() => {
    element.dispatchEvent(event);
  });
  return event;
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
    const waveform = view.host.querySelector('[data-testid="logic-analyzer-waveform"]')!;
    expect(waveform.tagName).toBe("SECTION");
    expect(waveform.parentElement).toBe(grid.parentElement);
    expect(Array.from(grid.parentElement!.children).indexOf(waveform))
      .toBeGreaterThan(Array.from(grid.parentElement!.children).indexOf(grid));
    expect(settings.contains(waveform)).toBe(false);
    expect(selection.contains(waveform)).toBe(false);
    expect(view.host.querySelector('[data-testid="logic-analyzer-mode-switch"]')).toBeNull();
    expect(view.host.querySelector('[data-testid="logic-analyzer-decoder"]')).not.toBeNull();
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

    // ADC1-ADC3 (GP27/28/29) are read-only layout context, not channels.
    for (const [contact, label] of [[8, "ADC1"], [7, "ADC2"], [6, "ADC3"]] as const) {
      const cell = view.host.querySelector(`[data-layout-group="J8"] [data-physical-cell="${contact}"]`)!;
      expect(cell.querySelector("[data-logic-pin]")).toBeNull();
      const context = cell.querySelector('[data-physical-inert][data-physical-role="adc"]')!;
      expect(context.getAttribute("aria-label")).toContain(label);
      expect(context.getAttribute("role")).toBe("img");
      expect(context.hasAttribute("tabindex")).toBe(false);
      expect(context.hasAttribute("aria-pressed")).toBe(false);
      click(context);
      contextMenu(context);
      keyDown(context, "c");
      keyDown(context, "Enter");
    }
    expect(view.host.textContent).toContain("1/5");
    expect(view.host.textContent).toContain("Temporary Web range: GP12-GP16");
    expect(view.host.textContent).toContain("ADC1-ADC3 capture is not implemented");
    expect(view.host.textContent).toContain("FAST8 still takes GP10/GP11 and pauses J4");
    expect(view.host.querySelector('[data-logic-pin-state="trigger"]')).toBeNull();

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
    const triggerSelect = triggerTypeSelect(view.host);
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
    expect(view.host.textContent).toContain("2/5");

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
    const triggerSelect = triggerTypeSelect(view.host);

    click(pin(view.host, 13));
    expect(stateOf(pin(view.host, 13))).toBe("trigger");
    expect(triggerSelect?.value).toBe("either");
    expect(view.host.textContent).toContain("1/5");

    // Right click on the trigger pin only removes it from the capture
    // selection; the trigger must die with it, never move to another pin.
    contextMenu(pin(view.host, 13));
    expect(stateOf(pin(view.host, 13))).toBe("unselected");
    expect(triggerSelect?.value).toBe("none");
    expect(view.host.textContent).toContain("0/5");
    expect(view.host.querySelector('[data-logic-pin-state="trigger"]')).toBeNull();

    keyDown(pin(view.host, 13), "c");
    expect(view.host.textContent).toContain("1/5");
    expect(triggerSelect?.value).toBe("none");
    view.close();
  });

  it("keeps the trigger on its own pin across sorting and non-trigger removals", () => {
    const view = renderCard();
    const triggerSelect = triggerTypeSelect(view.host);

    // GP12 is added behind GP13 but sorts before it: the trigger index must
    // follow the pin identity, not the pre-sort position.
    click(pin(view.host, 12));
    expect(stateOf(pin(view.host, 12))).toBe("trigger");
    expect(stateOf(pin(view.host, 13))).toBe("selected");
    expect(triggerSelect?.value).toBe("either");
    expect(view.host.textContent).toContain("2/5");

    // Removing a non-trigger pin only affects capture selection.
    contextMenu(pin(view.host, 13));
    expect(stateOf(pin(view.host, 12))).toBe("trigger");
    expect(stateOf(pin(view.host, 13))).toBe("unselected");
    expect(triggerSelect?.value).toBe("either");
    expect(view.host.textContent).toContain("1/5");

    // Removing the last pin clears the trigger too.
    contextMenu(pin(view.host, 12));
    expect(stateOf(pin(view.host, 12))).toBe("unselected");
    expect(triggerSelect?.value).toBe("none");
    expect(view.host.textContent).toContain("0/5");
    expect(view.host.querySelector('[data-logic-pin-state="trigger"]')).toBeNull();
    view.close();
  });

  it("activates chips through the native Enter/Space click path", () => {
    const view = renderCard({ withLayout: false });
    const gp12 = chip(view.host, 12);
    // Chips are native buttons: bare Enter/Space stay on the browser's own
    // activation path, which arrives at the component as a click.
    expect(gp12.tagName).toBe("BUTTON");
    expect(gp12.type).toBe("button");
    expect(keyEvent(gp12, { key: "Enter" }).defaultPrevented).toBe(false);
    expect(keyEvent(gp12, { key: " " }).defaultPrevented).toBe(false);

    click(gp12);
    expect(stateOf(chip(view.host, 12))).toBe("trigger");
    click(gp12);
    expect(stateOf(chip(view.host, 12))).toBe("selected");
    keyDown(chip(view.host, 12), "c");
    expect(stateOf(chip(view.host, 12))).toBe("unselected");
    view.close();
  });

  it("never triggers from Ctrl/Meta/Alt key combos on pins or chips", () => {
    const view = renderCard();
    const fallback = renderCard({ withLayout: false });
    const gp13 = pin(view.host, 13);
    const gp12 = chip(fallback.host, 12);

    for (const element of [gp13, gp12]) {
      for (const modifiers of [{ ctrlKey: true }, { metaKey: true }, { altKey: true }]) {
        for (const key of ["Enter", " ", "c", "C"]) {
          keyEvent(element, { key, ...modifiers });
        }
      }
    }
    // No combo acts on the pin or the chip.
    expect(stateOf(pin(view.host, 13))).toBe("selected");
    expect(stateOf(chip(fallback.host, 12))).toBe("unselected");
    expect(triggerTypeSelect(view.host)?.value).toBe("none");
    expect(triggerTypeSelect(fallback.host)?.value).toBe("none");
    expect(view.host.textContent).toContain("1/5");
    expect(fallback.host.textContent).toContain("1/5");

    // Browser shortcuts such as Ctrl/Cmd+C are never swallowed.
    expect(keyEvent(gp13, { key: "c", ctrlKey: true }).defaultPrevented).toBe(false);
    expect(keyEvent(gp12, { key: "C", metaKey: true }).defaultPrevented).toBe(false);
    // Modified Enter/Space do nothing at all on the SVG pin.
    expect(keyEvent(gp13, { key: "Enter", altKey: true }).defaultPrevented).toBe(false);
    expect(keyEvent(gp13, { key: " ", ctrlKey: true }).defaultPrevented).toBe(false);
    // On a native chip button they only suppress its activation.
    expect(keyEvent(gp12, { key: "Enter", ctrlKey: true }).defaultPrevented).toBe(true);
    expect(keyEvent(gp12, { key: " ", metaKey: true }).defaultPrevented).toBe(true);
    view.close();
    fallback.close();
  });

  it("keeps controllable dedicated-route pins selectable only through the extra area", () => {
    const { layout, gpios } = physicalLayoutFixture();
    const patched: GpioPhysicalLayout = {
      ...layout,
      groups: layout.groups.map((group) => ({
        ...group,
        cells: group.cells.map((cell) =>
          cell.gpioName === "GP13" ? { ...cell, role: "dedicated" } : cell
        ),
      })),
    };
    const view = mount(<LogicAnalyzerCard boardGpios={gpios} physicalLayout={patched} />);
    // The shared control predicate excludes dedicated routes, so the pin can
    // never vanish between the diagram and the extra area.
    expect(view.host.querySelector('[data-logic-pin="13"]')).toBeNull();
    const gp13Cell = view.host.querySelector('[data-layout-group="J8"] [data-physical-cell="2"]')!;
    expect(gp13Cell.querySelector("[data-physical-inert]")).not.toBeNull();
    expect(view.host.querySelector('[data-logic-chip="13"]')).not.toBeNull();

    click(chip(view.host, 13));
    expect(stateOf(chip(view.host, 13))).toBe("trigger");
    view.close();
  });

  it("offers only GP12-GP16 and removes the old GP10-GP20 supplemental area", () => {
    const view = renderCard();
    for (const number of [10, 11, 17, 18, 19, 20]) {
      expect(view.host.querySelector(`[data-logic-chip="${number}"]`)).toBeNull();
      expect(view.host.querySelector(`[data-logic-pin="${number}"]`)).toBeNull();
    }
    expect(view.host.querySelector("[data-logic-extra-pins]")).toBeNull();
    expect(view.host.querySelectorAll('[data-logic-pin]:not([aria-disabled="true"])')).toHaveLength(5);
    expect(view.host.textContent).toContain("1/5");

    // Each of the five J8 digital contacts can be selected on the diagram.
    for (const number of [12, 13, 14, 15, 16]) {
      expect(view.host.querySelector(`[data-logic-chip="${number}"]`)).toBeNull();
      const contact = pin(view.host, number);
      expect(contact.getAttribute("aria-disabled")).not.toBe("true");
      if (number !== 13) contextMenu(contact);
      expect(stateOf(pin(view.host, number))).toBe("selected");
    }
    expect(view.host.textContent).toContain("5/5");
    view.close();
  });

  it("marks disabled pins inert to selection and keeps non-capture contacts read-only", () => {
    const view = renderCard();
    for (const number of [7, 8, 9]) {
      const contact = pin(view.host, number);
      expect(stateOf(contact)).toBe("disabled");
      expect(contact.getAttribute("aria-disabled")).toBe("true");
      click(contact);
      contextMenu(contact);
      keyDown(contact, "Enter");
      expect(stateOf(pin(view.host, number))).toBe("disabled");
    }
    expect(view.host.textContent).toContain("1/5");
    expect(view.host.querySelector('[data-logic-pin-state="trigger"]')).toBeNull();

    // Direct GPIO5/6 and SCL/ADC contacts never gain capture gestures.
    for (const contactNumber of [15, 16, 9, 6]) {
      const cell = view.host.querySelector(`[data-layout-group="J8"] [data-physical-cell="${contactNumber}"]`)!;
      expect(cell.querySelector("[data-physical-inert]")).not.toBeNull();
      expect(cell.querySelector("[data-logic-pin]")).toBeNull();
    }
    for (const contact of view.host.querySelectorAll("[data-physical-inert]")) {
      click(contact);
      contextMenu(contact);
      keyDown(contact, "c");
      keyDown(contact, "Enter");
    }
    expect(view.host.textContent).toContain("1/5");
    expect(view.host.querySelector('[data-logic-pin-state="trigger"]')).toBeNull();
    view.close();
  });

  it("falls back only to the five Web capture pins when no physical layout is available", () => {
    const view = renderCard({ withLayout: false });
    expect(view.host.querySelector("[data-physical-assembly]")).toBeNull();
    expect(view.host.textContent).toContain("Connector layout unavailable");
    expect(view.host.textContent).toContain("all capture pins are listed below");
    expect(view.host.querySelectorAll("[data-logic-chip]")).toHaveLength(5);
    for (const number of [10, 11, 17, 18, 19, 20, 27, 28, 29]) {
      expect(view.host.querySelector(`[data-logic-chip="${number}"]`)).toBeNull();
    }
    for (let number = 12; number <= 16; number += 1) {
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
