import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import {
  DIRECTION_RING_R,
  GPIO_FILL_HIGH,
  GPIO_INPUT_DASH,
  GPIO_PENDING_STROKE,
  GPIO_RING_BRAND,
  advanceTimers,
  focusRing,
  gpio,
  gpioPin,
  holdArc,
  pendingRing,
  pinCircle,
  pointerCancel,
  pointerDown,
  pointerUp,
  renderSvg,
} from "./GpioPinoutSvg.testUtils";

beforeEach(() => {
  vi.useFakeTimers();
});

afterEach(() => {
  vi.useRealTimers();
  document.body.replaceChildren();
  localStorage.clear();
});

describe("GpioPinoutSvg gpio gesture accessibility and visual contract", () => {
  it("renders the danger hold arc on the direction ring geometry only while holding", () => {
    const onGpioAction = vi.fn();
    const view = renderSvg({ variant: "gpio", gpios: [gpio(10), gpio(11)], onGpioAction });
    const pin = gpioPin(view.host, 10);

    expect(holdArc(pin)).toBeNull();

    pointerDown(pin);
    const arc = holdArc(pin);
    const direction = pinCircle(pin, DIRECTION_RING_R);
    expect(arc).not.toBeNull();
    expect(arc?.getAttribute("r")).toBe(DIRECTION_RING_R);
    expect(arc?.getAttribute("cx")).toBe(direction.getAttribute("cx"));
    expect(arc?.getAttribute("cy")).toBe(direction.getAttribute("cy"));
    expect(arc?.getAttribute("fill")).toBe("none");
    expect(arc?.getAttribute("stroke")).toBe(GPIO_FILL_HIGH);
    expect(arc?.getAttribute("stroke-width")).toBe("2.5");
    expect(arc?.getAttribute("pathLength")).toBe("1");
    expect(arc?.getAttribute("vector-effect")).toBe("non-scaling-stroke");
    expect(arc?.getAttribute("class")).toContain("gpio-hold-arc");
    expect(arc?.getAttribute("class")).toContain("pointer-events-none");
    expect(holdArc(gpioPin(view.host, 11))).toBeNull();

    advanceTimers(600);
    expect(onGpioAction).toHaveBeenCalledTimes(1);
    expect(holdArc(pin)).toBeNull();

    pointerUp(pin);
    pointerDown(pin);
    expect(holdArc(pin)).not.toBeNull();
    pointerCancel(pin);
    expect(holdArc(pin)).toBeNull();
    view.close();
  });

  it("keeps the idle direction ring at r14 with dashed input and solid output", () => {
    const view = renderSvg({
      variant: "gpio",
      gpios: [gpio(10), gpio(11, { direction: "output" })],
      onGpioAction: vi.fn(),
    });

    const input = pinCircle(gpioPin(view.host, 10), DIRECTION_RING_R);
    expect(input.getAttribute("stroke-width")).toBe("2.5");
    expect(input.getAttribute("stroke-dasharray")).toBe(GPIO_INPUT_DASH);

    const output = pinCircle(gpioPin(view.host, 11), DIRECTION_RING_R);
    expect(output.getAttribute("stroke-width")).toBe("2.5");
    expect(output.getAttribute("stroke-dasharray")).toBeNull();
    view.close();
  });

  it("renders no separate hold track or r16 hold circle while holding", () => {
    const view = renderSvg({ variant: "gpio", gpios: [gpio(10)], onGpioAction: vi.fn() });
    const pin = gpioPin(view.host, 10);

    pointerDown(pin);
    const arc = holdArc(pin);
    if (!arc) throw new Error("hold arc missing");
    expect(pin.querySelectorAll('circle[r="16"]')).toHaveLength(1);
    expect(pin.querySelectorAll("circle.gpio-hold-arc")).toHaveLength(1);

    const ring = focusRing(pin);
    if (!ring) throw new Error("focus ring missing");
    expect(ring.getAttribute("r")).toBe("16");
    expect(ring.getAttribute("stroke")).toBe(GPIO_RING_BRAND);
    expect(ring.getAttribute("class")).toContain("opacity-0");
    expect(ring.getAttribute("class")).toContain("group-focus-visible:opacity-100");

    advanceTimers(600);
    expect(holdArc(pin)).toBeNull();
    view.close();
  });

  it("disables every pin while pending and marks only the target as busy", () => {
    const view = renderSvg({
      variant: "gpio",
      gpios: [gpio(10), gpio(11)],
      onGpioAction: vi.fn(),
      gpioPendingPin: 10,
    });

    const target = gpioPin(view.host, 10);
    expect(target.getAttribute("aria-disabled")).toBe("true");
    expect(target.getAttribute("aria-busy")).toBe("true");
    expect(target.getAttribute("class")).toContain("opacity-40");
    expect(target.style.cursor).toBe("not-allowed");

    const other = gpioPin(view.host, 11);
    expect(other.getAttribute("aria-disabled")).toBe("true");
    expect(other.getAttribute("aria-busy")).toBeNull();
    expect(other.getAttribute("class")).not.toContain("opacity-40");
    expect(other.getAttribute("class")).not.toContain("hover:opacity-80");
    expect(other.style.cursor).toBe("not-allowed");
    view.close();
  });

  it("renders a warn dashed busy ring only on the pending target pin", () => {
    const onGpioAction = vi.fn();
    const view = renderSvg({
      variant: "gpio",
      gpios: [gpio(10), gpio(11)],
      onGpioAction,
      gpioPendingPin: 10,
    });

    const target = gpioPin(view.host, 10);
    const ring = pendingRing(target);
    if (!ring) throw new Error("pending ring missing");
    expect(ring.getAttribute("fill")).toBe("none");
    expect(ring.getAttribute("stroke")).toBe(GPIO_PENDING_STROKE);
    expect(ring.getAttribute("stroke-width")).toBe("1.5");
    expect(ring.getAttribute("stroke-dasharray")).not.toBeNull();
    expect(ring.getAttribute("vector-effect")).toBe("non-scaling-stroke");
    expect(ring.getAttribute("aria-hidden")).toBe("true");
    expect(ring.getAttribute("class")).toContain("animate-spin");
    expect(ring.getAttribute("class")).toContain("pointer-events-none");

    const focus = focusRing(target);
    if (!focus) throw new Error("focus ring missing");
    expect(focus.getAttribute("stroke")).toBe(GPIO_RING_BRAND);
    expect(ring.compareDocumentPosition(focus) & Node.DOCUMENT_POSITION_FOLLOWING).toBe(
      Node.DOCUMENT_POSITION_FOLLOWING
    );
    expect(holdArc(target)).toBeNull();

    const other = gpioPin(view.host, 11);
    expect(pendingRing(other)).toBeNull();
    expect(other.getAttribute("aria-disabled")).toBe("true");
    view.close();
  });

  it("renders no busy ring when no request is pending", () => {
    const view = renderSvg({
      variant: "gpio",
      gpios: [gpio(10)],
      onGpioAction: vi.fn(),
    });

    expect(pendingRing(gpioPin(view.host, 10))).toBeNull();
    view.close();
  });

  it("exposes no selection state and keeps keyboard hints in the accessibility contract", () => {
    const view = renderSvg({ variant: "gpio", gpios: [gpio(10)], onGpioAction: vi.fn() });
    const pin = gpioPin(view.host, 10);

    expect(pin.getAttribute("aria-pressed")).toBeNull();
    expect(pin.getAttribute("aria-keyshortcuts")).toBe("Enter Space 0 1 I");
    expect(pin.style.touchAction).toBe("manipulation");
    view.close();
  });

  it("connects every pin to the gesture instructions through aria-describedby", () => {
    const view = renderSvg({
      variant: "gpio",
      gpios: [gpio(10), gpio(11)],
      onGpioAction: vi.fn(),
      gpioInstructionsId: "gpio-gesture-hint",
    });

    expect(gpioPin(view.host, 10).getAttribute("aria-describedby")).toBe("gpio-gesture-hint");
    expect(gpioPin(view.host, 11).getAttribute("aria-describedby")).toBe("gpio-gesture-hint");
    view.close();
  });
});
