import { act, Profiler, type ComponentProps } from "react";
import { createRoot } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import { LanguageProvider } from "@/lib/i18n";
import { EMPTY_PD_STATUS } from "@/lib/pd";
import type { AdcReading, PdStatus } from "@/lib/types";
import { PowerCard } from "./PowerCard";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
const closeViews: (() => void)[] = [];
type ControlProps = Partial<Pick<ComponentProps<typeof PowerCard>, "disabled" | "stale" | "onSetPd">>;
const UNKNOWN_PD_WITH_CAPABILITIES: PdStatus = {
  ...EMPTY_PD_STATUS,
  available: true, pd: true, protocol: "pd", capabilities_valid: true,
  fixed_profiles: [
    { voltage_mv: 5000, max_current_ma: 3000 },
    { voltage_mv: 9000, max_current_ma: 3000 },
    { voltage_mv: 12000, max_current_ma: 2500 },
  ],
  pps_ranges: [{ min_voltage_mv: 3300, max_voltage_mv: 11000, step_mv: 100, max_current_ma: 3000 }],
};

function render(lang: "en" | "zh" = "en", pdOverrides: Partial<PdStatus> = {}, readings: readonly AdcReading[] = [
  { kind: "current", unit: "uA", name: "5v_out", signal: "INA226_5V", value: 300_000,
    power_enabled: false, voltage_valid: true, voltage_uv: 4_800_000 },
], controls: ControlProps = {}) {
  localStorage.setItem("lang", lang);
  const onSetPd = vi.fn(controls.onSetPd ?? (async () => {}));
  const host = document.createElement("div");
  document.body.append(host);
  const root = createRoot(host);
  const pd: PdStatus = {
    ...EMPTY_PD_STATUS,
    available: true, pd: true, mode: 1, voltage_mv: 5000, capabilities_valid: true,
    fixed_profiles: [{ voltage_mv: 5000, max_current_ma: 3000 }],
    pps_ranges: [{ min_voltage_mv: 3300, max_voltage_mv: 11000, step_mv: 100, max_current_ma: 3000 }],
    ...pdOverrides,
  };
  act(() => root.render(
    <LanguageProvider>
      <PowerCard
        {...controls}
        pd={pd} onSetPd={onSetPd} onSet={vi.fn()}
        outputs={["5v_out", "12v_out", "20v_out", "vdd_5v"].map((name) => ({ name, controllable: true, state: "off", value: 0 }))}
        readings={readings}
      />
    </LanguageProvider>,
  ));
  closeViews.push(() => act(() => { root.unmount(); host.remove(); }));
  return {
    host, onSetPd,
    input: host.querySelector<HTMLInputElement>('input[type="number"]')!,
    apply: [...host.querySelectorAll<HTMLButtonElement>("button")].find((button) => button.textContent === (lang === "en" ? "Apply" : "应用"))!,
  };
}

function typeValue(input: HTMLInputElement, value: string) {
  act(() => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(input, value);
    input.dispatchEvent(new Event("input", { bubbles: true }));
  });
}

function selectValue(select: HTMLSelectElement, value: string) {
  act(() => {
    select.value = value;
    select.dispatchEvent(new Event("change", { bubbles: true }));
  });
}

