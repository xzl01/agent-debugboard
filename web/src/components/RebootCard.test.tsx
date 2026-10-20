import { act } from "react";
import { createRoot } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import type { AutomationTaskOwner } from "@/lib/automationTask";
import { LanguageProvider } from "@/lib/i18n";
import { BootCard } from "./BootCard";
import { RebootCard } from "./RebootCard";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
const cleanups: (() => void)[] = [];
type Options = { connected: boolean; restarting: boolean; restartTimedOut: boolean; disabled: boolean; owner: AutomationTaskOwner | null };

function render(lang: "en" | "zh" = "en", overrides: Partial<Options> = {}) {
  localStorage.setItem("lang", lang);
  const host = document.createElement("div");
  document.body.append(host);
  const root = createRoot(host);
  const onReboot = vi.fn<() => Promise<void>>().mockResolvedValue(undefined);
  const onBoot = vi.fn(async () => {});
  let options: Options = { connected: true, restarting: false, restartTimedOut: false, disabled: false, owner: null, ...overrides };
  const update = (next: Partial<Options> = {}) => {
    options = { ...options, ...next };
    act(() => root.render(<LanguageProvider>
      <RebootCard {...options} taskControl={{ owner: options.owner, acquire: vi.fn(() => true), release: vi.fn() }} onReboot={onReboot} />
      <BootCard onBoot={onBoot} />
    </LanguageProvider>));
  };
  update();
  cleanups.push(() => act(() => { root.unmount(); host.remove(); }));
  return { host, update, onReboot, onBoot, button: () => host.querySelector<HTMLButtonElement>("button")! };
}

describe("normal restart UI is separate from ROM BOOTSEL", () => {
  afterEach(() => { cleanups.splice(0).forEach((cleanup) => cleanup()); localStorage.clear(); vi.restoreAllMocks(); });

  it.each(["en", "zh"] as const)("confirms a normal cold restart in %s, with cancellation sending no request", async (lang) => {
    const view = render(lang);
    const confirm = vi.spyOn(window, "confirm").mockReturnValueOnce(false).mockReturnValueOnce(true);
    expect(view.host.textContent).toContain(lang === "en" ? "Normal cold restart" : "普通冷重启");
    expect(view.host.textContent).toContain(lang === "en" ? "does not enter ROM BOOTSEL" : "不会进入 ROM BOOTSEL");
    expect(view.host.textContent).toContain("RPI-RP2");
    act(() => view.button().click());
    expect(view.onReboot).not.toHaveBeenCalled();
    expect(view.onBoot).not.toHaveBeenCalled();
    await act(async () => view.button().click());
    expect(confirm.mock.calls[0]?.[0]).toContain(lang === "en" ? "cold restart" : "普通冷重启");
    expect(confirm.mock.calls[0]?.[0]).toContain(lang === "en" ? "NOT enter ROM BOOTSEL" : "不会进入 ROM BOOTSEL");
    expect(view.onReboot).toHaveBeenCalledTimes(1);
    expect(view.onBoot).not.toHaveBeenCalled();
    view.update({ restarting: true, connected: false });
    expect(view.host.textContent).toContain(lang === "en" ? "Waiting for the debugger to disconnect and reconnect" : "正在等待调试板断开并重新连接");
    view.update({ restarting: false, connected: true, restartTimedOut: true });
    expect(view.host.textContent).toContain(lang === "en" ? "Reconnection was not observed within 30 seconds" : "30 秒内未观察到重新连接");
  });

  it("keeps the existing BOOTSEL confirmation and callback independent", async () => {
    const view = render();
    const confirm = vi.spyOn(window, "confirm").mockReturnValue(true);
    const bootButton = [...view.host.querySelectorAll<HTMLButtonElement>("button")].find((button) => button.textContent === "Enter BOOTSEL")!;
    await act(async () => bootButton.click());
    expect(confirm).toHaveBeenCalledWith("Enter RP2350 BOOTSEL? The board will reboot into ROM bootloader in ~250 ms.");
    expect(view.onBoot).toHaveBeenCalledTimes(1);
    expect(view.onReboot).not.toHaveBeenCalled();
  });

  it.each([
    { connected: false }, { disabled: true }, { restarting: true }, { owner: "test" as const }, { owner: "boot" as const }, { owner: "reboot" as const },
  ])("disables restart without even confirming when blocked: %j", (options) => {
    const view = render("en", options);
    const confirm = vi.spyOn(window, "confirm").mockReturnValue(true);
    expect(view.button().disabled).toBe(true);
    act(() => view.button().click());
    expect(confirm).not.toHaveBeenCalled();
    expect(view.onReboot).not.toHaveBeenCalled();
  });

  it("prevents duplicate requests during the POST and displays the expected restart outage without an error", async () => {
    const view = render();
    const confirm = vi.spyOn(window, "confirm").mockReturnValue(true);
    let accept!: () => void;
    view.onReboot.mockImplementation(() => new Promise<void>((resolve) => { accept = resolve; }));
    act(() => { view.button().click(); view.button().click(); });
    expect(view.onReboot).toHaveBeenCalledTimes(1);
    expect(confirm).toHaveBeenCalledTimes(1);
    expect(view.button().disabled).toBe(true);
    view.update({ restarting: true, connected: false });
    await act(async () => accept());
    expect(view.host.textContent).toContain("Restarting debugger…");
    expect(view.host.textContent).toContain("Waiting for the debugger to disconnect and reconnect");
    expect(view.host.querySelector('[role="alert"]')).toBeNull();
    expect(view.button().disabled).toBe(true);
    view.update({ restarting: false, connected: true, restartTimedOut: true });
    expect(view.button().disabled).toBe(false);
    expect(view.host.textContent).toContain("Reconnection was not observed within 30 seconds");
  });

  it("reports request failure and permits another confirmed attempt", async () => {
    const view = render();
    vi.spyOn(window, "confirm").mockReturnValue(true);
    view.onReboot.mockRejectedValueOnce(Object.assign(new Error("timeout"), { code: "reboot_request_timeout" }));
    await act(async () => view.button().click());
    expect(view.host.querySelector('[role="alert"]')?.textContent).toContain("The restart request timed out");
    expect(view.button().disabled).toBe(false);
    await act(async () => view.button().click());
    expect(view.onReboot).toHaveBeenCalledTimes(2);
    expect(view.host.querySelector('[role="alert"]')).toBeNull();
  });
});
