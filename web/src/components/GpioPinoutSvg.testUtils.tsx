import { act, type ReactElement } from "react";
import { createRoot, type Root } from "react-dom/client";
import { vi } from "vitest";
import { LanguageProvider } from "@/lib/i18n";
import type { SafeGpio } from "@/lib/types";
import { GpioPinoutSvg, type GpioPinoutSvgProps } from "./GpioPinoutSvg";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

export const GPIO_FILL_HIGH = "rgb(var(--c-danger))";
export const GPIO_FILL_LOW = "rgb(var(--c-gpio-low))";
export const GPIO_DIRECTION_STROKE = "rgb(var(--c-ink-dim))";
export const GPIO_INPUT_DASH = "3 2";
export const GPIO_PENDING_STROKE = "rgb(var(--c-warn))";
export const GPIO_RING_BRAND = "rgb(var(--c-brand))";
export const GPIO_LABEL_ON_LEVEL = "rgb(var(--c-gpio-on-level))";
export const LA_FILL_UNSELECTED = "rgb(var(--c-panel))";
export const LA_FILL_SELECTED = "rgb(var(--c-brand) / 0.14)";
export const LA_FILL_TRIGGER = "rgb(var(--c-warn) / 0.14)";

export const LEVEL_DISC_R = "11.5";
export const DIRECTION_RING_R = "14";
export const FOCUS_RING_R = "16";
export const HIT_TARGET_R = "17";

export function gpio(pin: number, overrides: Partial<SafeGpio> = {}): SafeGpio {
  return {
    name: `sig${pin}`,
    pin,
    note: `note ${pin}`,
    value: 0,
    direction: "input",
    layoutGroup: "J16",
    layoutLabel: `GP${pin}`,
    layoutRow: pin - 10,
    layoutColumn: 0,
    ...overrides,
  };
}

export interface View {
  readonly host: HTMLDivElement;
  readonly root: Root;
  readonly close: () => void;
}

export function mount(element: ReactElement): View {
  localStorage.setItem("lang", "en");
  const host = document.createElement("div");
  document.body.append(host);
  const root = createRoot(host);
  act(() => {
    root.render(<LanguageProvider>{element}</LanguageProvider>);
  });
  return {
    host,
    root,
    close: () => {
      act(() => root.unmount());
      host.remove();
    },
  };
}

export function renderSvg(props: GpioPinoutSvgProps): View {
  return mount(<GpioPinoutSvg {...props} />);
}

export function rerenderSvg(view: View, props: GpioPinoutSvgProps): void {
  act(() => {
    view.root.render(
      <LanguageProvider>
        <GpioPinoutSvg {...props} />
      </LanguageProvider>
    );
  });
}

export function gpioPin(host: HTMLElement, pin: number): SVGGElement {
  const element = host.querySelector<SVGGElement>(`[role="button"][aria-label*="GP${pin},"]`);
  if (!element) throw new Error(`GPIO pin GP${pin} not found`);
  return element;
}

export function laPin(host: HTMLElement, label: string): SVGGElement {
  for (const element of host.querySelectorAll<SVGGElement>("svg g")) {
    if (element.textContent === label) return element;
  }
  throw new Error(`logic-analyzer pin ${label} not found`);
}

export function pinCircle(pin: SVGGElement, r: string): SVGCircleElement {
  const circle = pin.querySelector<SVGCircleElement>(`circle[r="${r}"]`);
  if (!circle) throw new Error(`pin circle r=${r} not found`);
  return circle;
}

export function optionalCircle(pin: SVGGElement, r: string): SVGCircleElement | null {
  return pin.querySelector<SVGCircleElement>(`circle[r="${r}"]`);
}

export function visibleCircle(pin: SVGGElement): SVGCircleElement {
  const circle = pin.querySelector("circle");
  if (!circle) throw new Error("pin circle not found");
  return circle;
}

export function holdArc(pin: SVGGElement): SVGCircleElement | null {
  return pin.querySelector<SVGCircleElement>("circle.gpio-hold-arc");
}

export function focusRing(pin: SVGGElement): SVGCircleElement | null {
  for (const circle of pin.querySelectorAll<SVGCircleElement>(`circle[r="${FOCUS_RING_R}"]`)) {
    if (circle.getAttribute("stroke") === GPIO_RING_BRAND) return circle;
  }
  return null;
}

export function pendingRing(pin: SVGGElement): SVGCircleElement | null {
  for (const circle of pin.querySelectorAll<SVGCircleElement>(`circle[r="${FOCUS_RING_R}"]`)) {
    if (circle.getAttribute("stroke") === GPIO_PENDING_STROKE) return circle;
  }
  return null;
}

export function click(element: Element): void {
  act(() => {
    element.dispatchEvent(new MouseEvent("click", { bubbles: true, cancelable: true }));
  });
}

export function contextMenu(element: Element): void {
  act(() => {
    element.dispatchEvent(new MouseEvent("contextmenu", { bubbles: true, cancelable: true }));
  });
}

export function keyDown(element: Element, key: string, shiftKey = false, repeat = false): void {
  act(() => {
    element.dispatchEvent(
      new KeyboardEvent("keydown", { key, shiftKey, repeat, bubbles: true, cancelable: true })
    );
  });
}

export interface PointerGestureInit {
  readonly pointerId?: number;
  readonly clientX?: number;
  readonly clientY?: number;
  readonly button?: number;
  readonly isPrimary?: boolean;
}

function gestureEvent(type: string, init: PointerGestureInit): PointerEvent {
  return new PointerEvent(type, {
    bubbles: true,
    cancelable: true,
    pointerId: init.pointerId ?? 1,
    clientX: init.clientX ?? 0,
    clientY: init.clientY ?? 0,
    button: init.button ?? 0,
    isPrimary: init.isPrimary ?? true,
  });
}

function stubPointerCapture(element: Element): void {
  if (!("setPointerCapture" in element)) {
    Object.defineProperty(element, "setPointerCapture", {
      value: () => undefined,
      configurable: true,
    });
  }
}

export function pointerDown(element: Element, init: PointerGestureInit = {}): void {
  stubPointerCapture(element);
  act(() => {
    element.dispatchEvent(gestureEvent("pointerdown", init));
  });
}

export function pointerMove(element: Element, init: PointerGestureInit = {}): void {
  act(() => {
    element.dispatchEvent(gestureEvent("pointermove", init));
  });
}

export function pointerUp(element: Element, init: PointerGestureInit = {}): void {
  act(() => {
    element.dispatchEvent(gestureEvent("pointerup", init));
  });
}

export function pointerCancel(element: Element, init: PointerGestureInit = {}): void {
  act(() => {
    element.dispatchEvent(gestureEvent("pointercancel", init));
  });
}

export function lostPointerCapture(element: Element, pointerId = 1): void {
  act(() => {
    element.dispatchEvent(new PointerEvent("lostpointercapture", { bubbles: true, pointerId }));
  });
}

export function advanceTimers(ms: number): void {
  act(() => {
    vi.advanceTimersByTime(ms);
  });
}
