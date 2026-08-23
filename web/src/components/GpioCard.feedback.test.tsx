import { act } from "react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { useI18n } from "@/lib/i18n";
import { GpioCard } from "./GpioCard";
import { flush, gpio, mount, pinButton, renderCard, rerenderCard } from "./GpioCard.testUtils";
import {
  GPIO_FILL_HIGH,
  GPIO_FILL_LOW,
  advanceTimers,
  keyDown,
  pinCircle,
} from "./GpioPinoutSvg.testUtils";

beforeEach(() => {
  vi.useFakeTimers();
});

afterEach(() => {
  vi.useRealTimers();
  document.body.replaceChildren();
  localStorage.clear();
});

describe("GpioCard feedback", () => {
  it("surfaces a rejected request through the inline alert", async () => {
    const onSet = vi.fn().mockRejectedValue(new Error("gpio boom"));
    const view = renderCard([gpio(10)], onSet);

    keyDown(pinButton(view.host, 10), "1");
    await flush();

    const alert = view.host.querySelector('[role="alert"]');
    expect(alert?.textContent).toContain("gpio boom");
    expect(view.host.querySelector('[role="status"]')).toBeNull();
    view.close();
  });

  it("announces a polite localized status after a successful action", async () => {
    const onSet = vi.fn().mockResolvedValue(undefined);
    const view = renderCard([gpio(10)], onSet);

    keyDown(pinButton(view.host, 10), "1");
    await flush();
    const status = view.host.querySelector('[role="status"]');
    expect(status?.getAttribute("aria-live")).toBe("polite");
    expect(status?.textContent).toContain("Output high applied to sig10.");
    expect(view.host.querySelector('[role="alert"]')).toBeNull();

    keyDown(pinButton(view.host, 10), "i");
    await flush();
    expect(view.host.querySelector('[role="status"]')?.textContent).toContain(
      "Input applied to sig10."
    );
    view.close();
  });

  it("re-localizes a stored success status when the language changes", async () => {
    let switchLang: ((lang: "en" | "zh") => void) | undefined;
    function LangProbe(): null {
      const { setLang } = useI18n();
      switchLang = setLang;
      return null;
    }
    const onSet = vi.fn().mockResolvedValue(undefined);
    const view = mount(
      <>
        <LangProbe />
        <GpioCard gpios={[gpio(10)]} onSet={onSet} />
      </>
    );
    keyDown(pinButton(view.host, 10), "1");
    await flush();
    expect(view.host.querySelector('[role="status"]')?.textContent).toContain(
      "Output high applied to sig10."
    );

    act(() => {
      switchLang?.("zh");
    });

    expect(view.host.querySelector('[role="status"]')?.textContent).toContain(
      "已将 sig10 设为输出高电平。"
    );
    view.close();
  });

  it("disables every pin while pending and marks only the request target as busy", async () => {
    let resolveRequest: (() => void) | undefined;
    const onSet = vi.fn().mockImplementation(
      () =>
        new Promise<void>((resolve) => {
          resolveRequest = resolve;
        })
    );
    const view = renderCard([gpio(10), gpio(11)], onSet);

    keyDown(pinButton(view.host, 10), "1");
    expect(pinButton(view.host, 10).getAttribute("aria-disabled")).toBe("true");
    expect(pinButton(view.host, 10).getAttribute("aria-busy")).toBe("true");
    expect(pinButton(view.host, 11).getAttribute("aria-disabled")).toBe("true");
    expect(pinButton(view.host, 11).getAttribute("aria-busy")).toBeNull();

    await act(async () => {
      resolveRequest?.();
      await Promise.resolve();
    });
    expect(pinButton(view.host, 10).getAttribute("aria-disabled")).toBeNull();
    expect(pinButton(view.host, 10).getAttribute("aria-busy")).toBeNull();
    view.close();
  });

  it("connects every pin to the visible gesture hint through aria-describedby", () => {
    const onSet = vi.fn().mockResolvedValue(undefined);
    const view = renderCard([gpio(10), gpio(11)], onSet);

    const hint = view.host.querySelector("p[id]");
    expect(hint?.textContent).toContain("Tap: output low");
    const hintId = hint?.getAttribute("id");
    expect(hintId).toBeTruthy();
    expect(pinButton(view.host, 10).getAttribute("aria-describedby")).toBe(hintId);
    expect(pinButton(view.host, 11).getAttribute("aria-describedby")).toBe(hintId);
    view.close();
  });
});

describe("GpioCard feedback layout ordering", () => {
  function pinoutSection(host: HTMLElement): HTMLElement {
    const svg = host.querySelector('svg[role="group"]');
    const section = svg?.closest("section");
    if (!section) throw new Error("pinout section not found");
    return section;
  }

  it("renders a success status after the stable pinout section", async () => {
    // Given a GPIO card whose request will succeed
    const onSet = vi.fn().mockResolvedValue(undefined);
    const view = renderCard([gpio(10)], onSet);

    // When one pin action completes successfully
    keyDown(pinButton(view.host, 10), "1");
    await flush();

    // Then the status feedback appears after the pinout section in DOM order,
    // so mounting it cannot shift the pin geometry
    const status = view.host.querySelector('[role="status"]');
    expect(status?.textContent).toContain("Output high applied to sig10.");
    if (!status) throw new Error("success status feedback not found");
    const position = pinoutSection(view.host).compareDocumentPosition(status);
    expect(position & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
    view.close();
  });

  it("renders an error alert after the stable pinout section", async () => {
    // Given a GPIO card whose request will be rejected
    const onSet = vi.fn().mockRejectedValue(new Error("gpio boom"));
    const view = renderCard([gpio(10)], onSet);

    // When one pin action fails
    keyDown(pinButton(view.host, 10), "1");
    await flush();

    // Then the alert feedback appears after the pinout section in DOM order
    const alert = view.host.querySelector('[role="alert"]');
    expect(alert?.textContent).toContain("gpio boom");
    if (!alert) throw new Error("error alert feedback not found");
    const position = pinoutSection(view.host).compareDocumentPosition(alert);
    expect(position & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
    view.close();
  });
});

describe("GpioCard firmware authority", () => {
  it("is non-optimistic: the level disc changes only when a new snapshot arrives", async () => {
    const onSet = vi.fn().mockResolvedValue(undefined);
    const view = renderCard([gpio(10, { direction: "output", value: 0 })], onSet);

    keyDown(pinButton(view.host, 10), "1");
    await flush();
    expect(onSet).toHaveBeenCalledWith("sig10", "output", 1);
    expect(pinCircle(pinButton(view.host, 10), "11.5").getAttribute("fill")).toBe(GPIO_FILL_LOW);

    rerenderCard(view, [gpio(10, { direction: "output", value: 1 })], onSet);
    expect(pinCircle(pinButton(view.host, 10), "11.5").getAttribute("fill")).toBe(GPIO_FILL_HIGH);
    view.close();
  });

  it("never writes on mount or on snapshot rerender", () => {
    const onSet = vi.fn().mockResolvedValue(undefined);
    const view = renderCard([gpio(10, { direction: "input", value: 0 })], onSet);

    rerenderCard(view, [gpio(10, { direction: "output", value: 1 })], onSet);
    advanceTimers(2000);

    expect(onSet).not.toHaveBeenCalled();
    view.close();
  });
});
