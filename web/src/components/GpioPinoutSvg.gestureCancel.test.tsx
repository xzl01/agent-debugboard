import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import {
  advanceTimers,
  gpio,
  gpioPin,
  holdArc,
  keyDown,
  lostPointerCapture,
  pointerCancel,
  pointerDown,
  pointerMove,
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

describe("GpioPinoutSvg gpio gesture cancellation", () => {
  it("keeps the gesture alive when movement stays within 8px", () => {
    const onGpioAction = vi.fn();
    const view = renderSvg({ variant: "gpio", gpios: [gpio(10)], onGpioAction });
    const pin = gpioPin(view.host, 10);

    pointerDown(pin, { clientX: 100, clientY: 100 });
    pointerMove(pin, { clientX: 108, clientY: 100 });
    advanceTimers(600);

    expect(onGpioAction).toHaveBeenCalledTimes(1);
    expect(onGpioAction).toHaveBeenCalledWith(10, { kind: "outputHigh" });
    view.close();
  });

  it("cancels without writing when movement exceeds 8px", () => {
    const onGpioAction = vi.fn();
    const view = renderSvg({ variant: "gpio", gpios: [gpio(10)], onGpioAction });
    const pin = gpioPin(view.host, 10);

    pointerDown(pin, { clientX: 100, clientY: 100 });
    pointerMove(pin, { clientX: 106, clientY: 106 });
    advanceTimers(2000);

    expect(onGpioAction).not.toHaveBeenCalled();
    view.close();
  });

  it("cancels without writing on pointercancel", () => {
    const onGpioAction = vi.fn();
    const view = renderSvg({ variant: "gpio", gpios: [gpio(10)], onGpioAction });
    const pin = gpioPin(view.host, 10);

    pointerDown(pin);
    advanceTimers(300);
    pointerCancel(pin);
    advanceTimers(2000);

    expect(onGpioAction).not.toHaveBeenCalled();
    expect(holdArc(pin)).toBeNull();
    view.close();
  });

  it("cancels without writing when an active hold loses pointer capture", () => {
    const onGpioAction = vi.fn();
    const view = renderSvg({ variant: "gpio", gpios: [gpio(10)], onGpioAction });
    const pin = gpioPin(view.host, 10);

    pointerDown(pin);
    advanceTimers(300);
    lostPointerCapture(pin);
    advanceTimers(2000);

    expect(onGpioAction).not.toHaveBeenCalled();
    expect(holdArc(pin)).toBeNull();
    view.close();
  });

  it("preserves the deferred LOW when capture is lost after the release", () => {
    const onGpioAction = vi.fn();
    const view = renderSvg({ variant: "gpio", gpios: [gpio(10)], onGpioAction });
    const pin = gpioPin(view.host, 10);

    pointerDown(pin);
    pointerUp(pin);
    lostPointerCapture(pin);
    advanceTimers(220);

    expect(onGpioAction).toHaveBeenCalledTimes(1);
    expect(onGpioAction).toHaveBeenCalledWith(10, { kind: "outputLow" });
    view.close();
  });

  it("cancels an in-progress hold and a pending LOW on Escape", () => {
    const onGpioAction = vi.fn();
    const view = renderSvg({ variant: "gpio", gpios: [gpio(10)], onGpioAction });
    const pin = gpioPin(view.host, 10);

    pointerDown(pin);
    advanceTimers(300);
    keyDown(pin, "Escape");
    advanceTimers(2000);
    expect(onGpioAction).not.toHaveBeenCalled();
    expect(holdArc(pin)).toBeNull();

    pointerDown(pin);
    pointerUp(pin);
    keyDown(pin, "Escape");
    advanceTimers(2000);
    expect(onGpioAction).not.toHaveBeenCalled();
    view.close();
  });

  it("ignores non-primary buttons and non-primary pointers", () => {
    const onGpioAction = vi.fn();
    const view = renderSvg({ variant: "gpio", gpios: [gpio(10), gpio(11)], onGpioAction });
    const pin10 = gpioPin(view.host, 10);
    const pin11 = gpioPin(view.host, 11);

    pointerDown(pin10, { button: 2 });
    pointerUp(pin10, { button: 2 });
    pointerDown(pin11, { pointerId: 2, isPrimary: false });
    pointerUp(pin11, { pointerId: 2, isPrimary: false });
    advanceTimers(2000);

    expect(onGpioAction).not.toHaveBeenCalled();
    view.close();
  });

  it("clears every scheduled timer on unmount", () => {
    const onGpioAction = vi.fn();
    const view = renderSvg({ variant: "gpio", gpios: [gpio(10), gpio(11)], onGpioAction });
    const baseline = vi.getTimerCount();

    pointerDown(gpioPin(view.host, 10));
    pointerDown(gpioPin(view.host, 11));
    pointerUp(gpioPin(view.host, 11));
    expect(vi.getTimerCount()).toBeGreaterThan(baseline);

    view.close();
    expect(vi.getTimerCount()).toBe(baseline);
    expect(onGpioAction).not.toHaveBeenCalled();
  });
});
