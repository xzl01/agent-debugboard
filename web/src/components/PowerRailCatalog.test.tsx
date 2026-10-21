import { act } from "react";
import { createRoot } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import { LanguageProvider } from "@/lib/i18n";
import { x12PowerFixture } from "@/lib/power.testFixtures";
import type { AdcReading, PowerOutput } from "@/lib/types";
import type { TestStep } from "@/lib/testScript";
import { PowerRailCatalogProvider } from "./PowerRailCatalog";
import { StepParams } from "./TestEditor";
import { WorkflowComposer } from "./WorkflowComposer";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
const close: (() => void)[] = [];
afterEach(() => { close.splice(0).forEach((fn) => fn()); localStorage.clear(); });

function mount(step: TestStep, outputs: PowerOutput[], readings: AdcReading[]) {
  localStorage.setItem("lang", "en");
  const host = document.createElement("div"); document.body.append(host); const root = createRoot(host);
  const onChange = vi.fn();
  const update = (nextOutputs = outputs, nextReadings = readings) => act(() => root.render(<LanguageProvider>
    <PowerRailCatalogProvider outputs={nextOutputs} readings={nextReadings}><StepParams step={step} onParamChange={onChange} /></PowerRailCatalogProvider>
  </LanguageProvider>));
  update(); close.push(() => { act(() => root.unmount()); host.remove(); });
  return { host, update, onChange };
}
const enabled = (select: HTMLSelectElement) => [...select.options].filter((option) => !option.disabled).map((option) => option.value);

describe("advertised workflow power choices", () => {
  it.each(["power_on", "power_off"] as const)("offers all five advertised controllable outputs for %s without manufacturing measurements", (type) => {
    const { outputs, readings } = x12PowerFixture();
    const view = mount({ id: "power", type, params: { rail: "5v_out" } }, outputs, readings);
    const select = view.host.querySelector("select")!;
    expect(enabled(select)).toEqual(outputs.map((output) => output.name));
    act(() => { select.value = "5v_usb_hub"; select.dispatchEvent(new Event("change", { bubbles: true })); });
    expect(view.onChange).toHaveBeenCalledExactlyOnceWith("rail", "5v_usb_hub");
    view.update([{ name: "future_aux", state: "off", value: 0, controllable: true }], []);
    expect(enabled(view.host.querySelector("select")!)).toEqual(["future_aux"]);
    expect(view.onChange).toHaveBeenCalledTimes(1); // No automatic draft retarget/write.
  });

  it.each(["adc_read", "capture"] as const)("only offers four current descriptors for %s, never the uninstrumented fifth output", (type) => {
    const { outputs, readings } = x12PowerFixture();
    const step: TestStep = type === "capture"
      ? { id: "capture", type, params: { rail: "5v_out", duration_ms: 1000 } }
      : { id: "adc", type, params: { channel: "5v_out" } };
    const view = mount(step, outputs, readings);
    const select = view.host.querySelector("select")!;
    expect(enabled(select)).toEqual(readings.filter((reading) => reading.kind === "current").map((reading) => reading.name));
    expect(select.querySelector('option[value="5v_usb_hub"]')).toBeNull();
    expect(enabled(select)).toHaveLength(4);
  });

  it("retains an absent draft as unavailable without inventing rail options or mutating it", () => {
    const view = mount({ id: "power", type: "power_on", params: { rail: "saved_external" } }, [], []);
    const select = view.host.querySelector("select")!;
    expect(select.disabled).toBe(true);
    expect(enabled(select)).toEqual([]);
    expect(select.textContent).toContain("not reported by the board");
    expect(view.onChange).not.toHaveBeenCalled();
  });

  it("reaches the nested workflow inspector through the same live catalog", () => {
    const { outputs, readings } = x12PowerFixture();
    const host = document.createElement("div"); document.body.append(host); const root = createRoot(host);
    close.push(() => { act(() => root.unmount()); host.remove(); });
    act(() => root.render(<LanguageProvider><PowerRailCatalogProvider outputs={outputs} readings={readings}>
      <WorkflowComposer script={{ schema: "linkr-test.v1", name: "fixture", version: "1.0", steps: [{ id: "p", type: "power_on", params: { rail: "5v_usb_hub" } }] }} onChange={vi.fn()} onRun={vi.fn()} />
    </PowerRailCatalogProvider></LanguageProvider>));
    const option = host.querySelector<HTMLOptionElement>('option[value="5v_usb_hub"]');
    expect(option).not.toBeNull();
    expect((option!.parentElement as HTMLSelectElement).value).toBe("5v_usb_hub");
    expect(option!.disabled).toBe(false);
  });
});