describe("PowerCard PD editing and real translated measurement copy", () => {
  afterEach(() => {
    closeViews.splice(0).forEach((close) => close());
    localStorage.clear();
  });

  it.each([0, 1] as const)("keeps mode %s unknown and disabled without capabilities until 20 V arrives, with no 5 V commit or startup write", (mode) => {
    localStorage.setItem("lang", "en");
    const host = document.createElement("div");
    document.body.append(host);
    const root = createRoot(host);
    closeViews.push(() => act(() => { root.unmount(); host.remove(); }));
    const onSetPd = vi.fn();
    const frames: { status: string; value: string; selection: string }[] = [];
    const renderPd = (pd: PdStatus) => <LanguageProvider><Profiler id="pd" onRender={() => {
      const editor = host.querySelector<HTMLInputElement | HTMLSelectElement>('[aria-label="Requested voltage"], [aria-label="Requested voltage (mV)"]');
      frames.push({ status: host.querySelector('[data-testid="pd-live-status"]')?.textContent ?? "", value: editor?.value ?? "",
        selection: editor instanceof HTMLSelectElement ? editor.selectedOptions[0]?.textContent ?? "" : "" });
    }}><PowerCard outputs={[]} readings={[]} pd={pd} onSet={vi.fn()} onSetPd={onSetPd} /></Profiler></LanguageProvider>;
    act(() => root.render(renderPd({ ...EMPTY_PD_STATUS, mode })));
    const initialEditor = host.querySelector<HTMLInputElement | HTMLSelectElement>('[aria-label="Requested voltage"], [aria-label="Requested voltage (mV)"]')!;
    expect(initialEditor.disabled).toBe(true);
    expect(host.querySelector<HTMLSelectElement>('[aria-label="PD mode"]')?.disabled).toBe(true);
    expect([...host.querySelectorAll<HTMLButtonElement>("button")].find((button) => button.textContent === "Apply")?.disabled).toBe(true);
    expect(initialEditor.value).toBe(mode === 0 ? "0" : "");
    expect(host.querySelector('[data-testid="pd-live-status"]')?.textContent).toBe("Voltage not reported yet");
    act(() => root.render(renderPd({ ...EMPTY_PD_STATUS, available: true, pd: true, protocol: "pd", mode, voltage_mv: 20000,
      capabilities_valid: true, fixed_profiles: [{ voltage_mv: 5000, max_current_ma: 3000 }, { voltage_mv: 20000, max_current_ma: 3000 }],
      pps_ranges: [{ min_voltage_mv: 3300, max_voltage_mv: 20000, step_mv: 100, max_current_ma: 3000 }],
    })));
    expect(host.querySelector('[data-testid="pd-live-status"]')?.textContent).toContain("requested 20.0 V");
    expect(host.querySelector<HTMLInputElement | HTMLSelectElement>('[aria-label="Requested voltage"], [aria-label="Requested voltage (mV)"]')?.value).toBe("20000");
    expect(frames[0].status).toBe("Voltage not reported yet");
    expect(frames.every((frame) => frame.value !== "5000" && !/requested 5\.0 V|^5 V/u.test(frame.status + frame.selection))).toBe(true);
    expect(onSetPd).not.toHaveBeenCalled();
  });

  it.each(["en", "zh"] as const)("allows an explicit fixed request from unknown voltage with valid source capabilities in %s", async (lang) => {
    const view = render(lang, UNKNOWN_PD_WITH_CAPABILITIES, [{
      kind: "voltage", unit: "uV", role: "pd_input", name: "adc0", signal: "GPIO26_ADC0", value: 11_660_000,
    }]);
    const mode = view.host.querySelector<HTMLSelectElement>(`[aria-label="${lang === "en" ? "PD mode" : "PD 模式"}"]`)!;
    const voltage = view.host.querySelector<HTMLSelectElement>(`[aria-label="${lang === "en" ? "Requested voltage" : "请求电压"}"]`)!;
    expect(view.host.textContent).toContain(lang === "en"
      ? "Source supports fixed profiles: 5V/3000mA, 9V/3000mA, 12V/2500mA · PPS: 3.3–11V/3000mA"
      : "电源支持固定档：5V/3000mA, 9V/3000mA, 12V/2500mA · PPS：3.3–11V/3000mA");
    const unknownHint = lang === "en"
      ? "Current requested voltage is unknown; select a voltage and apply it"
      : "当前请求电压未知；请选择档位后应用";
    expect(view.host.querySelector('[data-testid="pd-live-status"]')?.textContent).toBe(unknownHint);
    expect(view.host.querySelector('[data-testid="pd-input-measurement"]')?.textContent).toContain("11.660 V");
    expect(mode.disabled).toBe(false);
    expect(voltage.disabled).toBe(false);
    expect(voltage.value).toBe("0");
    expect(voltage.selectedOptions[0].disabled).toBe(true);
    expect(voltage.selectedOptions[0].textContent).toBe(lang === "en" ? "Current requested voltage is unknown" : "当前请求电压未知");
    expect(view.apply.disabled).toBe(true);
    act(() => view.apply.click());
    expect(view.onSetPd).not.toHaveBeenCalled();

    selectValue(voltage, "9000");
    expect(view.apply.disabled).toBe(false);
    expect(view.host.querySelector('[data-testid="pd-live-status"]')?.textContent).toBe(unknownHint);
    expect(view.onSetPd).not.toHaveBeenCalled();
    await act(async () => view.apply.click());
    expect(view.onSetPd).toHaveBeenCalledExactlyOnceWith(0, 9000);
  });

  it("allows switching from unknown voltage to a PPS draft without submitting it", async () => {
    const view = render("en", UNKNOWN_PD_WITH_CAPABILITIES);
    const mode = view.host.querySelector<HTMLSelectElement>('[aria-label="PD mode"]')!;
    expect(mode.disabled).toBe(false);
    expect(view.onSetPd).not.toHaveBeenCalled();
    selectValue(mode, "1");
    const input = view.host.querySelector<HTMLInputElement>('input[type="number"]')!;
    expect(input.disabled).toBe(false);
    expect(input.value).toBe("3300");
    expect(view.onSetPd).not.toHaveBeenCalled();
    typeValue(input, "9001");
    expect(view.apply.disabled).toBe(true);
    act(() => view.apply.click());
    expect(view.onSetPd).not.toHaveBeenCalled();
    typeValue(input, "9000");
    expect(view.apply.disabled).toBe(false);
    expect(view.onSetPd).not.toHaveBeenCalled();
    await act(async () => view.apply.click());
    expect(view.onSetPd).toHaveBeenCalledExactlyOnceWith(1, 9000);
  });

  it.each([0, 1] as const)("preserves unsupported mode %s until the user switches to the only supported mode", async (unsupportedMode) => {
    const supportedMode = unsupportedMode === 0 ? 1 : 0;
    const view = render("en", {
      ...UNKNOWN_PD_WITH_CAPABILITIES,
      mode: unsupportedMode,
      fixed_profiles: unsupportedMode === 0 ? [] : UNKNOWN_PD_WITH_CAPABILITIES.fixed_profiles,
      pps_ranges: unsupportedMode === 1 ? [] : UNKNOWN_PD_WITH_CAPABILITIES.pps_ranges,
    });
    const mode = view.host.querySelector<HTMLSelectElement>('[aria-label="PD mode"]')!;
    expect(mode.value).toBe(String(unsupportedMode));
    expect(mode.selectedOptions[0].disabled).toBe(true);
    expect(mode.querySelector<HTMLOptionElement>(`option[value="${supportedMode}"]`)?.disabled).toBe(false);
    expect(mode.disabled).toBe(false);
    const initialEditor = view.host.querySelector<HTMLInputElement | HTMLSelectElement>(unsupportedMode === 0
      ? 'select[aria-label="Requested voltage"]' : 'input[aria-label="Requested voltage (mV)"]')!;
    expect(initialEditor.disabled).toBe(true);
    expect(view.apply.disabled).toBe(true);
    expect(view.onSetPd).not.toHaveBeenCalled();

    selectValue(mode, String(supportedMode));
    expect(mode.value).toBe(String(supportedMode));
    const editor = view.host.querySelector<HTMLInputElement | HTMLSelectElement>(supportedMode === 0
      ? 'select[aria-label="Requested voltage"]' : 'input[aria-label="Requested voltage (mV)"]')!;
    expect(editor.disabled).toBe(false);
    expect(editor.value).toBe(supportedMode === 0 ? "0" : "3300");
    expect(view.onSetPd).not.toHaveBeenCalled();
    if (editor instanceof HTMLSelectElement) selectValue(editor, "9000");
    else typeValue(editor, "9000");
    expect(view.apply.disabled).toBe(false);
    expect(view.onSetPd).not.toHaveBeenCalled();
    await act(async () => view.apply.click());
    expect(view.onSetPd).toHaveBeenCalledExactlyOnceWith(supportedMode, 9000);
  });

  it.each([
    ["invalid capabilities", { capabilities_valid: false }, {}],
    ["empty capabilities", { fixed_profiles: [], pps_ranges: [] }, {}],
    ["unavailable PD", { available: false }, {}],
    ["non-PD source", { pd: false }, {}],
    ["PD error", { last_error: -5 }, {}],
    ["stale data", {}, { stale: true }],
    ["external disable", {}, { disabled: true }],
  ] as const)("keeps both PD modes read-only with %s even when a voltage is known", (_reason, pdOverrides, controls) => {
    for (const mode of [0, 1] as const) {
      const view = render("en", { ...UNKNOWN_PD_WITH_CAPABILITIES, mode, voltage_mv: 9000, ...pdOverrides }, [], controls);
      expect(view.host.querySelector<HTMLSelectElement>('[aria-label="PD mode"]')?.disabled).toBe(true);
      expect(view.host.querySelector<HTMLInputElement | HTMLSelectElement>('[aria-label="Requested voltage"], [aria-label="Requested voltage (mV)"]')?.disabled).toBe(true);
      expect(view.apply.disabled).toBe(true);
      act(() => view.apply.click());
      expect(view.onSetPd).not.toHaveBeenCalled();
    }
  });

  it.each([0, 1] as const)("disables mode %s controls while an explicit request is pending", async (mode) => {
    let finish!: () => void;
    const pending = new Promise<void>((resolve) => { finish = resolve; });
    const view = render("en", { ...UNKNOWN_PD_WITH_CAPABILITIES, mode }, [], { onSetPd: () => pending });
    const editor = view.host.querySelector<HTMLInputElement | HTMLSelectElement>('[aria-label="Requested voltage"], [aria-label="Requested voltage (mV)"]')!;
    if (editor instanceof HTMLSelectElement) selectValue(editor, "9000");
    else typeValue(editor, "9000");
    await act(async () => view.apply.click());
    expect(view.host.querySelector<HTMLSelectElement>('[aria-label="PD mode"]')?.disabled).toBe(true);
    expect(editor.disabled).toBe(true);
    expect(view.apply.disabled).toBe(true);
    act(() => view.apply.click());
    expect(view.onSetPd).toHaveBeenCalledExactlyOnceWith(mode, 9000);
    await act(async () => finish());
    expect(editor.disabled).toBe(false);
  });

  it("keeps each PPS draft keystroke and submits a valid millivolt value", async () => {
    const view = render();
    expect(view.input.getAttribute("aria-label")).toBe("Requested voltage (mV)");
    expect(view.input.parentElement?.textContent).toContain("mV");
    for (const value of ["", "9", "90", "900", "9000"]) {
      typeValue(view.input, value);
      expect(view.input.value).toBe(value);
      expect(view.apply.disabled).toBe(value !== "9000");
    }
    await act(async () => view.apply.click());
    expect(view.onSetPd).toHaveBeenCalledExactlyOnceWith(1, 9000);
  });

  it.each(["en", "zh"] as const)("provides translated expanded-chart controls and a rail-named dialog in %s", (lang) => {
    const view = render(lang);
    const previewVoltage = view.host.querySelector('[data-measurement-view="preview"] li[data-metric="voltage"]')!;
    expect(previewVoltage.textContent).toBe(lang === "en" ? "Voltage (V)4.800 V" : "电压 (V)4.800 V");
    expect(previewVoltage.querySelector(".whitespace-nowrap")).not.toBeNull();
    const enlarge = view.host.querySelector<HTMLButtonElement>('button[aria-haspopup="dialog"]')!;
    expect(enlarge.getAttribute("aria-label")).toBe(lang === "en" ? "5V · Expand chart" : "5V · 放大查看");
    act(() => { enlarge.focus(); enlarge.click(); });
    const modal = document.querySelector<HTMLElement>('[role="dialog"]')!;
    expect(modal.querySelector('li[data-metric="voltage"]')?.textContent).toContain(lang === "en" ? "Measured voltage (V)4.800 V" : "实测电压 (V)4.800 V");
    const colors = { current: "brand", voltage: "ok", power: "warn" };
    for (const metric of ["current", "voltage", "power"] as const) {
      const swatch = modal.querySelector<HTMLElement>(`li[data-metric="${metric}"] span[aria-hidden="true"]`)!;
      expect(swatch.style.color).toBe(`rgb(var(--c-${colors[metric]}))`);
      expect(swatch.style.borderTopColor).toBe(`rgb(var(--c-${colors[metric]}))`);
    }
    expect(modal.textContent).toContain(lang === "en" ? "5V · Power trend" : "5V · 电源趋势");
    expect(modal.textContent).toContain("600");
    expect(modal.textContent).toContain(lang === "en" ? "horizontal scrollbar" : "水平滚动条");
    expect(modal.querySelector('[aria-label="' + (lang === "en" ? "Zoom in on time axis" : "放大时间轴") + '"]')).not.toBeNull();
    const close = modal.querySelector<HTMLButtonElement>('[aria-label="' + (lang === "en" ? "Close expanded chart" : "关闭放大图表") + '"]')!;
    expect(close).not.toBeNull();
    act(() => close.click());
    expect(document.activeElement).toBe(enlarge);
  });

  it.each(["en", "zh"] as const)("merges source-side ADC0 into PD while keeping pad diagnostics distinct in %s", (lang) => {
    const view = render(lang, {}, [{
      kind: "voltage", unit: "uV", role: "pd_input", name: "adc0", signal: "GPIO26_ADC0", value: 20_000_000,
      raw: 3236, mv: 2609,
    }]);
    expect(view.host.querySelector('[data-testid="pd-input-card"]')?.textContent).toContain("20.000 V");
    expect(view.host.querySelector('[data-testid="pd-input-card"] details')?.textContent).toContain("2609 mV");
    expect(view.host.querySelector('[data-native-adc="adc0"]')).toBeNull();
    const enlarge = view.host.querySelector<HTMLButtonElement>('button[aria-haspopup="dialog"]')!;
    expect(enlarge.getAttribute("aria-label")).toContain("adc0 · GPIO26_ADC0");
    act(() => enlarge.click());
    expect(document.querySelector('[role="dialog"]')?.textContent).toContain(
      lang === "en" ? "adc0 · GPIO26_ADC0 · PD input voltage trend" : "adc0 · GPIO26_ADC0 · PD 输入电压趋势",
    );
  });

  it.each(["", "3200", "11100", "9001", "9000.5"])("rejects invalid PPS draft %j without replacing it", (value) => {
    const view = render();
    typeValue(view.input, value);
    act(() => view.input.dispatchEvent(new FocusEvent("focusout", { bubbles: true })));
    expect(view.input.value).toBe(value);
    expect(view.input.getAttribute("aria-invalid")).toBe("true");
    expect(view.apply.disabled).toBe(true);
    expect(view.host.querySelector('[role="alert"]')?.textContent).toBe(
      "Enter a whole number of mV within a source PPS range and at its supported step.",
    );
    view.apply.click();
    expect(view.onSetPd).not.toHaveBeenCalled();
    typeValue(view.input, "9000");
    expect(view.host.querySelector('[role="alert"]')).toBeNull();
    expect(view.apply.disabled).toBe(false);
  });

  it("validates against individual source ranges rather than the envelope between them", () => {
    const view = render("en", { pps_ranges: [
      { min_voltage_mv: 3300, max_voltage_mv: 5900, step_mv: 100, max_current_ma: 3000 },
      { min_voltage_mv: 9000, max_voltage_mv: 11000, step_mv: 200, max_current_ma: 2000 },
    ] });
    for (const value of ["6000", "9100"]) {
      typeValue(view.input, value);
      expect(view.apply.disabled).toBe(true);
    }
    typeValue(view.input, "9200");
    expect(view.apply.disabled).toBe(false);
  });

  it.each([
    ["en", "rail switch off — showing actual measurements", "5V power", "Each line uses its own scale"],
    ["zh", "电源开关已关闭 — 仍显示实际测量值", "5V 电源", "各曲线使用独立量程"],
  ] as const)("shows honest %s copy and names the rail switch while actual load remains nonzero", (lang, copy, label, scales) => {
    const view = render(lang);
    expect(view.host.textContent).toContain(copy);
    expect(view.host.textContent).toContain("0.300 A");
    expect(view.host.textContent).toContain("4.800 V");
    expect(view.host.textContent).toContain("1.44 W");
    expect(view.host.textContent).toContain(scales);
    expect(view.host.querySelector('[role="switch"]')?.getAttribute("aria-label")).toBe(label);
    expect([...view.host.querySelectorAll('[role="switch"]')].map((item) => item.getAttribute("aria-label"))).toEqual(
      ["5V", "12V", "20V", "vdd_5v"].map((rail) => `${rail} ${lang === "en" ? "power" : "电源"}`),
    );
    expect(view.host.textContent).not.toMatch(/showing zero|显示为 0/);
    typeValue(view.input, "9001");
    act(() => view.input.dispatchEvent(new FocusEvent("focusout", { bubbles: true })));
    expect(view.host.querySelector('[role="alert"]')?.textContent).toBe(lang === "en"
      ? "Enter a whole number of mV within a source PPS range and at its supported step."
      : "请输入电源 PPS 范围内且符合步进的整数毫伏值。");
  });
});
