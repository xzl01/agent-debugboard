import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import {
  advanceTimers,
  click,
  contextMenu,
  gpio,
  gpioPin,
  keyDown,
  pointerDown,
  pointerUp,
  renderSvg,
  rerenderSvg,
} from "./GpioPinoutSvg.testUtils";

beforeEach(() => {
  vi.useFakeTimers();
});

afterEach(() => {
  vi.useRealTimers();
  document.body.replaceChildren();
  localStorage.clear();
});

describe("GpioPinoutSvg gpio gesture timing", () => {
  it("writes nothing on mount or on snapshot-driven rerender", () => {
    const onGpioAction = vi.fn();
    const view = renderSvg({ variant: "gpio", gpios: [gpio(10), gpio(11)], onGpioAction });

    rerenderSvg(view, {
      variant: "gpio",
      gpios: [gpio(10, { value: 1 }), gpio(11, { direction: "output", value: 1 })],
      onGpioAction,
    });
    advanceTimers(2000);

    expect(onGpioAction).not.toHaveBeenCalled();
    view.close();
  });

  it("requests output LOW only after the 220ms double window expires", () => {
    const onGpioAction = vi.fn();
    const view = renderSvg({ variant: "gpio", gpios: [gpio(10)], onGpioAction });
    const pin = gpioPin(view.host, 10);

    pointerDown(pin);
    pointerUp(pin);
    expect(onGpioAction).not.toHaveBeenCalled();

    advanceTimers(219);
    expect(onGpioAction).not.toHaveBeenCalled();

    advanceTimers(1);
    expect(onGpioAction).toHaveBeenCalledTimes(1);
    expect(onGpioAction).toHaveBeenCalledWith(10, { kind: "outputLow" });
    view.close();
  });

  it("requests input exactly once on a double short press with no LOW glitch", () => {
    const onGpioAction = vi.fn();
    const view = renderSvg({ variant: "gpio", gpios: [gpio(10)], onGpioAction });
    const pin = gpioPin(view.host, 10);

    pointerDown(pin);
    pointerUp(pin);
    advanceTimers(100);
    pointerDown(pin);
    pointerUp(pin);
    advanceTimers(1000);

    expect(onGpioAction).toHaveBeenCalledTimes(1);
    expect(onGpioAction).toHaveBeenCalledWith(10, { kind: "input" });
    view.close();
  });

  it("requests output HIGH exactly once when the pointer is still down at 600ms", () => {
    const onGpioAction = vi.fn();
    const view = renderSvg({ variant: "gpio", gpios: [gpio(10)], onGpioAction });
    const pin = gpioPin(view.host, 10);

    pointerDown(pin);
    advanceTimers(599);
    expect(onGpioAction).not.toHaveBeenCalled();

    advanceTimers(1);
    expect(onGpioAction).toHaveBeenCalledTimes(1);
    expect(onGpioAction).toHaveBeenCalledWith(10, { kind: "outputHigh" });

    advanceTimers(2000);
    expect(onGpioAction).toHaveBeenCalledTimes(1);
    view.close();
  });

  it("treats release after a completed hold as inert", () => {
    const onGpioAction = vi.fn();
    const view = renderSvg({ variant: "gpio", gpios: [gpio(10)], onGpioAction });
    const pin = gpioPin(view.host, 10);

    pointerDown(pin);
    advanceTimers(600);
    pointerUp(pin);
    advanceTimers(2000);

    expect(onGpioAction).toHaveBeenCalledTimes(1);
    expect(onGpioAction).toHaveBeenCalledWith(10, { kind: "outputHigh" });
    view.close();
  });

  it("runs a hold as the second press through the hold path, not the click path", () => {
    const onGpioAction = vi.fn();
    const view = renderSvg({ variant: "gpio", gpios: [gpio(10)], onGpioAction });
    const pin = gpioPin(view.host, 10);

    pointerDown(pin);
    pointerUp(pin);
    advanceTimers(100);
    pointerDown(pin);
    advanceTimers(600);
    pointerUp(pin);
    advanceTimers(2000);

    expect(onGpioAction).toHaveBeenCalledTimes(1);
    expect(onGpioAction).toHaveBeenCalledWith(10, { kind: "outputHigh" });
    view.close();
  });

  it("keeps per-pin gestures independent", () => {
    const onGpioAction = vi.fn();
    const view = renderSvg({ variant: "gpio", gpios: [gpio(10), gpio(11)], onGpioAction });

    pointerDown(gpioPin(view.host, 10));
    pointerUp(gpioPin(view.host, 10));
    advanceTimers(100);
    pointerDown(gpioPin(view.host, 11));
    pointerUp(gpioPin(view.host, 11));
    advanceTimers(120);

    expect(onGpioAction).toHaveBeenCalledTimes(1);
    expect(onGpioAction).toHaveBeenNthCalledWith(1, 10, { kind: "outputLow" });

    advanceTimers(100);
    expect(onGpioAction).toHaveBeenCalledTimes(2);
    expect(onGpioAction).toHaveBeenNthCalledWith(2, 11, { kind: "outputLow" });
    view.close();
  });

  it("handles keyboard immediately and ignores auto-repeat", () => {
    const onGpioAction = vi.fn();
    const view = renderSvg({ variant: "gpio", gpios: [gpio(10)], onGpioAction });
    const pin = gpioPin(view.host, 10);

    keyDown(pin, "Enter");
    keyDown(pin, " ");
    keyDown(pin, "0");
    keyDown(pin, "1");
    keyDown(pin, "i");
    keyDown(pin, "I");
    advanceTimers(2000);

    expect(onGpioAction).toHaveBeenCalledTimes(6);
    expect(onGpioAction).toHaveBeenNthCalledWith(1, 10, { kind: "outputLow" });
    expect(onGpioAction).toHaveBeenNthCalledWith(2, 10, { kind: "outputLow" });
    expect(onGpioAction).toHaveBeenNthCalledWith(3, 10, { kind: "outputLow" });
    expect(onGpioAction).toHaveBeenNthCalledWith(4, 10, { kind: "outputHigh" });
    expect(onGpioAction).toHaveBeenNthCalledWith(5, 10, { kind: "input" });
    expect(onGpioAction).toHaveBeenNthCalledWith(6, 10, { kind: "input" });

    keyDown(pin, "1", false, true);
    keyDown(pin, "Enter", false, true);
    advanceTimers(2000);
    expect(onGpioAction).toHaveBeenCalledTimes(6);
    view.close();
  });

  it("binds no native click, double-click, or context-menu path", () => {
    const onGpioAction = vi.fn();
    const onTogglePin = vi.fn();
    const onSetTriggerPin = vi.fn();
    const view = renderSvg({
      variant: "gpio",
      gpios: [gpio(10)],
      onGpioAction,
      onTogglePin,
      onSetTriggerPin,
    });
    const pin = gpioPin(view.host, 10);

    click(pin);
    click(pin);
    contextMenu(pin);
    keyDown(pin, "ContextMenu");
    keyDown(pin, "F10", true);
    advanceTimers(2000);

    expect(onGpioAction).not.toHaveBeenCalled();
    expect(onTogglePin).not.toHaveBeenCalled();
    expect(onSetTriggerPin).not.toHaveBeenCalled();
    view.close();
  });
});
