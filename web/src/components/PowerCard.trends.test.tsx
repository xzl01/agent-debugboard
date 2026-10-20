import { act, StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { CurrentAdcReading, VoltageAdcReading } from "@/lib/types";
import { PowerCard } from "./PowerCard";
import { MeasurementSparkline } from "./PowerSparkline";
import { MEASUREMENT_HISTORY_LIMIT, MEASUREMENT_PREVIEW_SAMPLES } from "@/lib/adc";

vi.mock("@/lib/i18n", () => ({
  useI18n: () => ({ t: (key: string) => key }),
}));

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const closeViews: (() => void)[] = [];

function render(node: React.ReactNode) {
  const host = document.createElement("div");
  document.body.append(host);
  const root = createRoot(host);
  act(() => root.render(node));
  closeViews.push(() => {
    act(() => root.unmount());
    host.remove();
  });
  return {
    host,
    rerender(next: React.ReactNode) {
      vi.setSystemTime(Date.now() + 1000);
      act(() => root.render(next));
    },
  };
}

function reading(overrides: Partial<CurrentAdcReading> = {}): CurrentAdcReading {
  return {
    name: "5v_out",
    signal: "INA226_5V",
    kind: "current",
    unit: "uA",
    value: 200_000,
    power_enabled: true,
    voltage_valid: true,
    voltage_uv: 5_100_000,
    ...overrides,
  };
}

function voltageReading(overrides: Partial<VoltageAdcReading> = {}): VoltageAdcReading {
  return {
    name: "adc0",
    signal: "GPIO26_ADC0",
    kind: "voltage",
    unit: "uV",
    value: 2_608_696,
    ...overrides,
  };
}

function voltageReadings(): VoltageAdcReading[] {
  return [
    voltageReading({ note: "PD input divider; ADC pad voltage" }),
    voltageReading({ name: "adc1", signal: "GPIO27_ADC1", value: 1_100_000 }),
    voltageReading({ name: "adc2", signal: "GPIO28_ADC2", value: 2_200_000 }),
    voltageReading({ name: "adc3", signal: "GPIO29_ADC3", value: 1_650_000 }),
  ];
}

function chart(host: HTMLElement, rail = "5V"): SVGSVGElement {
  const svg = host.querySelector<SVGSVGElement>(`svg[role="img"][aria-label^="${rail} ·"]`);
  if (!svg) throw new Error("Measurement chart not found");
  return svg;
}

function points(svg: SVGSVGElement, metric: "current" | "voltage" | "power"): number[][] {
  return (svg.querySelector(`g[data-metric="${metric}"] polyline`)?.getAttribute("points") ?? "")
    .split(" ").filter(Boolean).map((point) => point.split(",").map(Number));
}

function legend(svg: SVGSVGElement, metric: "current" | "voltage" | "power"): HTMLLIElement {
  const item = svg.parentElement?.querySelector<HTMLLIElement>(`li[data-metric="${metric}"]`);
  if (!item) throw new Error(`Legend not found: ${metric}`);
  return item;
}

function trendButton(host: HTMLElement): HTMLButtonElement {
  const button = host.querySelector<HTMLButtonElement>("button[aria-expanded]");
  if (!button) throw new Error("Trend toggle not found");
  return button;
}

function expandChart(host: HTMLElement, rail = "5V") {
  const button = host.querySelector<HTMLButtonElement>(`button[aria-haspopup="dialog"][aria-label^="${rail} ·"]`);
  if (!button) throw new Error(`Expand button not found: ${rail}`);
  act(() => { button.focus(); button.click(); });
  const modal = document.querySelector<HTMLElement>('[role="dialog"]');
  if (!modal) throw new Error("Expanded chart dialog not found");
  return { modal, opener: button };
}

function dialogButton(modal: HTMLElement, key: string) {
  const button = [...modal.querySelectorAll<HTMLButtonElement>("button")].find((item) =>
    item.getAttribute("aria-label") === key || item.textContent === key,
  );
  if (!button) throw new Error(`Dialog button not found: ${key}`);
  return button;
}

describe("overlaid power measurement trends", () => {
  beforeEach(() => {
    vi.useFakeTimers();
    vi.setSystemTime(10_000);
  });
  afterEach(() => {
    closeViews.splice(0).forEach((close) => close());
    localStorage.clear();
    vi.useRealTimers();
  });

  it("defaults to one expanded SVG with three coloured traces, legends and independent scales", () => {
    expect(localStorage.getItem("linkr-power-trends-expanded")).toBeNull();
    const onSet = vi.fn();
    const view = render(<PowerCard outputs={[]} readings={[reading()]} onSet={onSet} />);
    expect(trendButton(view.host).getAttribute("aria-expanded")).toBe("true");
    expect(view.host.querySelector('[role="tablist"]')).toBeNull();
    expect(view.host.querySelectorAll('svg[role="img"]')).toHaveLength(1);
    const updated = reading({ value: 300_000, voltage_uv: 4_800_000 });
    view.rerender(<PowerCard outputs={[]} readings={[updated]} onSet={onSet} />);
    const svg = chart(view.host);
    expect(svg.querySelectorAll("g[data-metric]")).toHaveLength(3);
    expect(svg.querySelectorAll("polyline")).toHaveLength(3);
    expect(svg.getAttribute("aria-label")).toContain("power.chart.independentScales");
    expect(svg.parentElement?.textContent).toContain("power.chart.independentScales");
    expect(legend(svg, "current").textContent).toBe("power.chart.current (A)0.300 A");
    expect(legend(svg, "voltage").textContent).toBe("power.chart.voltage (V)4.800 V");
    expect(legend(svg, "power").textContent).toBe("power.chart.power (W)1.44 W");
    const colors = { current: "brand", voltage: "ok", power: "warn" };
    for (const metric of ["current", "voltage", "power"] as const) {
      expect(points(svg, metric)).toHaveLength(2);
      expect(svg.querySelector(`g[data-metric="${metric}"] polyline`)?.getAttribute("stroke"))
        .toBe(`rgb(var(--c-${colors[metric]}))`);
      expect(legend(svg, metric).querySelector("[style]")?.getAttribute("style"))
        .toContain(`color: rgb(var(--c-${colors[metric]}))`);
      const swatch = legend(svg, metric).querySelector<HTMLElement>('span[aria-hidden="true"]')!;
      expect(swatch.style.color).toBe(`rgb(var(--c-${colors[metric]}))`);
      expect(swatch.style.borderTopColor).toBe(`rgb(var(--c-${colors[metric]}))`);
      expect(swatch.style.borderStyle).toBe(metric === "voltage" ? "dashed" : metric === "power" ? "dotted" : "solid");
    }
    expect(svg.querySelector('g[data-metric="voltage"] polyline')?.getAttribute("stroke-dasharray")).toBe("5 3");
    expect(svg.querySelector('g[data-metric="power"] polyline')?.getAttribute("stroke-dasharray")).toBe("2 3");
    const currentPoints = points(svg, "current");
    const voltagePoints = points(svg, "voltage");
    const powerPoints = points(svg, "power");
    // Positive-only previews spend 85% of the drawable height above zero.
    const zeroY = Number(svg.querySelector('[data-reference="zero"]')?.getAttribute("y1"));
    expect(zeroY).toBeCloseTo(4 + (58 - 8) * 0.85);
    expect(currentPoints[1][1]).toBeLessThan(currentPoints[0][1]);
    expect(voltagePoints[1][1]).toBeGreaterThan(voltagePoints[0][1]);
    // Each metric's maximum reaches the same normalized height despite different numeric units.
    expect(currentPoints[1][1]).toBeCloseTo(voltagePoints[0][1]);
    expect(currentPoints[1][1]).toBeCloseTo(powerPoints[1][1]);
    view.rerender(<PowerCard outputs={[]} readings={[updated]} onSet={onSet} />);
    expect(points(chart(view.host), "current")).toEqual(currentPoints);
    expect(points(chart(view.host), "voltage")).toEqual(voltagePoints);
    expect(onSet).not.toHaveBeenCalled();
  });

  it.each(["preview", "expanded"] as const)("keeps the %s zero baseline readable but visually subordinate to data traces", (size) => {
    const view = render(<MeasurementSparkline mode="power" reading={reading()} />);
    view.rerender(<MeasurementSparkline mode="power" reading={reading({ value: 300_000 })} />);
    const svg = chart(size === "expanded" ? expandChart(view.host).modal : view.host);
    const baseline = svg.querySelector('line[data-reference="zero"]')!;
    expect(baseline.getAttribute("stroke")).toBe("rgb(var(--c-ink-dim))");
    expect(baseline.getAttribute("stroke-opacity")).toBe("0.7");
    expect(baseline.getAttribute("stroke-dasharray")).toBe("5 4");
    expect(baseline.getAttribute("stroke-width")).toBe("1");
    expect(baseline.getAttribute("vector-effect")).toBe("non-scaling-stroke");
    expect(baseline.parentElement?.querySelector("text")?.textContent).toBe("0");
    expect(baseline.parentElement?.querySelector("text")?.getAttribute("fill")).toBe("rgb(var(--c-ink-dim))");
    const colors = { current: "brand", voltage: "ok", power: "warn" };
    for (const metric of ["current", "voltage", "power"] as const) {
      const trace = svg.querySelector(`g[data-metric="${metric}"] polyline`)!;
      expect(trace.getAttribute("stroke")).toBe(`rgb(var(--c-${colors[metric]}))`);
      expect(Number(trace.getAttribute("stroke-width"))).toBeGreaterThan(Number(baseline.getAttribute("stroke-width")));
      expect(trace.getAttribute("stroke-opacity")).toBeNull();
      expect(baseline.compareDocumentPosition(trace) & Node.DOCUMENT_POSITION_FOLLOWING).not.toBe(0);
    }
  });

  it("respects a stored collapsed preference and persists manual show/hide actions", () => {
    localStorage.setItem("linkr-power-trends-expanded", "false");
    const view = render(<PowerCard outputs={[]} readings={[reading()]} onSet={vi.fn()} />);
    expect(trendButton(view.host).getAttribute("aria-expanded")).toBe("false");
    expect(view.host.querySelector('svg[role="img"]')).toBeNull();
    expect(view.host.querySelector('button[aria-haspopup="dialog"]')).toBeNull();
    act(() => trendButton(view.host).click());
    expect(view.host.querySelectorAll('svg[role="img"]')).toHaveLength(1);
    expect(localStorage.getItem("linkr-power-trends-expanded")).toBe("true");
    act(() => trendButton(view.host).click());
    expect(view.host.querySelector('svg[role="img"]')).toBeNull();
    expect(localStorage.getItem("linkr-power-trends-expanded")).toBe("false");
    const reopened = render(<PowerCard outputs={[]} readings={[reading()]} onSet={vi.fn()} />);
    expect(trendButton(reopened.host).getAttribute("aria-expanded")).toBe("false");
    expect(reopened.host.querySelector('svg[role="img"]')).toBeNull();
  });

  it("keeps compact cards free of charts even when trends are expanded", () => {
    localStorage.setItem("linkr-power-trends-expanded", "true");
    const view = render(<PowerCard outputs={[]} readings={[reading()]} onSet={vi.fn()} compact />);
    expect(view.host.querySelector('svg[role="img"]')).toBeNull();
    expect(view.host.querySelector("button[aria-expanded]")).toBeNull();
    expect(view.host.querySelector('button[aria-haspopup="dialog"]')).toBeNull();
    expect(localStorage.getItem("linkr-power-trends-expanded")).toBe("true");
  });

  it("renders four descriptor-driven voltage cards after the four current overlays", () => {
    const onSet = vi.fn();
    const currents = ["5v_out", "12v_out", "20v_out", "vdd_5v"].map((name) => reading({ name }));
    const voltages = voltageReadings();
    const view = render(<PowerCard outputs={[]} readings={[...currents, ...voltages]} onSet={onSet} />);
    expect(view.host.querySelectorAll('svg[role="img"]')).toHaveLength(8);
    const cards = [...view.host.querySelectorAll("h4")].map((heading) => heading.closest("li")!);
    expect(cards).toHaveLength(4);
    expect(cards[0].parentElement?.className).toContain("grid-cols-1");
    expect(cards[0].parentElement?.className).toContain("sm:grid-cols-2");
    expect(view.host.textContent?.split("adc.voltage.disclaimer")).toHaveLength(2);
    voltages.forEach((voltage, index) => {
      expect(cards[index].querySelector("h4")?.textContent).toBe(voltage.name);
      expect(cards[index].textContent).toContain(voltage.signal);
      expect(cards[index].textContent).toContain(`${(voltage.value / 1_000_000).toFixed(3)} V`);
      expect(chart(cards[index], voltage.name).querySelectorAll("g[data-metric]")).toHaveLength(1);
      expect(cards[index].querySelector('[role="switch"]')).toBeNull();
    });
    expect(cards[0].textContent).toContain(voltages[0].note);
    expect(cards[0].textContent).not.toContain("20.000 V");
    expect(onSet).not.toHaveBeenCalled();
  });

  it("keeps all reported voltage identities and latest values visible in compact mode", () => {
    const voltages = voltageReadings();
    const view = render(<PowerCard outputs={[]} readings={voltages} onSet={vi.fn()} compact />);
    expect([...view.host.querySelectorAll("h4")].map((heading) => heading.textContent)).toEqual(voltages.map(({ name }) => name));
    for (const voltage of voltages) {
      expect(view.host.textContent).toContain(voltage.signal);
      expect(view.host.textContent).toContain(`${(voltage.value / 1_000_000).toFixed(3)} V`);
    }
    expect(view.host.querySelector('svg[role="img"]')).toBeNull();
    expect(view.host.querySelector('button[aria-haspopup="dialog"]')).toBeNull();
  });

  it("never invents missing voltage cards and keeps a reported zero distinct from absence", () => {
    const onSet = vi.fn();
    const adc3 = voltageReadings()[3];
    const view = render(<PowerCard outputs={[]} readings={[reading(), adc3]} onSet={onSet} />);
    expect([...view.host.querySelectorAll("h4")].map((heading) => heading.textContent)).toEqual(["adc3"]);
    view.rerender(<PowerCard outputs={[]} readings={[reading()]} onSet={onSet} />);
    expect(view.host.querySelector("h4")).toBeNull();
    expect(view.host.textContent).not.toContain("adc.voltage.disclaimer");
    view.rerender(<PowerCard outputs={[]} readings={[reading(), { ...adc3, value: 0 }]} onSet={onSet} />);
    expect(view.host.querySelector("h4")?.closest("li")?.textContent).toContain("0.000 V");
    // An absent channel unmounts its history, so resumed samples never bridge the missing interval.
    expect(chart(view.host, "adc3").querySelector("polyline")).toBeNull();
    expect(onSet).not.toHaveBeenCalled();
  });

  it("plots nonzero sensor values for all three metrics even when the rail is reported disabled", () => {
    const onSet = vi.fn();
    const view = render(
      <PowerCard outputs={[]} readings={[reading({ power_enabled: false })]} onSet={onSet} />,
    );
    view.rerender(
      <PowerCard outputs={[]} readings={[reading({ power_enabled: false, value: 300_000, voltage_uv: 4_800_000 })]} onSet={onSet} />,
    );
    const svg = chart(view.host);
    expect(view.host.textContent).toContain("adc.disabled");
    expect(svg.querySelectorAll("polyline")).toHaveLength(3);
    expect(legend(svg, "current").textContent).toContain("0.300 A");
    expect(legend(svg, "voltage").textContent).toContain("4.800 V");
    expect(legend(svg, "power").textContent).toContain("1.44 W");
    for (const metric of ["current", "voltage", "power"] as const) {
      const tracePoints = points(svg, metric);
      expect(tracePoints).toHaveLength(2);
      // Positive measurements must be above the signed domain's zero baseline.
      const zeroY = Number(svg.querySelector('[data-reference="zero"]')?.getAttribute("y1"));
      expect(tracePoints.every(([, y]) => Number.isFinite(y) && y < zeroY)).toBe(true);
    }
    expect(onSet).not.toHaveBeenCalled();
  });

  it("retains negative current and power in the card, legend and independent signed trace domains", () => {
    const onSet = vi.fn();
    const sample = reading({ power_enabled: false, value: -300_000, voltage_uv: 4_800_000 });
    const view = render(<PowerCard outputs={[]} readings={[sample]} onSet={onSet} />);
    view.rerender(<PowerCard outputs={[]} readings={[{ ...sample, value: -100_000 }]} onSet={onSet} />);
    const negativeOnly = chart(view.host);
    const zeroY = Number(negativeOnly.querySelector('[data-reference="zero"]')?.getAttribute("y1"));
    expect(zeroY).toBeCloseTo(4 + (58 - 8) * 0.15);
    expect(legend(negativeOnly, "current").textContent).toContain("-0.100 A");
    expect(legend(negativeOnly, "power").textContent).toContain("-0.48 W");
    for (const metric of ["current", "power"] as const) {
      const trace = points(negativeOnly, metric);
      expect(trace[0][1]).toBeGreaterThan(trace[1][1]);
      expect(trace.every(([, y]) => y > zeroY && y < 54)).toBe(true);
    }
    expect(points(negativeOnly, "voltage").every(([, y]) => y < zeroY)).toBe(true);
    view.rerender(<PowerCard outputs={[]} readings={[{ ...sample, value: 0 }]} onSet={onSet} />);
    view.rerender(<PowerCard outputs={[]} readings={[{ ...sample, value: 300_000 }]} onSet={onSet} />);
    const mixedZeroY = Number(chart(view.host).querySelector('[data-reference="zero"]')?.getAttribute("y1"));
    expect(mixedZeroY).toBe(29);
    for (const metric of ["current", "power"] as const) {
      const trace = points(chart(view.host), metric);
      expect(trace[2][1]).toBe(mixedZeroY);
      expect(trace[3][1]).toBeLessThan(mixedZeroY);
      expect(trace[0][1] - mixedZeroY).toBeCloseTo(mixedZeroY - trace[3][1]);
    }
    expect(onSet).not.toHaveBeenCalled();
  });

  it("keeps tiny negative samples visible in a compact lower margin with every metric aligned at zero", () => {
    const sample = reading({ value: 300_000, voltage_uv: 4_800_000 });
    const view = render(<MeasurementSparkline mode="power" reading={sample} />);
    view.rerender(<MeasurementSparkline mode="power" reading={{ ...sample, value: 0, voltage_uv: 0 }} />);
    view.rerender(<MeasurementSparkline mode="power" reading={{ ...sample, value: -300 }} />);
    const svg = chart(view.host);
    const zeroY = Number(svg.querySelector('[data-reference="zero"]')?.getAttribute("y1"));
    expect(zeroY).toBeCloseTo(46.5);
    for (const metric of ["current", "voltage", "power"] as const) {
      const trace = points(svg, metric);
      expect(trace[0][1]).toBeLessThan(zeroY);
      expect(trace[1][1]).toBe(zeroY);
      expect(trace.every(([, y]) => y > 4 && y < 54)).toBe(true);
    }
    for (const metric of ["current", "power"] as const) {
      expect(points(svg, metric)[2][1]).toBeGreaterThan(zeroY);
    }
    expect(points(svg, "voltage")[2][1]).toBeLessThan(zeroY);
    expect(legend(svg, "current").textContent).toContain("-0.3 mA");
    expect(legend(svg, "power").textContent).toContain("-1.4 mW");
  });

  it("expands the shared negative area for substantial reverse current and power without clipping", () => {
    const sample = reading({ value: 300_000, voltage_uv: 4_800_000 });
    const view = render(<MeasurementSparkline mode="power" reading={sample} />);
    view.rerender(<MeasurementSparkline mode="power" reading={{ ...sample, value: 0, voltage_uv: 0 }} />);
    view.rerender(<MeasurementSparkline mode="power" reading={{ ...sample, value: -1_200_000, voltage_uv: 1_200_000 }} />);
    const svg = chart(view.host);
    const zeroY = Number(svg.querySelector('[data-reference="zero"]')?.getAttribute("y1"));
    expect(zeroY).toBeCloseTo(4 + (58 - 8) * 0.2);
    for (const metric of ["current", "power"] as const) {
      const trace = points(svg, metric);
      expect(trace[0][1]).toBeLessThan(zeroY);
      expect(trace[1][1]).toBe(zeroY);
      expect(trace[2][1]).toBeGreaterThan(zeroY);
      expect(trace.every(([, y]) => y > 4 && y < 54)).toBe(true);
      // Different current/power distributions must still share a zero position
      // while preserving each metric's signed proportions.
      expect((trace[2][1] - zeroY) / (zeroY - trace[0][1])).toBeCloseTo(metric === "current" ? 4 : 1);
    }
    expect(points(svg, "voltage")[1][1]).toBe(zeroY);
    expect(points(svg, "voltage").every(([, y]) => y > 4 && y <= zeroY)).toBe(true);
    expect(legend(svg, "current").textContent).toContain("-1.200 A");
    expect(legend(svg, "power").textContent).toContain("-1.44 W");
  });

  it.each([
    ["positive-only", 200_000],
    ["tiny reverse", -300],
    ["substantial reverse", -1_200_000],
  ])("keeps expanded signed domains symmetric for %s data and voltage nonnegative", (_label, latestCurrent) => {
    const sample = reading({ value: 300_000, voltage_uv: 4_800_000 });
    const view = render(<MeasurementSparkline mode="power" reading={sample} />);
    view.rerender(<MeasurementSparkline mode="power" reading={{ ...sample, value: latestCurrent }} />);
    const { modal } = expandChart(view.host);
    const svg = chart(modal);
    const zeroY = Number(svg.querySelector('[data-reference="zero"]')?.getAttribute("y1"));
    expect(zeroY).toBe(140);
    const maximumCurrent = Math.max(300_000, Math.abs(latestCurrent));
    for (const metric of ["current", "power"] as const) {
      const trace = points(svg, metric);
      expect(trace[0][1]).toBeCloseTo(zeroY - (136 / 1.15) * 300_000 / maximumCurrent);
      expect(trace[1][1]).toBeCloseTo(zeroY - (136 / 1.15) * latestCurrent / maximumCurrent);
      expect(trace.every(([, y]) => y > 4 && y < 276)).toBe(true);
      expect(modal.querySelector(`li[data-metric="${metric}"]`)?.textContent).toContain("power.chart.range ±");
    }
    const voltageLegend = modal.querySelector('li[data-metric="voltage"]')?.textContent;
    expect(voltageLegend).toContain("power.chart.range 0.000 V – 5.520 V");
    expect(voltageLegend).not.toContain("±");
    expect(points(svg, "voltage").every(([, y]) => y > 4 && y < zeroY)).toBe(true);
  });

  it("keeps current visible but never invents voltage or power when measured voltage is missing", () => {
    const onSet = vi.fn();
    const view = render(<PowerCard outputs={[]} readings={[reading({ voltage_valid: false })]} onSet={onSet} />);
    view.rerender(<PowerCard outputs={[]} readings={[reading({ voltage_uv: undefined })]} onSet={onSet} />);
    const svg = chart(view.host);
    expect(svg.querySelectorAll("polyline")).toHaveLength(1);
    expect(points(svg, "current")).toHaveLength(2);
    expect(points(svg, "power")).toHaveLength(0);
    expect(svg.querySelector('g[data-metric="voltage"]')?.childElementCount).toBe(0);
    expect(legend(svg, "voltage").textContent).toBe("power.chart.voltage (V)—");
    expect(legend(svg, "current").textContent).toContain("0.200 A");
    expect(legend(svg, "power").textContent).toBe("power.chart.power (W)—");
    expect(svg.parentElement?.textContent).toContain("power.chart.noVoltage");
    expect(svg.parentElement?.textContent).not.toContain("5.000 V");
    expect(svg.parentElement?.textContent).not.toContain("5.100 V");
    view.rerender(
      <PowerCard outputs={[]} readings={[reading({ power_enabled: false, voltage_uv: 0 })]} onSet={onSet} />,
    );
    expect(legend(chart(view.host), "voltage").textContent).toContain("0.000 V");
  });

  it("leaves voltage and power gaps at their original timestamps without interrupting current", () => {
    const view = render(<MeasurementSparkline mode="power" reading={reading()} />);
    view.rerender(<MeasurementSparkline mode="power" reading={reading({ voltage_valid: false })} />);
    expect(legend(chart(view.host), "voltage").textContent).toContain("—");
    expect(legend(chart(view.host), "voltage").textContent).not.toContain("5.100 V");
    expect(chart(view.host).querySelector('g[data-metric="voltage"] circle[cx="320"]')).toBeNull();
    view.rerender(<MeasurementSparkline mode="power" reading={reading({ voltage_uv: 4_900_000 })} />);
    expect(chart(view.host).querySelector('g[data-metric="voltage"] polyline')).toBeNull();
    view.rerender(<MeasurementSparkline mode="power" reading={reading({ voltage_uv: 5_000_000 })} />);
    const voltagePoints = points(chart(view.host), "voltage");
    expect(voltagePoints).toHaveLength(2);
    expect(voltagePoints[0][0]).toBeCloseTo(320 * 2 / 3);
    expect(voltagePoints[1][0]).toBe(320);
    expect(points(chart(view.host), "current")).toHaveLength(4);
    expect(points(chart(view.host), "power")).toHaveLength(2);
  });

  it("uses actual elapsed time on X and splits every trace after a long missing-sample interval", () => {
    const view = render(<MeasurementSparkline mode="power" reading={reading()} />);
    view.rerender(<MeasurementSparkline mode="power" reading={reading()} />); // 11 s
    vi.setSystemTime(12_000);
    view.rerender(<MeasurementSparkline mode="power" reading={reading()} />); // 13 s
    expect(points(chart(view.host), "current")[1][0]).toBeCloseTo(320 / 3);
    vi.setSystemTime(22_000);
    view.rerender(<MeasurementSparkline mode="power" reading={reading()} />); // 23 s, gap
    view.rerender(<MeasurementSparkline mode="power" reading={reading()} />); // 24 s
    for (const metric of ["current", "voltage", "power"]) {
      const segments = chart(view.host).querySelectorAll(`g[data-metric="${metric}"] polyline`);
      expect(segments).toHaveLength(2);
      expect(segments[0].getAttribute("points")?.split(" ")).toHaveLength(3);
      const resumed = segments[1].getAttribute("points")!.split(" ").map((point) => point.split(",").map(Number));
      expect(resumed[0][0]).toBeCloseTo(320 * 13 / 14);
      expect(resumed[1][0]).toBe(320);
    }
  });

  it("does not append duplicate initial samples under StrictMode", () => {
    const view = render(<StrictMode><MeasurementSparkline mode="power" reading={reading()} /></StrictMode>);
    expect(chart(view.host).querySelector("polyline")).toBeNull();
  });

  it("keeps each rail history isolated across telemetry updates and input ordering changes", () => {
    const onSet = vi.fn();
    const first = reading();
    const other = reading({ name: "12v_out", value: 600_000, voltage_uv: 11_800_000 });
    const view = render(<PowerCard outputs={[]} readings={[first, other]} onSet={onSet} />);
    view.rerender(
      <PowerCard outputs={[]} readings={[reading({ value: 300_000, voltage_uv: 4_900_000 }), other]} onSet={onSet} />,
    );
    expect(view.host.querySelectorAll('svg[role="img"]')).toHaveLength(2);
    expect(points(chart(view.host, "5V"), "current")).toHaveLength(2);
    expect(points(chart(view.host, "5V"), "voltage")).toHaveLength(2);
    expect(chart(view.host, "12V").querySelector("polyline")).toBeNull();
    view.rerender(
      <PowerCard outputs={[]} readings={[{ ...other, voltage_uv: 12_100_000 }, first]} onSet={onSet} />,
    );
    expect(points(chart(view.host, "5V"), "current")).toHaveLength(3);
    expect(points(chart(view.host, "5V"), "voltage")).toHaveLength(3);
    expect(points(chart(view.host, "12V"), "current")).toHaveLength(2);
    expect(points(chart(view.host, "12V"), "voltage")).toHaveLength(2);
    expect(legend(chart(view.host, "5V"), "current").textContent).toContain("0.200 A");
    expect(legend(chart(view.host, "5V"), "voltage").textContent).toContain("5.100 V");
    expect(legend(chart(view.host, "12V"), "current").textContent).toContain("0.600 A");
    expect(legend(chart(view.host, "12V"), "voltage").textContent).toContain("12.100 V");
  });

  it("starts a clean series when reused for another rail or sensor mode", () => {
    const first = reading();
    const view = render(<MeasurementSparkline mode="power" reading={first} />);
    view.rerender(<MeasurementSparkline mode="power" reading={reading({ voltage_uv: 4_900_000 })} />);
    expect(points(chart(view.host), "voltage")).toHaveLength(2);
    view.rerender(<MeasurementSparkline mode="power" reading={reading({ name: "12v_out", voltage_uv: 11_800_000 })} />);
    expect(chart(view.host, "12V").querySelector("polyline")).toBeNull();
    expect(legend(chart(view.host, "12V"), "voltage").textContent).toContain("11.800 V");
    view.rerender(
      <MeasurementSparkline mode="voltage" reading={{ kind: "voltage", unit: "uV", name: "adc3", signal: "GP29", value: 1_650_000 }} />,
    );
    const adc3 = chart(view.host, "adc3");
    expect(adc3.getAttribute("aria-label")).toBe("adc3 · GP29 · adc.voltage.chart.aria");
    expect(adc3.parentElement?.textContent).toContain("1.650 V");
    expect(adc3.parentElement?.querySelector("ul")).toBeNull();
    view.rerender(<MeasurementSparkline mode="power" reading={first} />);
    expect(chart(view.host).querySelector("polyline")).toBeNull();
    expect(legend(chart(view.host), "voltage").textContent).toContain("5.100 V");
  });

  it("retains independent voltage histories through descriptor reordering and resets on signal changes", () => {
    const onSet = vi.fn();
    const initial = voltageReadings();
    const view = render(<PowerCard outputs={[]} readings={initial} onSet={onSet} />);
    const updated = [{ ...initial[0], value: 2_700_000 }, ...initial.slice(1)];
    view.rerender(<PowerCard outputs={[]} readings={updated} onSet={onSet} />);
    expect(points(chart(view.host, "adc0"), "voltage")).toHaveLength(2);
    expect(chart(view.host, "adc1").querySelector("polyline")).toBeNull();
    view.rerender(<PowerCard outputs={[]} readings={[updated[3], updated[0], { ...updated[1], value: 1_200_000 }, updated[2]]} onSet={onSet} />);
    expect(points(chart(view.host, "adc0"), "voltage")).toHaveLength(2);
    expect(points(chart(view.host, "adc1"), "voltage")).toHaveLength(2);
    expect(chart(view.host, "adc2").querySelector("polyline")).toBeNull();
    expect(chart(view.host, "adc3").querySelector("polyline")).toBeNull();
    const { modal } = expandChart(view.host, "adc0");
    expect(modal.textContent).toContain("adc0 · GPIO26_ADC0 · adc.voltage.chart.title");
    expect(modal.textContent).toContain("2.700 V");
    expect(modal.textContent).not.toContain("1.200 V");
    act(() => dialogButton(modal, "power.chart.close").click());
    view.rerender(<PowerCard outputs={[]} readings={[{ ...updated[0], signal: "replacement-sensor" }]} onSet={onSet} />);
    expect(chart(view.host, "adc0").getAttribute("aria-label")).toContain("replacement-sensor");
    expect(chart(view.host, "adc0").querySelector("polyline")).toBeNull();
    expect(onSet).not.toHaveBeenCalled();
  });

  it.each(["preview", "expanded"] as const)("uses a fixed 0–3.3 V domain for every %s ADC channel without rail scaling", (size) => {
    const view = render(<MeasurementSparkline mode="voltage" reading={voltageReading({ value: 0 })} />);
    view.rerender(<MeasurementSparkline mode="voltage" reading={voltageReading()} />);
    view.rerender(<MeasurementSparkline mode="voltage" reading={voltageReading({ value: 3_300_000 })} />);
    const svg = chart(size === "expanded" ? expandChart(view.host, "adc0").modal : view.host, "adc0");
    const height = size === "expanded" ? 280 : 58;
    const trace = points(svg, "voltage");
    expect(trace).toHaveLength(3);
    expect(trace[0][1]).toBe(height - 4);
    expect(trace[1][1]).toBeCloseTo(height - 4 - (2_608_696 / 3_300_000) * (height - 8));
    expect(trace[2][1]).toBe(4);
    expect(svg.querySelectorAll("g[data-metric]")).toHaveLength(1);
    expect(svg.querySelector('[data-reference="zero"]')).toBeNull();
  });

  it("splits ADC voltage history at a long sample gap instead of interpolating a voltage", () => {
    const view = render(<MeasurementSparkline mode="voltage" reading={voltageReading()} />);
    view.rerender(<MeasurementSparkline mode="voltage" reading={voltageReading({ value: 2_600_000 })} />);
    vi.setSystemTime(22_000);
    view.rerender(<MeasurementSparkline mode="voltage" reading={voltageReading({ value: 2_700_000 })} />);
    view.rerender(<MeasurementSparkline mode="voltage" reading={voltageReading({ value: 2_800_000 })} />);
    const segments = chart(view.host, "adc0").querySelectorAll('g[data-metric="voltage"] polyline');
    expect(segments).toHaveLength(2);
    for (const segment of segments) expect(segment.getAttribute("points")?.split(" ")).toHaveLength(2);
  });

  it("opens a rail-named three-trace dialog, traps focus, closes on Escape or button and restores the opener", () => {
    const view = render(<PowerCard outputs={[]} readings={[reading()]} onSet={vi.fn()} />);
    view.rerender(<PowerCard outputs={[]} readings={[reading({ value: 300_000 })]} onSet={vi.fn()} />);
    expect(document.querySelector('[role="dialog"]')).toBeNull();
    const originalOverflow = document.body.style.overflow;
    const parentEscape = vi.fn();
    window.addEventListener("keydown", parentEscape);
    closeViews.push(() => window.removeEventListener("keydown", parentEscape));

    const { modal, opener } = expandChart(view.host);
    expect(modal.getAttribute("aria-modal")).toBe("true");
    expect(document.getElementById(modal.getAttribute("aria-labelledby")!)?.textContent).toContain("5V");
    expect(chart(modal).getAttribute("viewBox")).toBe("0 0 960 280");
    expect(chart(modal).querySelectorAll("g[data-metric]")).toHaveLength(3);
    expect(chart(modal).querySelectorAll("polyline")).toHaveLength(3);
    expect(document.body.style.overflow).toBe("hidden");
    expect(view.host.inert).toBe(true);
    const close = dialogButton(modal, "power.chart.close");
    const viewport = modal.querySelector<HTMLElement>('[role="region"]')!;
    expect(document.activeElement).toBe(close);
    act(() => close.dispatchEvent(new KeyboardEvent("keydown", { key: "Tab", shiftKey: true, bubbles: true, cancelable: true })));
    expect(document.activeElement).toBe(viewport);
    act(() => viewport.dispatchEvent(new KeyboardEvent("keydown", { key: "Tab", bubbles: true, cancelable: true })));
    expect(document.activeElement).toBe(close);
    act(() => close.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true, cancelable: true })));
    expect(document.querySelector('[role="dialog"]')).toBeNull();
    expect(parentEscape).not.toHaveBeenCalled();
    expect(document.activeElement).toBe(opener);
    expect(view.host.getAttribute("aria-hidden")).toBeNull();
    expect(view.host.inert).not.toBe(true);
    expect(document.body.style.overflow).toBe(originalOverflow);
    const reopened = expandChart(view.host).modal;
    act(() => dialogButton(reopened, "power.chart.close").click());
    expect(document.querySelector('[role="dialog"]')).toBeNull();
    expect(localStorage.getItem("linkr-power-trends-expanded")).toBe("true");
  });

  it("zooms the time-axis content from 1x through 8x inside a native horizontal scroll viewport and resets pan", () => {
    const view = render(<MeasurementSparkline mode="power" reading={reading()} />);
    const { modal } = expandChart(view.host);
    const viewport = modal.querySelector<HTMLDivElement>('[role="region"]')!;
    const content = viewport.querySelector<HTMLDivElement>("[data-chart-content]")!;
    const zoomIn = dialogButton(modal, "power.chart.zoomIn");
    const zoomOut = dialogButton(modal, "power.chart.zoomOut");
    const reset = dialogButton(modal, "power.chart.zoomReset");
    expect(viewport.classList.contains("overflow-x-scroll")).toBe(true);
    expect(viewport.style.scrollbarWidth).toBe("auto");
    expect(viewport.tabIndex).toBe(0);
    expect(viewport.querySelector('input[type="range"]')).toBeNull();
    expect(content.style.width).toBe("100%");
    expect(zoomOut.disabled).toBe(true);
    expect(reset.disabled).toBe(true);
    for (const width of ["200%", "400%", "800%"]) {
      act(() => zoomIn.click());
      expect(content.style.width).toBe(width);
    }
    expect(zoomIn.disabled).toBe(true);
    act(() => zoomIn.click());
    expect(content.style.width).toBe("800%");
    act(() => zoomOut.click());
    expect(content.style.width).toBe("400%");
    viewport.scrollLeft = 280;
    act(() => reset.click());
    expect(content.style.width).toBe("100%");
    expect(viewport.scrollLeft).toBe(0);
    expect(zoomOut.disabled).toBe(true);
    expect(reset.disabled).toBe(true);
    expect(zoomIn.disabled).toBe(false);
  });

  it("keeps negative values, voltage/power gaps and real elapsed-time breaks in the live expanded view", () => {
    const sample = reading({ value: -200_000, voltage_uv: 4_800_000, power_enabled: false });
    const view = render(<MeasurementSparkline mode="power" reading={sample} />);
    const { modal } = expandChart(view.host);
    view.rerender(<MeasurementSparkline mode="power" reading={{ ...sample, voltage_valid: false }} />);
    expect(modal.querySelector('li[data-metric="voltage"]')?.textContent).toContain("—");
    expect(modal.querySelector('li[data-metric="power"]')?.textContent).toContain("—");
    view.rerender(<MeasurementSparkline mode="power" reading={{ ...sample, value: -300_000, voltage_uv: 4_900_000 }} />);
    expect(modal.querySelector('li[data-metric="current"]')?.textContent).toContain("-0.300 A");
    expect(modal.querySelector('li[data-metric="power"]')?.textContent).toContain("-1.47 W");
    expect(chart(modal).querySelector('g[data-metric="voltage"] polyline')).toBeNull();
    expect(chart(modal).querySelector('g[data-metric="power"] polyline')).toBeNull();
    const zero = Number(chart(modal).querySelector('[data-reference="zero"]')?.getAttribute("y1"));
    expect(points(chart(modal), "current").every(([, y]) => y > zero)).toBe(true);
    vi.setSystemTime(22_000);
    view.rerender(<MeasurementSparkline mode="power" reading={sample} />); // 23 s: gap
    view.rerender(<MeasurementSparkline mode="power" reading={{ ...sample, value: 200_000 }} />); // 24 s
    const currentSegments = chart(modal).querySelectorAll('g[data-metric="current"] polyline');
    expect(currentSegments).toHaveLength(2);
    const resumed = currentSegments[1].getAttribute("points")!.split(" ").map((point) => point.split(",").map(Number));
    expect(resumed[0][0]).toBeCloseTo(960 * 13 / 14);
    expect(resumed[0][1]).toBeGreaterThan(zero);
    expect(resumed[1][0]).toBe(960);
    expect(resumed[1][1]).toBeLessThan(zero);
  });

  it("retains one bounded full history per rail while rendering only the recent window outside the dialog", () => {
    const other = reading({ name: "12v_out", value: 600_000, voltage_uv: 12_000_000 });
    let current = reading();
    const onSet = vi.fn();
    const view = render(<PowerCard outputs={[]} readings={[current, other]} onSet={onSet} />);
    for (let index = 1; index <= MEASUREMENT_HISTORY_LIMIT + 5; index++) {
      current = reading({ value: index * 1000 });
      view.rerender(<PowerCard outputs={[]} readings={[current, other]} onSet={onSet} />);
    }
    expect(document.querySelector('[role="dialog"]')).toBeNull();
    expect(view.host.querySelectorAll('svg[role="img"]')).toHaveLength(2);
    expect(points(chart(view.host), "current")).toHaveLength(MEASUREMENT_PREVIEW_SAMPLES);
    const first = expandChart(view.host);
    expect(points(chart(first.modal), "current")).toHaveLength(MEASUREMENT_HISTORY_LIMIT);
    view.rerender(<PowerCard outputs={[]} readings={[other, current]} onSet={onSet} />);
    expect(points(chart(first.modal), "current")).toHaveLength(MEASUREMENT_HISTORY_LIMIT);
    expect(first.modal.querySelector('li[data-metric="current"]')?.textContent).toContain("0.605 A");
    act(() => dialogButton(first.modal, "power.chart.close").click());
    const second = expandChart(view.host, "12V");
    expect(chart(second.modal, "12V").querySelector("polyline")).toBeNull();
    expect(second.modal.querySelector('li[data-metric="current"]')?.textContent).toContain("0.600 A");
    expect(onSet).not.toHaveBeenCalled();
  });
});
