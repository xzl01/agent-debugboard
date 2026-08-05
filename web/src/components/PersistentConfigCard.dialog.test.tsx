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
  press,
  state,
  switchItem,
  type CardView,
} from "./PersistentConfigCard.testUtils";

let view: CardView | null = null;
afterEach(() => {
  view?.close();
  view = null;
  localStorage.clear();
  vi.clearAllMocks();
});

describe("PersistentConfigCard confirmations", () => {
  it("portals the viewport-bounded dialog outside the transformed card container", () => {
    view = mount(state({
      config: config([
        powerItem("firmware.danger", { selected: true, risk: "confirmation_required" }),
      ]),
    }));

    click(button(view.host, "Save selected"));

    const dialog = document.body.querySelector("dialog");
    const overlay = dialog?.parentElement;
    expect(overlay?.parentElement).toBe(document.body);
    expect(view.host.contains(overlay ?? null)).toBe(false);
    expect(dialog?.classList.contains("relative")).toBe(true);
    expect(dialog?.classList.contains("max-h-[calc(100dvh-3rem)]")).toBe(true);
    expect(dialog?.classList.contains("overflow-y-auto")).toBe(true);
  });

  it("traps focus, cancels dangerous save with Escape, and restores the trigger", async () => {
    const save = vi.fn().mockResolvedValue(undefined);
    view = mount(state({
      config: config([
        powerItem("firmware.danger.selected", { selected: true, risk: "confirmation_required" }),
        powerItem("firmware.danger.unselected", { risk: "confirmation_required" }),
      ]),
      save,
    }));
    await flush();
    const trigger = button(view.host, "Save selected");
    trigger.focus();
    click(trigger);
    const dialog = document.body.querySelector("dialog");
    expect(dialog?.textContent).toContain("firmware.danger.selected");
    expect(dialog?.textContent).not.toContain("firmware.danger.unselected");
    expect(save).not.toHaveBeenCalled();
    expect(document.activeElement).toBe(button(document.body, "Cancel"));
    press("Tab", true);
    expect(document.activeElement).toBe(button(document.body, "Save with confirmation"));
    press("Tab");
    expect(document.activeElement).toBe(button(document.body, "Cancel"));
    press("Escape");
    expect(document.body.querySelector("dialog")).toBeNull();
    expect(document.activeElement).toBe(trigger);
  });

  it("clears confirmed saved rows after a successful confirmed save even when authority still reports them selected", async () => {
    const savedConfig = config([
      powerItem("firmware.danger.saved", { selected: true, risk: "confirmation_required" }),
    ]);
    const save = vi.fn(async () => {
      view?.update(state({ config: savedConfig, save }));
    });
    view = mount(state({
      config: config([powerItem("firmware.danger.saved", { risk: "confirmation_required" })]),
      save,
    }));
    click(configRow(view.host, "firmware.danger.saved"));
    click(button(view.host, "Save selected"));
    expect(document.body.querySelector("dialog")?.textContent).toContain("firmware.danger.saved");
    click(button(document.body, "Save with confirmation"));
    await flush();
    expect(save).toHaveBeenCalledOnce();
    expect(save).toHaveBeenCalledWith(["firmware.danger.saved"], true);
    expect(view.host.textContent).toContain("Configuration saved and verified");
    const row = configRow(view.host, "firmware.danger.saved");
    expect(row.getAttribute("aria-checked")).toBe("false");
    expect(row.classList.contains("bg-brand/15")).toBe(false);
  });

  it("restores focus immediately but withholds success until confirmed GET authority resolves", async () => {
    const result = deferred<void>();
    const save = vi.fn().mockReturnValue(result.promise);
    view = mount(state({
      config: config([powerItem("firmware.danger", { selected: true, risk: "confirmation_required" })]),
      save,
    }));
    await flush();
    const trigger = button(view.host, "Save selected");
    click(trigger);
    const confirm = button(document.body, "Save with confirmation");
    click(confirm);
    click(confirm);
    expect(save).toHaveBeenCalledOnce();
    expect(save).toHaveBeenCalledWith(["firmware.danger"], true);
    expect(document.body.querySelector("dialog")).toBeNull();
    expect(document.activeElement).toBe(trigger);
    expect(view.host.textContent).not.toContain("Configuration saved and verified");
    await actResolve(result.resolve);
    expect(document.body.querySelector("dialog")).toBeNull();
    expect(view.host.textContent).toContain("Configuration saved and verified");
  });

  it("requires a separate clear confirmation that states current hardware is unchanged", async () => {
    const clear = vi.fn().mockResolvedValue(undefined);
    view = mount(state({ config: config([powerItem("firmware.saved")]), clear }));
    const trigger = button(view.host, "Clear saved");
    click(trigger);
    const dialog = document.body.querySelector("dialog");
    expect(dialog?.textContent).toContain("Current hardware is unchanged");
    expect(clear).not.toHaveBeenCalled();
    click(button(document.body, "Clear snapshot"));
    await flush();
    expect(clear).toHaveBeenCalledOnce();
    expect(document.activeElement).toBe(trigger);
  });

  it("opens a firmware-ID confirmation when the API returns a newer dangerous set", async () => {
    const error = new PersistentConfigApiError({
      kind: "confirmation_required",
      dangerousIds: ["firmware.server-danger"],
    }, "confirmation required");
    const save = vi.fn().mockRejectedValue(error);
    view = mount(state({
      config: config([powerItem("firmware.safe", { selected: true })]),
      save,
    }));
    await flush();
    click(button(view.host, "Save selected"));
    await flush();
    expect(document.body.querySelector("dialog")?.textContent).toContain("firmware.server-danger");
    expect(save).toHaveBeenCalledWith(["firmware.safe"], false);
  });
});

async function actResolve(resolve: (value: void) => void): Promise<void> {
  const { act } = await import("react");
  await act(async () => resolve());
}
