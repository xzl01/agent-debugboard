import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { setGpio } from "@/lib/api";
import { directGpioFixtureCases, physicalLayoutFixture } from "@/lib/gpioPhysicalLayout.testFixtures";
import { GpioCard } from "./GpioCard";
import { flush, mount, pinButton } from "./GpioCard.testUtils";
import { advanceTimers, click, keyDown, pointerDown, pointerUp } from "./GpioPinoutSvg.testUtils";

const applyGpio = async (...args: Parameters<typeof setGpio>): Promise<void> => { await setGpio(...args); };

beforeEach(() => vi.useFakeTimers());
afterEach(() => { vi.useRealTimers(); vi.unstubAllGlobals(); document.body.replaceChildren(); localStorage.clear(); });

describe("physical GPIO safety at the request boundary", () => {
  it("never PUTs GP29 output via tap, hold, second-press hold or keyboard, but keeps input", async () => {
    const { layout, gpios } = physicalLayoutFixture();
    const fetchMock = vi.fn().mockResolvedValue(new Response("{}"));
    vi.stubGlobal("fetch", fetchMock);
    const view = mount(<GpioCard gpios={gpios} physicalLayout={layout} onSet={applyGpio} />);
    const pin = pinButton(view.host, 29);
    expect(pin.querySelector("text")?.textContent).toBe("ADC3");
    for (const key of ["Enter", " ", "0", "1"]) keyDown(pin, key);
    pointerDown(pin); pointerUp(pin); advanceTimers(220);
    pointerDown(pin); advanceTimers(600); pointerUp(pin);
    pointerDown(pin); pointerUp(pin); advanceTimers(100); pointerDown(pin); advanceTimers(600); pointerUp(pin);
    await flush();
    expect(fetchMock).not.toHaveBeenCalled();
    expect(pin.querySelector(".gpio-hold-arc")).toBeNull();
    expect(pin.getAttribute("aria-keyshortcuts")).toBe("I");
    expect(pin.getAttribute("aria-label")).toContain("Input-only");
    keyDown(pin, "i");
    await flush();
    expect(fetchMock).toHaveBeenCalledTimes(1);
    expect(fetchMock).toHaveBeenCalledWith("/api/v1/gpio/GP29", expect.objectContaining({ method: "PUT", body: JSON.stringify({ direction: "input" }) }));
    view.close();
  });

  it("dispatches the exact gpioName directly from its physical circle, not its physical or MCU number", async () => {
    const { layout, gpios } = physicalLayoutFixture();
    const renamed = gpios.map((gpio) => gpio.name === "GP14" ? { ...gpio, name: "target_reset" } : gpio);
    const physicalLayout = { ...layout, groups: layout.groups.map((group) => ({ ...group, cells: group.cells.map((cell) => cell.gpioName === "GP14" ? { ...cell, gpioName: "target_reset" } : cell) })) };
    const onSet = vi.fn().mockResolvedValue(undefined);
    const view = mount(<GpioCard gpios={renamed} physicalLayout={physicalLayout} onSet={onSet} />);
    const physical = view.host.querySelector('[data-layout-group="J8"] [data-physical-cell="3"]')!;
    const button = physical.querySelector('[role="button"]')!;
    keyDown(button, "1");
    await flush();
    expect(onSet).toHaveBeenCalledExactlyOnceWith("target_reset", "output", 1);
    expect(button.getAttribute("aria-label")).toContain("J8 pin 3");
    view.close();
  });

  it("all inert cells produce zero requests from click, pointer and keys", async () => {
    const { layout, gpios } = physicalLayoutFixture();
    const fetchMock = vi.fn();
    vi.stubGlobal("fetch", fetchMock);
    const view = mount(<GpioCard gpios={gpios} physicalLayout={layout} onSet={applyGpio} />);
    expect(view.host.querySelectorAll("[data-physical-cell]")).toHaveLength(24);
    expect(view.host.querySelectorAll("[data-physical-inert]")).toHaveLength(13);
    expect(view.host.querySelectorAll('[role="button"]')).toHaveLength(11);
    for (const cell of view.host.querySelectorAll("[data-physical-inert]")) {
      click(cell); pointerDown(cell); pointerUp(cell); keyDown(cell, "Enter"); keyDown(cell, "1"); keyDown(cell, "i");
    }
    advanceTimers(1000);
    await flush();
    expect(fetchMock).not.toHaveBeenCalled();
    view.close();
  });

  it.each(directGpioFixtureCases)("sends every $gpioName physical gesture only to its exact GPIO endpoint, never a switch API", async (entry) => {
    const { layout, gpios } = physicalLayoutFixture();
    const fetchMock = vi.fn().mockImplementation(async () => new Response("{}"));
    vi.stubGlobal("fetch", fetchMock);
    const view = mount(<GpioCard gpios={gpios} physicalLayout={layout} onSet={applyGpio} />);
    const pin = view.host.querySelector(`[data-layout-group="J8"] [data-physical-cell="${entry.pinNumber}"] [role="button"]`)!;
    pointerDown(pin); pointerUp(pin); advanceTimers(220); await flush();
    pointerDown(pin); advanceTimers(600); pointerUp(pin); await flush();
    pointerDown(pin); pointerUp(pin); advanceTimers(100); pointerDown(pin); pointerUp(pin); await flush();
    for (const key of ["Enter", " ", "0", "1", "i"]) { keyDown(pin, key); await flush(); }
    const requests = fetchMock.mock.calls.map(([url, init]) => ({ url, method: init.method, body: JSON.parse(init.body) }));
    expect(requests).toHaveLength(8);
    expect(requests.every((request) => request.url === `/api/v1/gpio/${entry.gpioName}` && request.method === "PUT")).toBe(true);
    expect(requests.map((request) => request.body)).toEqual([
      { direction: "output", value: 0 }, { direction: "output", value: 1 }, { direction: "input" },
      { direction: "output", value: 0 }, { direction: "output", value: 0 }, { direction: "output", value: 0 },
      { direction: "output", value: 1 }, { direction: "input" },
    ]);
    expect(requests.some((request) => request.url.includes("/switch/"))).toBe(false);
    view.close();
  });

  it.each(["unsupported", null])("has no fallback controls when layout is missing/error: %s", (layoutError) => {
    const { gpios } = physicalLayoutFixture();
    const view = mount(<GpioCard gpios={gpios} layoutError={layoutError} onSet={vi.fn()} />);
    expect(view.host.querySelector('[role="button"]')).toBeNull();
    expect(view.host.textContent).toContain("read-only");
    view.close();
  });

  it("fails closed when layout and dynamic names do not form a bijection", () => {
    const { layout, gpios } = physicalLayoutFixture();
    const view = mount(<GpioCard gpios={gpios.slice(1)} physicalLayout={layout} onSet={vi.fn()} />);
    expect(view.host.querySelector('[role="button"]')).toBeNull();
    expect(view.host.querySelector('[role="alert"]')?.textContent).toContain("unmatched controllable cell");
    view.close();
  });
});
