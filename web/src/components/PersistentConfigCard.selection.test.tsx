import { act } from "react";
import { afterEach, describe, expect, it, vi } from "vitest";
import * as api from "@/lib/api";
import type { PersistentConfigSummary } from "@/lib/persistentConfig";
import {
  click,
  config,
  configRow,
  deferred,
  flush,
  mount,
  mountHooked,
  powerItem,
  state,
  type CardView,
} from "./PersistentConfigCard.testUtils";

vi.mock("@/lib/api", () => ({
  getPersistentConfig: vi.fn(),
  savePersistentConfig: vi.fn(),
  clearPersistentConfig: vi.fn(),
}));

const summary: PersistentConfigSummary = {
  available: true,
  reason: "ready",
  savedCount: 1,
  pendingCount: 0,
};

let view: CardView | null = null;
afterEach(() => {
  view?.close();
  view = null;
  localStorage.clear();
  vi.clearAllMocks();
});

describe("PersistentConfigCard selection", () => {
  it.each([
    { lang: "en" as const, name: "Select firmware.semantic" },
    { lang: "zh" as const, name: "选择 firmware.semantic" },
  ])("renders localized checkbox-role row buttons without native checkbox inputs in $lang", ({ lang, name }) => {
    view = mount(state({
      config: config([powerItem("firmware.semantic")]),
    }), { lang });

    const row = configRow(view.host, "firmware.semantic");
    expect(row.type).toBe("button");
    expect(row.getAttribute("aria-label")).toBe(name);
    expect(row.getAttribute("aria-checked")).toBe("false");
    expect(row.getAttribute("aria-disabled")).toBe("false");
    expect(row.classList.contains("w-full")).toBe(true);
    expect(row.classList.contains("focus-visible:ring-brand/40")).toBe(true);
    expect(view.host.querySelector('input[type="checkbox"]')).toBeNull();
  });

  it("marks selected rows with a token-only treatment distinct from panel and hover states", () => {
    view = mount(state({
      config: config([
        powerItem("firmware.selected", { selected: true }),
        powerItem("firmware.unselected"),
      ]),
    }));

    const selectedRow = configRow(view.host, "firmware.selected");
    expect(selectedRow.getAttribute("aria-checked")).toBe("true");
    expect(selectedRow.classList.contains("bg-brand/15")).toBe(true);
    expect(selectedRow.classList.contains("ring-1")).toBe(true);
    expect(selectedRow.classList.contains("ring-inset")).toBe(true);
    expect(selectedRow.classList.contains("ring-brand/30")).toBe(true);
    expect(selectedRow.classList.contains("hover:bg-brand/20")).toBe(true);
    expect(selectedRow.classList.contains("hover:bg-panel2/50")).toBe(false);

    const unselectedRow = configRow(view.host, "firmware.unselected");
    expect(unselectedRow.getAttribute("aria-checked")).toBe("false");
    expect(unselectedRow.classList.contains("bg-brand/15")).toBe(false);
    expect(unselectedRow.classList.contains("ring-1")).toBe(false);
    expect(unselectedRow.classList.contains("hover:bg-panel2/50")).toBe(true);
  });

  it("toggles an enabled row with a mouse click and retains the selection tint", () => {
    view = mount(state({ config: config([powerItem("firmware.mouse")]) }));

    const row = configRow(view.host, "firmware.mouse");
    click(row);

    expect(row.getAttribute("aria-checked")).toBe("true");
    expect(row.classList.contains("bg-brand/15")).toBe(true);
  });

  it.each([
    { key: " " as const, name: "Space" },
    { key: "Enter" as const, name: "Enter" },
  ])("toggles an enabled row with native $name activation", ({ key }) => {
    view = mount(state({ config: config([powerItem("firmware.keyboard")]) }));
    const row = configRow(view.host, "firmware.keyboard");

    activateNativeButton(row, key);

    expect(row.getAttribute("aria-checked")).toBe("true");
  });

  it("updates visible Current after one live invalidation without resetting a row draft", async () => {
    const updated = deferred<ReturnType<typeof config>>();
    const initialItem = { ...powerItem("firmware.live"), current: { kind: "power", state: "off" } as const };
    const updatedItem = { ...initialItem, current: { kind: "power", state: "on" } as const };
    vi.mocked(api.getPersistentConfig)
      .mockResolvedValueOnce(config([initialItem]))
      .mockReturnValueOnce(updated.promise);
    const hookedView = mountHooked({ connected: true, summary, currentStateKey: "controls-a" });
    try {
      await flush();
      click(configRow(hookedView.host, "firmware.live"));
      expect(configRow(hookedView.host, "firmware.live").getAttribute("aria-checked")).toBe("true");
      hookedView.update({ connected: true, summary, currentStateKey: "controls-b" });
      await flush();
      expect(api.getPersistentConfig).toHaveBeenCalledTimes(2);
      await act(async () => { updated.resolve(config([updatedItem])); });
      expect(configRow(hookedView.host, "firmware.live").textContent)
        .toContain("CurrentonSavedoff");
      expect(configRow(hookedView.host, "firmware.live").getAttribute("aria-checked")).toBe("true");
      expect(api.getPersistentConfig).toHaveBeenCalledTimes(2);
    } finally {
      hookedView.close();
    }
  });

  it("preserves row-control focus and blocks toggling during a loading authority refresh", () => {
    const fixture = state({
      config: config([powerItem("firmware.focus")]),
    });
    view = mount(fixture);
    const draftRow = configRow(view.host, "firmware.focus");
    click(draftRow);
    draftRow.focus();

    view.update({ ...fixture, loading: true });

    const loadingRow = configRow(view.host, "firmware.focus");
    expect(document.activeElement).toBe(loadingRow);
    expect(loadingRow.disabled).toBe(false);
    expect(loadingRow.getAttribute("aria-disabled")).toBe("true");
    click(loadingRow);
    expect(loadingRow.getAttribute("aria-checked")).toBe("true");
  });
});

function activateNativeButton(control: HTMLButtonElement, key: " " | "Enter"): void {
  control.focus();
  act(() => {
    control.dispatchEvent(new KeyboardEvent("keydown", { key, bubbles: true }));
    control.dispatchEvent(new KeyboardEvent("keyup", { key, bubbles: true }));
    control.click();
  });
}
