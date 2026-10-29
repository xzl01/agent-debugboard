import { act, type ComponentProps } from "react";
import { createRoot } from "react-dom/client";
import { flushSync } from "react-dom";
import { afterEach, describe, expect, it, vi } from "vitest";
import { EMPTY_BOARD_SNAPSHOT } from "@/lib/boardSnapshot";
import { LanguageProvider } from "@/lib/i18n";
import { ThemeProvider } from "@/lib/theme";
import { StatusBar } from "./StatusBar";
import { useVdd5vAction } from "./useVdd5vAction";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const snapshot = (state = "off", controllable = true) => ({
  ...EMPTY_BOARD_SNAPSHOT,
  powerOutputs: [
    { name: "vdd_5v", state, controllable, value: state === "on" ? 1 : 0 },
    { name: "5v_out", state: "on", controllable: true, value: 1 },
  ],
});

const cleanup: Array<() => void> = [];

// Exercise the same action owner the App shares with the drawer, rather than
// retaining a second header-only confirmation implementation in the fixture.
function ControlledHeader({ props }: { props: ComponentProps<typeof StatusBar> }) {
  const output = props.snapshot.powerOutputs.find((item) => item.name === "vdd_5v");
  const action = useVdd5vAction({
    available: props.connected && !props.loading && !props.powerStale && output?.controllable === true,
    state: output?.state === "on" || output?.state === "off" ? output.state : null,
    updatedAt: props.powerUpdatedAt ?? null,
    isLocked: () => props.powerDisabled === true,
    acquire: () => !props.powerDisabled,
    release: () => {},
    setPower: (on) => props.onSetVdd5v!(on),
  });
  return <StatusBar {...props} onSetVdd5v={props.onSetVdd5v ? action.set : undefined}
    powerBusy={action.pending} powerError={action.error} />;
}

function render(overrides: Partial<ComponentProps<typeof StatusBar>> = {}, lang = "en") {
  localStorage.setItem("lang", lang);
  localStorage.setItem("theme", "light");
  const host = document.createElement("div");
  document.body.append(host);
  const root = createRoot(host);
  const onSetVdd5v = vi.fn((_on: boolean) => Promise.resolve());
  const onParentClick = vi.fn();
  let props = {
    snapshot: snapshot(), connected: true, loading: false, auto: true,
    live: false, setAuto: vi.fn(), setLive: vi.fn(), onRefresh: vi.fn(),
    logicAnalyzerActive: false, onSetVdd5v, powerUpdatedAt: Date.now(), ...overrides,
  };
  const update = (next: Partial<ComponentProps<typeof StatusBar>> = {}) => {
    props = { ...props, ...next };
    act(() => root.render(
      <ThemeProvider><LanguageProvider>
        <div onClick={onParentClick}><ControlledHeader props={props} /></div>
      </LanguageProvider></ThemeProvider>,
    ));
  };
  update();
  cleanup.push(() => act(() => { root.unmount(); host.remove(); }));
  return {
    host, update, onSetVdd5v, onParentClick,
    toggle: host.querySelector<HTMLButtonElement>('[data-testid="logo-vdd5v-toggle"]')!,
    arm: host.querySelector('[data-indicator="vdd_5v"]')!,
  };
}

afterEach(() => {
  cleanup.splice(0).forEach((close) => close());
  vi.restoreAllMocks();
  localStorage.clear();
});

