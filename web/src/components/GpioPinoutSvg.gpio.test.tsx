import { act } from "react";
import { afterEach, describe, expect, it } from "vitest";
import { LanguageProvider } from "@/lib/i18n";
import { GpioPinoutSvg } from "./GpioPinoutSvg";
import {
  DIRECTION_RING_R,
  FOCUS_RING_R,
  GPIO_DIRECTION_STROKE,
  GPIO_FILL_HIGH,
  GPIO_FILL_LOW,
  GPIO_LABEL_ON_LEVEL,
  GPIO_RING_BRAND,
  HIT_TARGET_R,
  LEVEL_DISC_R,
  gpio,
  gpioPin,
  pinCircle,
  renderSvg,
} from "./GpioPinoutSvg.testUtils";

afterEach(() => {
  document.body.replaceChildren();
  localStorage.clear();
});

describe("GpioPinoutSvg gpio variant concentric geometry", () => {
  it("renders a separate strokeless level disc colored by the live SafeGpio value", () => {
    const view = renderSvg({
      variant: "gpio",
      gpios: [
        gpio(10, { direction: "input", value: 0 }),
        gpio(11, { direction: "output", value: 1 }),
        gpio(12, { direction: "output", value: 0 }),
        gpio(13, { direction: "input", value: 1 }),
      ],
    });

    expect(pinCircle(gpioPin(view.host, 10), LEVEL_DISC_R).getAttribute("fill")).toBe(GPIO_FILL_LOW);
    expect(pinCircle(gpioPin(view.host, 11), LEVEL_DISC_R).getAttribute("fill")).toBe(GPIO_FILL_HIGH);
    expect(pinCircle(gpioPin(view.host, 12), LEVEL_DISC_R).getAttribute("fill")).toBe(GPIO_FILL_LOW);
    expect(pinCircle(gpioPin(view.host, 13), LEVEL_DISC_R).getAttribute("fill")).toBe(GPIO_FILL_HIGH);
    expect(pinCircle(gpioPin(view.host, 11), LEVEL_DISC_R).getAttribute("stroke")).toBeNull();
    view.close();
  });

  it("follows live rerenders of SafeGpio.value on the level disc without caching levels", () => {
    const gpios = [gpio(10, { direction: "input", value: 0 })];
    const view = renderSvg({ variant: "gpio", gpios });
    expect(pinCircle(gpioPin(view.host, 10), LEVEL_DISC_R).getAttribute("fill")).toBe(GPIO_FILL_LOW);

    act(() => {
      view.root.render(
        <LanguageProvider>
          <GpioPinoutSvg variant="gpio" gpios={[gpio(10, { direction: "input", value: 1 })]} />
        </LanguageProvider>
      );
    });
    expect(pinCircle(gpioPin(view.host, 10), LEVEL_DISC_R).getAttribute("fill")).toBe(GPIO_FILL_HIGH);
    view.close();
  });

  it("carries direction on a neutral outer ring that never takes the level color", () => {
    const view = renderSvg({
      variant: "gpio",
      gpios: [
        gpio(10, { direction: "input", value: 1 }),
        gpio(11, { direction: "output", value: 1 }),
        gpio(12, { direction: "input", value: 0 }),
      ],
    });

    const highInput = pinCircle(gpioPin(view.host, 10), DIRECTION_RING_R);
    expect(highInput.getAttribute("stroke")).toBe(GPIO_DIRECTION_STROKE);
    expect(highInput.getAttribute("stroke-width")).toBe("2.5");
    expect(highInput.getAttribute("fill")).toBe("none");
    expect(highInput.getAttribute("stroke-dasharray")).toBe("3 2");

    const highOutput = pinCircle(gpioPin(view.host, 11), DIRECTION_RING_R);
    expect(highOutput.getAttribute("stroke")).toBe(GPIO_DIRECTION_STROKE);
    expect(highOutput.getAttribute("stroke-dasharray")).toBeNull();

    const lowInput = pinCircle(gpioPin(view.host, 12), DIRECTION_RING_R);
    expect(lowInput.getAttribute("stroke")).toBe(GPIO_DIRECTION_STROKE);
    expect(lowInput.getAttribute("stroke-dasharray")).toBe("3 2");
    view.close();
  });

  it("uses non-scaling strokes on the direction and focus rings", () => {
    const view = renderSvg({
      variant: "gpio",
      gpios: [gpio(10), gpio(11)],
    });

    for (const pin of [gpioPin(view.host, 10), gpioPin(view.host, 11)]) {
      for (const r of [DIRECTION_RING_R, FOCUS_RING_R]) {
        expect(pinCircle(pin, r).getAttribute("vector-effect")).toBe("non-scaling-stroke");
      }
    }
    view.close();
  });

  it("keeps the keyboard-only brand focus ring outside the direction ring", () => {
    const view = renderSvg({ variant: "gpio", gpios: [gpio(10)] });

    const pin = gpioPin(view.host, 10);
    expect(pin.getAttribute("class")).toContain("group");
    expect(pin.getAttribute("class")).toContain("focus:outline-none");
    expect(pin.getAttribute("class")).not.toContain("focus-visible:outline-none");
    const ring = pinCircle(pin, FOCUS_RING_R);
    expect(ring.getAttribute("fill")).toBe("none");
    expect(ring.getAttribute("stroke")).toBe(GPIO_RING_BRAND);
    expect(ring.getAttribute("stroke-width")).toBe("1.5");
    expect(ring.getAttribute("class")).toContain("opacity-0");
    expect(ring.getAttribute("class")).toContain("group-focus-visible:opacity-100");
    view.close();
  });

  it("keeps the transparent r17 hit target rendered at >=44px in the supported layout", () => {
    const view = renderSvg({
      variant: "gpio",
      gpios: [
        gpio(7, { layoutGroup: "J13", layoutRow: 0, layoutColumn: 0 }),
        gpio(8, { layoutGroup: "J13", layoutRow: 0, layoutColumn: 1 }),
        gpio(10),
      ],
    });

    expect(pinCircle(gpioPin(view.host, 10), HIT_TARGET_R).getAttribute("fill")).toBe(
      "transparent"
    );
    const svg = view.host.querySelector("svg");
    const viewBoxWidth = Number(svg?.getAttribute("viewBox")?.split(" ")[2]);
    const maxWidth = Number.parseFloat(svg?.style.maxWidth ?? "");
    expect(viewBoxWidth).toBeGreaterThan(0);
    expect(maxWidth).toBeGreaterThan(0);
    expect(2 * 17 * (maxWidth / viewBoxWidth)).toBeGreaterThanOrEqual(44);
    view.close();
  });

  it("keeps ordinary page scrolling available through touch-action manipulation", () => {
    const view = renderSvg({ variant: "gpio", gpios: [gpio(10)] });

    expect(gpioPin(view.host, 10).style.touchAction).toBe("manipulation");
    view.close();
  });

  it("exposes direction and level in the accessible name instead of relying on color", () => {
    const view = renderSvg({
      variant: "gpio",
      gpios: [gpio(11, { direction: "output", value: 1 })],
    });

    const pin = gpioPin(view.host, 11);
    expect(pin.getAttribute("aria-label")).toBe("sig11 GP11, output, high");
    expect(pin.getAttribute("tabindex")).toBe("0");
    expect(pin.querySelector("text")?.getAttribute("fill")).toBe(GPIO_LABEL_ON_LEVEL);
    view.close();
  });

  it("explains level and direction with a 2x2 four-item legend", () => {
    const view = renderSvg({ variant: "gpio", gpios: [gpio(10)] });

    const legend = view.host.querySelector(".grid.grid-cols-2");
    expect(legend?.textContent).toContain("low");
    expect(legend?.textContent).toContain("high");
    expect(legend?.textContent).toContain("input");
    expect(legend?.textContent).toContain("output");

    const swatches = Array.from(
      legend?.querySelectorAll<HTMLSpanElement>("span.inline-block") ?? []
    );
    const styles = swatches.map((swatch) => swatch.getAttribute("style") ?? "");
    expect(styles.some((s) => s.includes("dashed") && s.includes(GPIO_DIRECTION_STROKE))).toBe(
      true
    );
    expect(styles.some((s) => s.includes("solid") && s.includes(GPIO_DIRECTION_STROKE))).toBe(true);
    expect(styles.some((s) => s.includes(GPIO_FILL_LOW))).toBe(true);
    expect(styles.some((s) => s.includes(GPIO_FILL_HIGH))).toBe(true);
    view.close();
  });

  it("uses a group role with a GPIO title and a low/high legend", () => {
    const view = renderSvg({ variant: "gpio", gpios: [gpio(10)] });

    const svg = view.host.querySelector("svg");
    expect(svg?.getAttribute("role")).toBe("group");
    expect(svg?.querySelector("title")?.textContent).toBe("Safe GPIO pinout");
    const legend = svg?.parentElement?.querySelector(".grid.grid-cols-2");
    expect(legend?.textContent).toContain("low");
    expect(legend?.textContent).toContain("high");
    expect(view.host.textContent).not.toContain("Selected for capture");
    view.close();
  });
});
