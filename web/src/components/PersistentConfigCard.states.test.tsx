import { afterEach, describe, expect, it } from "vitest";
import { PersistentConfigApiError } from "@/lib/persistentConfig";
import {
  button,
  config,
  gpioItem,
  mount,
  powerItem,
  state,
  switchItem,
  type CardView,
} from "./PersistentConfigCard.testUtils";

let view: CardView | null = null;
afterEach(() => {
  view?.close();
  view = null;
  localStorage.clear();
});

describe("PersistentConfigCard states", () => {
  it("groups only firmware-enumerated supported rows and exposes values, risk, and apply state", () => {
    const unknown = {
      id: "future.item",
      kind: "unknown" as const,
      sourceKind: "future",
      current: null,
      saved: null,
      selected: false,
      risk: "unknown" as const,
      applyState: "unknown" as const,
    };
    view = mount(state({
      config: config([
        powerItem("firmware.power.alpha"),
        switchItem("firmware.switch.beta", "confirmation_required", "pending"),
        gpioItem("firmware.gpio.gamma"),
        unknown,
      ], { pending: 1 }),
    }));

    expect(view.host.textContent).toContain("Power outputs");
    expect(view.host.textContent).toContain("Routing switches");
    expect(view.host.textContent).toContain("Safe GPIO");
    expect(view.host.textContent).toContain("firmware.power.alpha");
    expect(view.host.textContent).toContain("firmware-current");
    expect(view.host.textContent).toContain("firmware-saved");
    expect(view.host.textContent).toContain("Confirmation required");
    expect(view.host.textContent).toContain("Pending");
    expect(view.host.textContent).not.toContain("future.item");
  });

  it.each([
    ["initial loading", state({ config: null, loading: true }), true, "Loading saved configuration"],
    ["old firmware", state({ config: null, supported: false }), true, "Unsupported firmware"],
    ["unavailable storage", state({ config: config([], { available: false }) }), true, "Saved configuration is unavailable"],
    ["disconnected", state({ config: config([powerItem("firmware.last-good")]) }), false, "Disconnected"],
    ["reconnecting", state({ config: config([powerItem("firmware.last-good")]), loading: true }), true, "Refreshing saved configuration"],
  ])("renders the %s state with non-color status text", (_name, fixture, connected, expected) => {
    view = mount(fixture, { connected });
    expect(view.host.textContent).toContain(expected);
  });

  it("renders storage errors with a retry action", () => {
    const error = new PersistentConfigApiError(
      { kind: "other", code: "storage_error" },
      "fixture storage detail"
    );
    view = mount(state({ config: null, error }));
    expect(view.host.textContent).toContain("Storage error");
    expect(view.host.textContent).toContain("fixture storage detail");
    expect(button(view.host, "Refresh").disabled).toBe(false);
  });

  it("renders busy and partial apply details without relying on color", () => {
    const busy = new PersistentConfigApiError({ kind: "busy", activity: "capture" }, "busy");
    view = mount(state({ config: config([powerItem("firmware.row")]), error: busy }));
    expect(view.host.textContent).toContain("Power capture is using the board");

    const partial = new PersistentConfigApiError({
      kind: "apply_failed",
      appliedIds: ["firmware.applied"],
      failedId: "firmware.failed",
      pendingIds: ["firmware.failed", "firmware.pending"],
    }, "partial");
    view.update(state({
      config: config([
        powerItem("firmware.applied", { applyState: "pending" }),
        powerItem("firmware.failed", { applyState: "pending" }),
        powerItem("firmware.pending", { applyState: "pending" }),
      ], { pending: 3 }),
      error: partial,
    }));
    expect(view.host.textContent).toContain("Partial apply stopped at firmware.failed");
    expect(view.host.textContent).toContain("Applied: firmware.applied");
    expect(view.host.textContent).toContain("Still pending: firmware.failed, firmware.pending");
    expect(view.host.textContent).toContain("Failed");
  });

  it("shows empty selection and pending guidance with disabled or enabled actions", () => {
    view = mount(state({
      config: config([powerItem("firmware.pending", { applyState: "pending" })], { pending: 1 }),
    }));
    expect(view.host.textContent).toContain("Select at least one item to save");
    expect(button(view.host, "Save selected").getAttribute("aria-disabled")).toBe("true");
    expect(button(view.host, "Apply pending").disabled).toBe(false);
    expect(view.host.textContent).toContain("1 pending item will be applied");
  });

  it("renders Chinese controls and updates the document language", () => {
    view = mount(state({ config: config([powerItem("firmware.locale")]) }), { lang: "zh" });
    expect(view.host.textContent).toContain("已保存配置");
    expect(view.host.textContent).toContain("保存所选项");
    expect(document.documentElement.lang).toBe("zh-CN");
  });
});