describe("Logo vdd_5v power control", () => {
  it.each(["off", "on"])("cancels from %s without a request, navigation, or any other header action", async (state) => {
    const confirm = vi.spyOn(window, "confirm").mockReturnValue(false);
    const view = render({ snapshot: snapshot(state) });
    await act(async () => view.toggle.click());
    expect(confirm).toHaveBeenCalledTimes(1);
    expect(confirm).toHaveBeenCalledWith(expect.stringContaining(`Turn vdd_5v ${state === "off" ? "on" : "off"}?`));
    expect(view.onSetVdd5v).not.toHaveBeenCalled();
    expect(view.onParentClick).not.toHaveBeenCalled();
    expect(view.toggle.closest("a")).toBeNull();
    expect(view.toggle.disabled).toBe(false);
    expect(view.toggle.getAttribute("aria-checked")).toBe(String(state === "on"));
  });

  it("confirms once, blocks duplicate activation while pending, and waits for authoritative state", async () => {
    const confirm = vi.spyOn(window, "confirm").mockReturnValue(true);
    let resolve!: () => void;
    const request = new Promise<void>((done) => { resolve = done; });
    const view = render();
    view.onSetVdd5v.mockReturnValue(request);
    act(() => { view.toggle.click(); view.toggle.click(); });
    expect(confirm).toHaveBeenCalledTimes(1);
    expect(view.onSetVdd5v).toHaveBeenCalledExactlyOnceWith(true);
    expect(view.toggle.disabled).toBe(true);
    expect(view.toggle.getAttribute("aria-busy")).toBe("true");
    expect(view.toggle.getAttribute("data-state")).toBe("pending");
    expect(view.host.querySelector('[data-testid="logo-vdd5v-status"]')?.textContent).toContain("OFF · Updating…");
    expect(view.toggle.getAttribute("aria-checked")).toBe("false");
    expect(view.arm.getAttribute("data-active")).toBe("false");
    await act(async () => { resolve(); await request; });
    expect(view.toggle.disabled).toBe(false);
    expect(view.toggle.getAttribute("aria-checked")).toBe("false");
    view.update({ snapshot: snapshot("on") });
    expect(view.toggle.getAttribute("aria-checked")).toBe("true");
    expect(view.toggle.getAttribute("data-state")).toBe("on");
    expect(view.arm.getAttribute("data-active")).toBe("true");
    expect(view.onParentClick).not.toHaveBeenCalled();
  });

  it("confirms shutdown separately and does not derive vdd_5v from the 5V output rail", async () => {
    const confirm = vi.spyOn(window, "confirm").mockReturnValue(true);
    const view = render({ snapshot: snapshot("on") });
    await act(async () => view.toggle.click());
    expect(confirm).toHaveBeenCalledTimes(1);
    expect(confirm).toHaveBeenCalledWith(expect.stringContaining("Unsaved target state may be lost"));
    expect(view.onSetVdd5v).toHaveBeenCalledExactlyOnceWith(false);
    view.update({ snapshot: snapshot("off") });
    expect(view.arm.getAttribute("data-active")).toBe("false");
    expect(view.host.querySelector('[data-indicator="5v_out"]')?.getAttribute("data-active")).toBe("true");
  });

  it.each([
    ["disconnected", { connected: false }],
    ["stale", { powerStale: true }],
    ["loading", { loading: true }],
    ["global task lock", { powerDisabled: true }],
    ["locked output", { snapshot: snapshot("locked") }],
    ["uncontrollable output", { snapshot: snapshot("off", false) }],
    ["missing output", { snapshot: EMPTY_BOARD_SNAPSHOT }],
    ["missing action", { onSetVdd5v: undefined }],
    ["unknown snapshot time", { powerUpdatedAt: null }],
  ] as const)("disables without confirmation or requests when %s", async (_name, props) => {
    const confirm = vi.spyOn(window, "confirm").mockReturnValue(true);
    const view = render(props);
    expect(view.toggle.disabled).toBe(true);
    await act(async () => view.toggle.click());
    expect(confirm).not.toHaveBeenCalled();
    expect(view.onSetVdd5v).not.toHaveBeenCalled();
  });

  it("rechecks the rendered state after confirmation and rejects a stale action", async () => {
    const view = render();
    vi.spyOn(window, "confirm").mockImplementation(() => {
      flushSync(() => view.update({ powerStale: true }));
      return true;
    });
    await act(async () => view.toggle.click());
    expect(view.onSetVdd5v).not.toHaveBeenCalled();
    expect(view.host.querySelector('[role="alert"]')?.textContent).toContain("Refresh and try again");
  });

  it.each([30_000, 31_000])("rejects confirmation after %i ms even when expiry timers and stale props have not updated", async (delayMs) => {
    const now = vi.spyOn(Date, "now").mockReturnValue(100_000);
    const view = render({ powerStale: false });
    const confirm = vi.spyOn(window, "confirm").mockImplementation(() => {
      // Advance wall time only: native confirm would prevent timer callbacks or rerenders.
      now.mockReturnValue(100_000 + delayMs);
      return true;
    });
    expect(view.toggle.disabled).toBe(false);
    await act(async () => view.toggle.click());
    expect(confirm).toHaveBeenCalledTimes(1);
    expect(view.onSetVdd5v).not.toHaveBeenCalled();
    expect(view.toggle.getAttribute("aria-checked")).toBe("false");
    expect(view.toggle.disabled).toBe(true);
    expect(view.host.querySelector('[role="alert"]')?.textContent).toContain("Refresh and try again");
  });

  it("accepts one confirmed action while its power snapshot is still younger than 30 s", async () => {
    const now = vi.spyOn(Date, "now").mockReturnValue(100_000);
    const view = render({ powerStale: false });
    const confirm = vi.spyOn(window, "confirm").mockImplementation(() => {
      now.mockReturnValue(129_999);
      return true;
    });
    await act(async () => view.toggle.click());
    expect(confirm).toHaveBeenCalledTimes(1);
    expect(view.onSetVdd5v).toHaveBeenCalledExactlyOnceWith(true);
    expect(view.host.querySelector('[role="alert"]')).toBeNull();
  });

  it("announces failures without changing the authoritative state and allows retry", async () => {
    const confirm = vi.spyOn(window, "confirm").mockReturnValue(true);
    const view = render();
    view.onSetVdd5v.mockRejectedValueOnce(new Error("device busy"));
    await act(async () => view.toggle.click());
    expect(view.host.querySelector('[role="alert"]')?.textContent).toBe("Could not change vdd_5v: device busy");
    expect(view.toggle.getAttribute("aria-checked")).toBe("false");
    expect(view.toggle.getAttribute("aria-busy")).toBe("false");
    expect(view.toggle.disabled).toBe(false);
    await act(async () => view.toggle.click());
    expect(confirm).toHaveBeenCalledTimes(2);
    expect(view.onSetVdd5v).toHaveBeenCalledTimes(2);
    expect(view.host.querySelector('[role="alert"]')).toBeNull();
  });

  it.each(["Enter", " "])("provides a named, focusable native switch for %s keyboard activation", async (key) => {
    const confirm = vi.spyOn(window, "confirm").mockReturnValue(true);
    const view = render();
    act(() => view.toggle.focus());
    expect(document.activeElement).toBe(view.toggle);
    expect(view.toggle.tagName).toBe("BUTTON");
    expect(view.toggle.type).toBe("button");
    expect(view.toggle.getAttribute("role")).toBe("switch");
    expect(view.toggle.getAttribute("aria-label")).toBe("vdd_5v power");
    expect(view.toggle.closest('[aria-hidden="true"]')).toBeNull();
    expect(document.getElementById(view.toggle.getAttribute("aria-describedby")!)?.textContent).toBe("vdd_5v · OFF");
    // jsdom has no native keyboard default actions; deliver the browser-generated click explicitly.
    await act(async () => {
      const down = new KeyboardEvent("keydown", { key, bubbles: true, cancelable: true });
      view.toggle.dispatchEvent(down);
      expect(down.defaultPrevented).toBe(false);
      view.toggle.dispatchEvent(new KeyboardEvent("keyup", { key, bubbles: true }));
      view.toggle.dispatchEvent(new MouseEvent("click", { bubbles: true, cancelable: true, detail: 0 }));
    });
    expect(confirm).toHaveBeenCalledTimes(1);
    expect(view.onSetVdd5v).toHaveBeenCalledExactlyOnceWith(true);
    expect(view.onParentClick).not.toHaveBeenCalled();
  });

  it("localizes the accessible name, state, and safety confirmation in Chinese", async () => {
    const confirm = vi.spyOn(window, "confirm").mockReturnValue(false);
    const view = render({ snapshot: snapshot("on") }, "zh");
    expect(view.toggle.getAttribute("aria-label")).toBe("vdd_5v 电源");
    expect(view.host.querySelector('[data-testid="logo-vdd5v-status"]')?.textContent).toContain("开");
    await act(async () => view.toggle.click());
    expect(confirm).toHaveBeenCalledWith(expect.stringContaining("关闭 vdd_5v 电源？"));
    expect(view.onSetVdd5v).not.toHaveBeenCalled();
  });

  it.each(["en", "zh"])("keeps the complete %s status in a reserved header slot without expanding the logo hit area", (lang) => {
    const view = render({ powerStale: true }, lang);
    const status = view.host.querySelector<HTMLElement>('[data-testid="logo-vdd5v-status"]')!;
    const details = view.host.querySelector<HTMLElement>('[data-testid="status-details"]')!;
    const description = document.getElementById(view.toggle.getAttribute("aria-describedby")!)!;
    expect(status.textContent).toBe(description.textContent);
    expect(status.textContent).toContain(lang === "zh" ? "只读快照" : "Read-only snapshot");
    expect(status.parentElement).toBe(details);
    expect(details.firstElementChild).toBe(status);
    expect(status.classList.contains("header-power-status")).toBe(true);
    expect(status.closest("button, svg")).toBeNull();
    expect(status.querySelector("button, a")).toBeNull();
    expect(details.classList.contains("overflow-x-auto")).toBe(true);
    expect(details.classList.contains("min-w-0")).toBe(true);
    expect(details.className).not.toContain("justify-center");
    // No custom or browser tooltip may escape the 56px header into the navigation.
    expect(view.toggle.hasAttribute("title")).toBe(false);
    expect(view.toggle.querySelector("span")).toBeNull();
    expect(view.toggle.className).toContain("absolute left-3 top-3 grid h-6 w-6");
    expect(view.toggle.className).not.toContain("group");
  });
});
