import { act } from "react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { flush, gpio, pinButton, renderCard, rerenderCard } from "./GpioCard.testUtils";
import { advanceTimers, keyDown, pointerDown, pointerUp } from "./GpioPinoutSvg.testUtils";

beforeEach(() => {
  vi.useFakeTimers();
});

afterEach(() => {
  vi.useRealTimers();
  document.body.replaceChildren();
  localStorage.clear();
});

describe("GpioCard gesture actions", () => {
  it("drives output LOW through onSet only after the 220ms double window", async () => {
    const onSet = vi.fn().mockResolvedValue(undefined);
    const view = renderCard([gpio(10)], onSet);
    const pin = pinButton(view.host, 10);

    pointerDown(pin);
    pointerUp(pin);
    expect(onSet).not.toHaveBeenCalled();

    advanceTimers(220);
    await flush();
    expect(onSet).toHaveBeenCalledTimes(1);
    expect(onSet).toHaveBeenCalledWith("sig10", "output", 0);
    view.close();
  });

  it("drives input acquisition on a double short press with no value argument", async () => {
    const onSet = vi.fn().mockResolvedValue(undefined);
    const view = renderCard([gpio(10)], onSet);
    const pin = pinButton(view.host, 10);

    pointerDown(pin);
    pointerUp(pin);
    advanceTimers(100);
    pointerDown(pin);
    pointerUp(pin);
    advanceTimers(1000);
    await flush();

    expect(onSet).toHaveBeenCalledTimes(1);
    expect(onSet).toHaveBeenCalledWith("sig10", "input");
    view.close();
  });

  it("drives output HIGH after a 600ms hold", async () => {
    const onSet = vi.fn().mockResolvedValue(undefined);
    const view = renderCard([gpio(10)], onSet);

    pointerDown(pinButton(view.host, 10));
    advanceTimers(600);
    await flush();

    expect(onSet).toHaveBeenCalledTimes(1);
    expect(onSet).toHaveBeenCalledWith("sig10", "output", 1);
    view.close();
  });

  it("drives keyboard keys immediately: Enter/Space/0 LOW, 1 HIGH, I input", async () => {
    const onSet = vi.fn().mockResolvedValue(undefined);
    const view = renderCard([gpio(10)], onSet);
    const pin = pinButton(view.host, 10);

    // Each request must settle before the next: the card allows at most one
    // in-flight GPIO request, so a flush follows every activation.
    keyDown(pin, "Enter");
    await flush();
    keyDown(pin, " ");
    await flush();
    keyDown(pin, "0");
    await flush();
    keyDown(pin, "1");
    await flush();
    keyDown(pin, "i");
    await flush();

    expect(onSet).toHaveBeenCalledTimes(5);
    expect(onSet).toHaveBeenNthCalledWith(1, "sig10", "output", 0);
    expect(onSet).toHaveBeenNthCalledWith(2, "sig10", "output", 0);
    expect(onSet).toHaveBeenNthCalledWith(3, "sig10", "output", 0);
    expect(onSet).toHaveBeenNthCalledWith(4, "sig10", "output", 1);
    expect(onSet).toHaveBeenNthCalledWith(5, "sig10", "input");
    view.close();
  });

  it("resolves the request name against the latest snapshot at dispatch time", async () => {
    const onSet = vi.fn().mockResolvedValue(undefined);
    const view = renderCard([gpio(10)], onSet);

    rerenderCard(view, [gpio(10, { name: "renamed" })], onSet);
    keyDown(pinButton(view.host, 10), "1");
    await flush();

    expect(onSet).toHaveBeenCalledWith("renamed", "output", 1);
    view.close();
  });
});

describe("GpioCard request locking", () => {
  it("allows at most one in-flight request with a synchronous lock", async () => {
    const resolvers: Array<() => void> = [];
    const onSet = vi.fn().mockImplementation(
      () =>
        new Promise<void>((resolve) => {
          resolvers.push(resolve);
        })
    );
    const view = renderCard([gpio(10), gpio(11)], onSet);

    keyDown(pinButton(view.host, 10), "1");
    keyDown(pinButton(view.host, 11), "Enter");
    pointerDown(pinButton(view.host, 11));
    pointerUp(pinButton(view.host, 11));
    advanceTimers(1000);
    await flush();
    expect(onSet).toHaveBeenCalledTimes(1);
    expect(onSet).toHaveBeenCalledWith("sig10", "output", 1);

    await act(async () => {
      resolvers[0]?.();
      await Promise.resolve();
    });
    keyDown(pinButton(view.host, 11), "Enter");
    await flush();
    expect(onSet).toHaveBeenCalledTimes(2);
    expect(onSet).toHaveBeenNthCalledWith(2, "sig11", "output", 0);
    view.close();
  });

  it("drops a gesture timer that completes into a pending state without writing", async () => {
    let resolveRequest: (() => void) | undefined;
    const onSet = vi.fn().mockImplementation(
      () =>
        new Promise<void>((resolve) => {
          resolveRequest = resolve;
        })
    );
    const view = renderCard([gpio(10), gpio(11)], onSet);

    pointerDown(pinButton(view.host, 10));
    pointerUp(pinButton(view.host, 10));
    keyDown(pinButton(view.host, 11), "1");
    advanceTimers(1000);
    await flush();

    expect(onSet).toHaveBeenCalledTimes(1);
    expect(onSet).toHaveBeenCalledWith("sig11", "output", 1);

    await act(async () => {
      resolveRequest?.();
      await Promise.resolve();
    });
    view.close();
  });
});
