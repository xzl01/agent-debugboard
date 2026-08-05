import { afterEach, describe, expect, it, vi } from "vitest";
import { PersistentConfigApiError } from "@/lib/persistentConfig";
import {
  button,
  click,
  config,
  configRow,
  deferred,
  flush,
  mount,
  powerItem,
  state,
  type CardView,
} from "./PersistentConfigCard.testUtils";

let view: CardView | null = null;
afterEach(() => {
  view?.close();
  view = null;
  localStorage.clear();
  vi.clearAllMocks();
});

describe("PersistentConfigCard actions", () => {
  it("saves exactly the row-selected safe firmware IDs without confirmation", async () => {
    const save = vi.fn().mockResolvedValue(undefined);
    view = mount(state({
      config: config([powerItem("firmware.safe.one"), powerItem("firmware.safe.two")]),
      save,
    }));
    click(configRow(view.host, "firmware.safe.two"));
    click(button(view.host, "Save selected"));
    await flush();
    expect(save).toHaveBeenCalledOnce();
    expect(save).toHaveBeenCalledWith(["firmware.safe.two"], false);
  });

  it("clears saved rows after a successful unconfirmed save even when authority still reports them selected", async () => {
    const savedConfig = config([
      powerItem("firmware.saved.one", { selected: true }),
      powerItem("firmware.saved.two", { selected: true }),
      powerItem("firmware.unrelated", { selected: true }),
    ]);
    const save = vi.fn(async () => {
      view?.update(state({ config: savedConfig, save }));
    });
    view = mount(state({
      config: config([
        powerItem("firmware.saved.one"),
        powerItem("firmware.saved.two"),
        powerItem("firmware.unrelated", { selected: true }),
      ]),
      save,
    }));
    click(configRow(view.host, "firmware.saved.one"));
    click(configRow(view.host, "firmware.saved.two"));
    click(configRow(view.host, "firmware.unrelated"));
    click(button(view.host, "Save selected"));
    await flush();
    expect(save).toHaveBeenCalledOnce();
    expect(save).toHaveBeenCalledWith(["firmware.saved.one", "firmware.saved.two"], false);
    expect(view.host.textContent).toContain("Configuration saved and verified");
    for (const id of ["firmware.saved.one", "firmware.saved.two"]) {
      expect(configRow(view.host, id).getAttribute("aria-checked")).toBe("false");
      expect(configRow(view.host, id).classList.contains("bg-brand/15")).toBe(false);
    }
    expect(configRow(view.host, "firmware.unrelated").getAttribute("aria-checked")).toBe("false");
  });

  it("preserves row selection when save rejects with a known API error", async () => {
    const error = new PersistentConfigApiError({ kind: "other", code: "save_failed" }, "save failed");
    const save = vi.fn().mockRejectedValue(error);
    view = mount(state({
      config: config([powerItem("firmware.rejected")]),
      save,
    }));
    click(configRow(view.host, "firmware.rejected"));
    click(button(view.host, "Save selected"));
    await flush();
    expect(save).toHaveBeenCalledOnce();
    expect(save).toHaveBeenCalledWith(["firmware.rejected"], false);
    expect(view.host.textContent).not.toContain("Configuration saved and verified");
    const row = configRow(view.host, "firmware.rejected");
    expect(row.getAttribute("aria-checked")).toBe("true");
    expect(row.classList.contains("bg-brand/15")).toBe(true);
  });

  it("does not show save success until the hook mutation and authoritative GET resolve", async () => {
    const result = deferred<void>();
    const save = vi.fn().mockReturnValue(result.promise);
    view = mount(state({
      config: config([powerItem("firmware.safe", { selected: true })]),
      save,
    }));
    await flush();
    click(button(view.host, "Save selected"));
    expect(view.host.textContent).not.toContain("Configuration saved and verified");
    await actResolve(result.resolve);
    expect(view.host.textContent).toContain("Configuration saved and verified");
  });

  it("keeps success visible when the authoritative GET rerenders the config before resolving", async () => {
    const nextConfig = config([powerItem("firmware.safe", { selected: true, applyState: "applied" })]);
    const save = vi.fn(async () => {
      view?.update(state({ config: nextConfig, save }));
    });
    view = mount(state({
      config: config([powerItem("firmware.safe", { selected: true })]),
      save,
    }));
    await flush();
    click(button(view.host, "Save selected"));
    await flush();
    expect(view.host.textContent).toContain("Configuration saved and verified");
  });

  it("keeps firmware and draft selections until save resolves, then clears both", async () => {
    const result = deferred<void>();
    const save = vi.fn().mockReturnValue(result.promise);
    view = mount(state({
      config: config([
        powerItem("firmware.selected", { selected: true }),
        powerItem("draft.selected"),
      ]),
      save,
    }));
    click(configRow(view.host, "draft.selected"));
    click(button(view.host, "Save selected"));
    expect(save).toHaveBeenCalledOnce();
    expect(save).toHaveBeenCalledWith(["firmware.selected", "draft.selected"], false);
    for (const id of ["firmware.selected", "draft.selected"]) {
      expect(configRow(view.host, id).getAttribute("aria-checked")).toBe("true");
      expect(configRow(view.host, id).classList.contains("bg-brand/15")).toBe(true);
    }

    await actResolve(result.resolve);

    for (const id of ["firmware.selected", "draft.selected"]) {
      expect(configRow(view.host, id).getAttribute("aria-checked")).toBe("false");
      expect(configRow(view.host, id).classList.contains("bg-brand/15")).toBe(false);
    }
  });

  it("preserves a selected failed-only row when save rejects with a known API error", async () => {
    const error = new PersistentConfigApiError({
      kind: "apply_failed",
      appliedIds: [],
      failedId: "firmware.failed.safe",
      pendingIds: ["firmware.failed.safe"],
    }, "save failed");
    const save = vi.fn().mockRejectedValue(error);
    view = mount(state({
      config: config([
        powerItem("firmware.failed.safe", { selected: true, applyState: "failed" }),
      ], { pending: 0 }),
      save,
    }));
    const saveButton = button(view.host, "Save selected");
    expect(saveButton.getAttribute("aria-disabled")).toBe("false");
    click(saveButton);
    await flush();
    expect(save).toHaveBeenCalledOnce();
    expect(save).toHaveBeenCalledWith(["firmware.failed.safe"], false);
    const row = configRow(view.host, "firmware.failed.safe");
    expect(row.getAttribute("aria-checked")).toBe("true");
    expect(row.classList.contains("bg-brand/15")).toBe(true);
  });

  it("refreshes through the strict hook and reports completion only afterward", async () => {
    const result = deferred<void>();
    const refresh = vi.fn().mockReturnValue(result.promise);
    view = mount(state({ config: config([powerItem("firmware.row")]), refresh }));
    click(button(view.host, "Refresh"));
    expect(view.host.textContent).not.toContain("Configuration refreshed");
    await actResolve(result.resolve);
    expect(refresh).toHaveBeenCalledOnce();
    expect(view.host.textContent).toContain("Configuration refreshed");
  });

  it("disables all actions and row selection while a mutation is busy", () => {
    view = mount(state({
      config: config([powerItem("firmware.busy", { selected: true })], { pending: 1 }),
      busy: "save",
    }));
    const busyRow = configRow(view.host, "firmware.busy");
    expect(busyRow.disabled).toBe(false);
    expect(busyRow.getAttribute("aria-disabled")).toBe("true");
    click(busyRow);
    expect(busyRow.getAttribute("aria-checked")).toBe("true");
    for (const name of ["Saving…", "Clear saved"]) {
      expect(button(view.host, name).getAttribute("aria-disabled")).toBe("true");
    }
    expect(button(view.host, "Refresh").disabled).toBe(true);
  });
});

async function actResolve(resolve: (value: void) => void): Promise<void> {
  const { act } = await import("react");
  await act(async () => resolve());
}
