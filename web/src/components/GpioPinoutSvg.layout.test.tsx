// @vitest-environment jsdom
import { afterEach, describe, expect, it } from "vitest";
import { gpio, gpioPin, laPin, renderSvg } from "./GpioPinoutSvg.testUtils";

afterEach(() => {
  document.body.replaceChildren();
  localStorage.clear();
});

describe("GpioPinoutSvg firmware-driven layout groups", () => {
  it("renders pins without valid layout metadata in a visible fallback group", () => {
    const view = renderSvg({
      variant: "gpio",
      gpios: [
        gpio(10),
        gpio(99, { layoutGroup: undefined, layoutRow: undefined, layoutColumn: undefined }),
        gpio(98, { layoutRow: -1 }),
        gpio(97, { layoutGroup: "J16", layoutRow: 0, layoutColumn: 0 }),
      ],
    });
    expect(gpioPin(view.host, 99)).toBeTruthy();
    expect(gpioPin(view.host, 98)).toBeTruthy();
    expect(view.host.textContent).toContain("Other");
    view.close();
  });

  it("keeps duplicate-cell colliding pins visible and actionable", () => {
    const view = renderSvg({
      variant: "gpio",
      gpios: [
        gpio(10, { layoutRow: 0, layoutColumn: 0 }),
        gpio(11, { layoutRow: 0, layoutColumn: 0 }),
      ],
    });
    expect(gpioPin(view.host, 10)).toBeTruthy();
    expect(gpioPin(view.host, 11)).toBeTruthy();
    view.close();
  });

  it("renders arbitrary firmware connector groups without host-side geometry", () => {
    const view = renderSvg({
      variant: "logic-analyzer",
      selectedPins: [1, 2, 3],
      gpios: [
        gpio(1, { layoutGroup: "HDR9", layoutRow: 0, layoutColumn: 0 }),
        gpio(2, { layoutGroup: "HDR9", layoutRow: 3, layoutColumn: 2 }),
        gpio(3, { layoutGroup: "X1", layoutRow: 0, layoutColumn: 0 }),
      ],
    });
    expect(laPin(view.host, "GP1")).toBeTruthy();
    expect(laPin(view.host, "GP2")).toBeTruthy();
    expect(laPin(view.host, "GP3")).toBeTruthy();
    expect(view.host.textContent).toContain("HDR9");
    expect(view.host.textContent).toContain("X1");
    view.close();
  });
});
