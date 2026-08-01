import { act } from "react";
import { createRoot } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import * as api from "@/lib/api";
import type { PersistentConfig, PersistentConfigSummary } from "@/lib/persistentConfig";
import { PersistentConfigApiError } from "@/lib/persistentConfig";
import {
  usePersistentConfig,
  type PersistentConfigMutation,
  type UsePersistentConfig,
} from "./usePersistentConfig";

vi.mock("@/lib/api", () => ({
  getPersistentConfig: vi.fn(), savePersistentConfig: vi.fn(), applyPersistentConfig: vi.fn(), clearPersistentConfig: vi.fn(),
}));
Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const summary: PersistentConfigSummary = { available: true, reason: "ready", savedCount: 1, pendingCount: 0 };
function config(saved: "on" | null): PersistentConfig {
  return { backend: { available: true, reason: "ready" }, snapshot: { present: saved !== null, version: saved === null ? null : 1 }, pending: 0, items: [{ id: "item", kind: "power", current: { kind: "power", state: "on" }, saved: saved === null ? null : { kind: "power", state: saved }, selected: saved !== null, risk: "safe", applyState: saved === null ? "not_saved" : "applied" }] };
}
function deferred<T>() {
  let resolve: (value: T) => void = () => {};
  const promise = new Promise<T>((done) => { resolve = done; });
  return { promise, resolve };
}
type HookOptions = {
  readonly connected: boolean;
  readonly summary?: PersistentConfigSummary;
  readonly currentStateKey?: string;
};
function render(options: HookOptions) {
  let current: UsePersistentConfig | null = null;
  const host = document.createElement("div"); const root = createRoot(host);
  function Probe(props: HookOptions) {
    current = usePersistentConfig({ ...props, currentStateKey: props.currentStateKey ?? "controls-a" });
    return null;
  }
  const update = (next: HookOptions) => act(() => { root.render(<Probe {...next} />); });
  update(options);
  return {
    current: () => {
      if (current === null) throw new TypeError("Hook did not render");
      return current;
    },
    update,
    close: () => act(() => root.unmount()),
  };
}
async function flush() { await act(async () => { await Promise.resolve(); }); }
function runMutation(state: UsePersistentConfig, activity: PersistentConfigMutation): Promise<void> {
  switch (activity) {
    case "save": return state.save(["item"], false);
    case "apply": return state.apply(false);
    case "clear": return state.clear();
  }
}
function mockSuccessfulMutation(activity: PersistentConfigMutation): void {
  switch (activity) {
    case "save": vi.mocked(api.savePersistentConfig).mockResolvedValueOnce(undefined); return;
    case "apply": vi.mocked(api.applyPersistentConfig).mockResolvedValueOnce(undefined); return;
    case "clear": vi.mocked(api.clearPersistentConfig).mockResolvedValueOnce(undefined); return;
  }
}

