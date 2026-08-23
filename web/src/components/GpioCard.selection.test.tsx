import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { flush, gpio, pinButton, renderCard } from "./GpioCard.testUtils";
import { advanceTimers, click, keyDown, pointerDown, pointerUp } from "./GpioPinoutSvg.testUtils";

beforeEach(() => {
  vi.useFakeTimers();
});

afterEach(() => {
  vi.useRealTimers();
  document.body.replaceChildren();
  localStorage.clear();
});

describe("GpioCard selection regression", () => {
  it("renders no selection state, action buttons, or selected detail panel", () => {
    const onSet = vi.fn().mockResolvedValue(undefined);
    const view = renderCard([gpio(10, { direction: "output", value: 1 })], onSet);

    expect(view.host.querySelector("[aria-pressed]")).toBeNull();
    expect(view.host.querySelector("button")).toBeNull();
    expect(view.host.querySelector('[aria-label="GPIO actions"]')).toBeNull();
    expect(view.host.textContent).not.toContain("No pin selected.");
    expect(view.host.textContent).not.toContain("Current direction");
    expect(view.host.textContent).not.toContain("Current level");
    view.close();
  });

  it("does not select a pin on click, Enter, or Space and shows no selected details", () => {
    const onSet = vi.fn().mockResolvedValue(undefined);
    const view = renderCard([gpio(10, { direction: "output", value: 1 })], onSet);

    click(pinButton(view.host, 10));
    keyDown(pinButton(view.host, 10), "Enter");
    keyDown(pinButton(view.host, 10), " ");

    expect(view.host.textContent).not.toContain("sig10,");
    expect(view.host.textContent).not.toContain("Current direction");
    expect(view.host.querySelector("[aria-pressed]")).toBeNull();
    view.close();
  });

  it("drives the pin directly through gestures instead of a select-then-apply flow", async () => {
    const onSet = vi.fn().mockResolvedValue(undefined);
    const view = renderCard([gpio(10, { direction: "output", value: 1 })], onSet);

    pointerDown(pinButton(view.host, 10));
    pointerUp(pinButton(view.host, 10));
    advanceTimers(220);
    await flush();

    expect(onSet).toHaveBeenCalledTimes(1);
    expect(onSet).toHaveBeenCalledWith("sig10", "output", 0);
    expect(view.host.querySelector("button")).toBeNull();
    view.close();
  });
});