describe("usePersistentConfig", () => {
  afterEach(() => vi.clearAllMocks());

  it("invalidates once per actual current-state key change and coalesces summary changes", async () => {
    vi.mocked(api.getPersistentConfig).mockResolvedValue(config("on"));
    const view = render({ connected: true, summary, currentStateKey: "controls-a" }); await flush();
    view.update({ connected: true, summary: { ...summary }, currentStateKey: "controls-a" }); await flush();
    expect(api.getPersistentConfig).toHaveBeenCalledTimes(1);
    view.update({ connected: true, summary, currentStateKey: "controls-b" }); await flush();
    expect(api.getPersistentConfig).toHaveBeenCalledTimes(2);
    view.update({
      connected: true,
      summary: { ...summary, savedCount: 2 },
      currentStateKey: "controls-c",
    });
    await flush();
    expect(api.getPersistentConfig).toHaveBeenCalledTimes(3);
    view.close();
  });

  it.each<PersistentConfigMutation>(["save", "apply", "clear"])(
    "keeps %s pending until a current-key superseding response commits",
    async (activity) => {
      const mutationGet = deferred<PersistentConfig>(); const winningGet = deferred<PersistentConfig>();
      vi.mocked(api.getPersistentConfig)
        .mockResolvedValueOnce(config(null))
        .mockReturnValueOnce(mutationGet.promise)
        .mockReturnValueOnce(winningGet.promise);
      mockSuccessfulMutation(activity);
      const view = render({ connected: true, summary, currentStateKey: "controls-a" }); await flush();
      expect(api.getPersistentConfig).toHaveBeenCalledTimes(1);
      const mutation = runMutation(view.current(), activity);
      let settled = false; void mutation.then(() => { settled = true; }, () => { settled = true; });
      await flush();
      expect(api.getPersistentConfig).toHaveBeenCalledTimes(2);
      view.update({ connected: true, summary, currentStateKey: "controls-b" }); await flush();
      expect(api.getPersistentConfig).toHaveBeenCalledTimes(3);
      await act(async () => { mutationGet.resolve(config("on")); });
      expect(settled).toBe(false);
      expect(api.getPersistentConfig).toHaveBeenCalledTimes(3);
      expect(view.current()?.config?.items[0]?.saved).toBeNull();
      await act(async () => { winningGet.resolve(config("on")); });
      await act(async () => { await mutation; });
      expect(view.current()?.config?.items[0]?.saved).toMatchObject({ state: "on" });
      expect(view.current()?.busy).toBeNull();
      view.close();
    }
  );

  it.each([
    ["disconnect", "disconnected"],
    ["unsupported firmware", "unsupported"],
    ["unmount", "unmounted"],
  ])("rejects an authoritative mutation on %s", async (transition, code) => {
    const mutationGet = deferred<PersistentConfig>();
    vi.mocked(api.getPersistentConfig).mockResolvedValueOnce(config(null)).mockReturnValueOnce(mutationGet.promise);
    vi.mocked(api.savePersistentConfig).mockResolvedValueOnce(undefined);
    const view = render({ connected: true, summary }); await flush();
    const mutation = view.current()?.save(["item"], false); await flush();
    const rejected = expect(mutation).rejects.toMatchObject({ detail: { code } });
    if (transition === "disconnect") view.update({ connected: false, summary });
    if (transition === "unsupported firmware") view.update({ connected: true });
    if (transition === "unmount") view.close();
    await act(async () => { mutationGet.resolve(config("on")); });
    await rejected;
    if (transition !== "unmount") view.close();
  });

  it("keeps the newer authoritative response when loads resolve out of order", async () => {
    const older = deferred<PersistentConfig>(); const newer = deferred<PersistentConfig>();
    vi.mocked(api.getPersistentConfig).mockReturnValueOnce(older.promise).mockReturnValueOnce(newer.promise);
    const view = render({ connected: true, summary });
    view.update({ connected: true, summary: { ...summary, savedCount: 2 } });
    await act(async () => { newer.resolve(config("on")); });
    await act(async () => { older.resolve(config(null)); });
    expect(view.current()?.config?.items[0]?.saved).toMatchObject({ state: "on" });
    view.close();
  });

  it("refetches on reconnect and only changes the display after clear refetches", async () => {
    const refreshed = deferred<PersistentConfig>();
    vi.mocked(api.getPersistentConfig).mockResolvedValueOnce(config("on")).mockResolvedValueOnce(config("on")).mockReturnValueOnce(refreshed.promise);
    vi.mocked(api.clearPersistentConfig).mockResolvedValue(undefined);
    const view = render({ connected: true, summary }); await flush();
    view.update({ connected: false, summary }); await flush();
    view.update({ connected: true, summary }); await flush();
    expect(api.getPersistentConfig).toHaveBeenCalledTimes(2);
    const clear = view.current()?.clear();
    await flush();
    expect(view.current()?.config?.items[0]?.saved).toMatchObject({ state: "on" });
    await act(async () => { refreshed.resolve(config(null)); });
    await clear;
    expect(view.current()?.config?.items[0]?.current).toMatchObject({ state: "on" });
    expect(view.current()?.config?.items[0]?.saved).toBeNull();
    view.close();
  });

  it.each<PersistentConfigMutation>(["save", "apply", "clear"])(
    "rejects a successful %s when its authoritative GET fails",
    async (activity) => {
      const failure = new PersistentConfigApiError({ kind: "other", code: "storage_error" }, "storage");
      vi.mocked(api.getPersistentConfig).mockResolvedValueOnce(config("on")).mockRejectedValueOnce(failure);
      mockSuccessfulMutation(activity);
      const view = render({ connected: true, summary }); await flush();
      const mutation = runMutation(view.current(), activity);
      await act(async () => { await expect(mutation).rejects.toBe(failure); });
      expect(view.current()?.config?.items[0]?.saved).toMatchObject({ state: "on" });
      expect(view.current()?.error).toBe(failure);
      view.close();
    }
  );

  it("does not let an older mutation clear a newer failure", async () => {
    const first = deferred<void>();
    const newerFailure = new PersistentConfigApiError({ kind: "busy", activity: "capture" }, "busy");
    vi.mocked(api.getPersistentConfig).mockResolvedValue(config("on"));
    vi.mocked(api.savePersistentConfig).mockReturnValueOnce(first.promise).mockRejectedValueOnce(newerFailure);
    const view = render({ connected: true, summary }); await flush();
    const older = view.current()?.save(["item"], false);
    const newer = view.current()?.save(["item"], false);
    await flush();
    expect(api.savePersistentConfig).toHaveBeenCalledTimes(1);
    await act(async () => { first.resolve(); });
    await older;
    await expect(newer).rejects.toBe(newerFailure);
    expect(view.current()?.error).toBe(newerFailure);
    expect(view.current()?.busy).toBeNull();
    view.close();
  });

  it("clears a previous refresh error after a later current refresh succeeds", async () => {
    const failure = new PersistentConfigApiError({ kind: "other", code: "storage_error" }, "storage");
    vi.mocked(api.getPersistentConfig).mockRejectedValueOnce(failure).mockResolvedValueOnce(config("on"));
    const view = render({ connected: true, summary }); await flush();
    let refresh = Promise.resolve();
    act(() => { refresh = view.current().refresh(); });
    await flush();
    await expect(refresh).resolves.toBeUndefined();
    expect(view.current()?.error).toBeNull();
    view.close();
  });

  it("cancels an in-flight GET on disconnect without losing displayed config", async () => {
    const pending = deferred<PersistentConfig>();
    vi.mocked(api.getPersistentConfig).mockReturnValueOnce(pending.promise);
    const view = render({ connected: true, summary }); await flush();
    view.update({ connected: false, summary });
    await act(async () => { pending.resolve(config(null)); });
    expect(view.current()?.config).toBeNull();
    expect(view.current()?.loading).toBe(false);
    view.close();
  });

  it("rejects refresh and mutations while disconnected without API calls", async () => {
    const view = render({ connected: false, summary }); await flush();
    await expect(view.current()?.refresh()).rejects.toMatchObject({ detail: { code: "disconnected" } });
    await expect(view.current()?.save(["item"], false)).rejects.toMatchObject({ detail: { code: "disconnected" } });
    expect(api.getPersistentConfig).not.toHaveBeenCalled();
    expect(api.savePersistentConfig).not.toHaveBeenCalled();
    view.close();
  });

  it("does not run queued mutation work or GET after unmount", async () => {
    const first = deferred<void>();
    vi.mocked(api.getPersistentConfig).mockResolvedValue(config("on"));
    vi.mocked(api.savePersistentConfig).mockReturnValueOnce(first.promise).mockResolvedValueOnce(undefined);
    const view = render({ connected: true, summary }); await flush();
    const one = view.current()?.save(["item"], false);
    const two = view.current()?.save(["item"], false);
    await flush();
    expect(api.savePersistentConfig).toHaveBeenCalledTimes(1);
    view.close();
    await act(async () => { first.resolve(); });
    await expect(one).rejects.toMatchObject({ detail: { code: "unmounted" } });
    await expect(two).rejects.toMatchObject({ detail: { code: "unmounted" } });
    expect(api.savePersistentConfig).toHaveBeenCalledTimes(1);
    expect(api.getPersistentConfig).toHaveBeenCalledTimes(1);
  });
});
