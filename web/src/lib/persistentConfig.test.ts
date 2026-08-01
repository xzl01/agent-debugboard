import { afterEach, describe, expect, it, vi } from "vitest";
import {
  applyPersistentConfig,
  clearPersistentConfig,
  getPersistentConfig,
  savePersistentConfig,
} from "./api";
import {
  groupPersistentConfigItems,
  parsePersistentConfigGet,
  parsePersistentConfigSummary,
  mergePersistentConfigSummary,
} from "./persistentConfig";

function getResponse(items: unknown): Record<string, unknown> {
  return {
    schema: "radxa-linkr-debugger.v1", ok: true, command: "config", action: "get",
    backend: { available: true, reason: "ready" },
    snapshot: { present: true, version: 1 }, pending: 1, items,
  };
}

function response(body: unknown, status: number = 200): Response {
  return new Response(JSON.stringify(body), { status, statusText: "test" });
}

describe("persistent configuration contracts", () => {
  afterEach(() => vi.unstubAllGlobals());

  it("normalizes all three firmware value variants and groups enumerated items", () => {
    const config = parsePersistentConfigGet(getResponse([
      { id: "rail", kind: "power", current: { state: "on" }, saved: { state: "off" }, selected: true, requires_confirm: true, apply_state: "pending" },
      { id: "mux", kind: "switch", current: { route: "target" }, saved: null, selected: false, requires_confirm: false, apply_state: "not_saved" },
      { id: "pin", kind: "gpio", current: { direction: "output", value: 1 }, saved: { direction: "input", value: 0 }, selected: true, requires_confirm: null, apply_state: "applied" },
    ]));

    expect(config.items.map((item) => item.kind)).toEqual(["power", "switch", "gpio"]);
    expect(groupPersistentConfigItems(config.items).power).toHaveLength(1);
    expect(config.items[0]?.risk).toBe("confirmation_required");
  });

  it("tolerates absent summaries and future fields or kinds", () => {
    expect(parsePersistentConfigSummary(undefined)).toBeUndefined();
    expect(parsePersistentConfigSummary({ available: true, reason: "future", saved_count: 2, pending_count: 1, future: true })).toMatchObject({ reason: "future" });
    expect(parsePersistentConfigGet(getResponse([
      { id: "future", kind: "relay", current: { state: "on" }, saved: null, selected: false, requires_confirm: false, apply_state: "future" },
    ])).items[0]?.kind).toBe("unknown");
  });

  it("reconciles HTTP and WS summaries without replacing valid data with malformed data", () => {
    const previous = parsePersistentConfigSummary({ available: true, reason: "ready", saved_count: 2, pending_count: 1 });
    expect(mergePersistentConfigSummary(previous, { available: true, reason: "bad", saved_count: 1, pending_count: 2 })).toEqual(previous);
    expect(mergePersistentConfigSummary(previous, { available: true, reason: "absent", saved_count: 0, pending_count: 0 })).toMatchObject({ reason: "absent" });
  });

  it("rejects invalid schemas and non-integer config counts", () => {
    expect(() => parsePersistentConfigGet({ ...getResponse([]), schema: "other" })).toThrow();
    expect(() => parsePersistentConfigGet({ ...getResponse([]), pending: -1 })).toThrow();
    expect(() => parsePersistentConfigGet({ ...getResponse([]), snapshot: { present: true, version: 1.5 } })).toThrow();
    expect(parsePersistentConfigSummary({ available: true, reason: "ready", saved_count: 1.5, pending_count: 0 })).toBeUndefined();
  });

  it("rejects malformed known rows while retaining explicit null and future kinds", () => {
    expect(() => parsePersistentConfigGet(getResponse([{ id: "rail", kind: "power", current: { state: "bad" }, saved: null }]))).toThrow();
    expect(() => parsePersistentConfigGet(getResponse([{ kind: "power", current: null, saved: null }]))).toThrow();
  });

  it("requires every frozen catalog row field", () => {
    const row = { id: "rail", kind: "power", current: null, saved: null, selected: false, requires_confirm: false, apply_state: "not_saved" };
    for (const field of ["current", "saved", "selected", "requires_confirm", "apply_state"] as const) {
      const copy = { ...row };
      delete copy[field];
      expect(() => parsePersistentConfigGet(getResponse([copy]))).toThrow();
    }
  });

  it("sends all mutation bodies and preserves typed dangerous and partial errors", async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(response(getResponse([])))
      .mockResolvedValueOnce(response({ schema: "radxa-linkr-debugger.v1", ok: true, command: "config", action: "save", saved_items: ["rail"], confirmation_items: [], snapshot: { present: true, version: 1 }, pending: 0 }))
      .mockResolvedValueOnce(response({ schema: "radxa-linkr-debugger.v1", ok: true, command: "config", action: "apply", noop: false, applied_items: ["rail"], failed_item: null, pending_items: [] }))
      .mockResolvedValueOnce(response({ schema: "radxa-linkr-debugger.v1", ok: true, command: "config", action: "clear", noop: false, snapshot: { present: false, version: null }, pending: 0 }))
      .mockResolvedValueOnce(response({ schema: "radxa-linkr-debugger.v1", ok: false, command: "config", action: "apply", error: { code: "confirmation_required", message: "confirm" }, dangerous_items: ["rail"] }, 409))
      .mockResolvedValueOnce(response({ schema: "radxa-linkr-debugger.v1", ok: false, command: "config", action: "apply", error: { code: "apply_failed", message: "partial" }, applied_items: ["rail"], failed_item: "mux", pending_items: ["mux"] }, 500));
    vi.stubGlobal("fetch", fetchMock);

    await getPersistentConfig();
    await savePersistentConfig(["rail"], true);
    await applyPersistentConfig(true);
    await clearPersistentConfig();
    await expect(applyPersistentConfig(false)).rejects.toMatchObject({ detail: { kind: "confirmation_required", dangerousIds: ["rail"] } });
    await expect(applyPersistentConfig(true)).rejects.toMatchObject({ detail: { kind: "apply_failed", appliedIds: ["rail"], failedId: "mux", pendingIds: ["mux"] } });

    expect(fetchMock.mock.calls[1]?.[1]).toMatchObject({ method: "PUT", body: JSON.stringify({ items: ["rail"], confirm: true }) });
    expect(fetchMock.mock.calls[2]?.[1]).toMatchObject({ method: "POST", body: JSON.stringify({ confirm: true }) });
    expect(fetchMock.mock.calls[3]?.[1]).toMatchObject({ method: "DELETE" });
  });

  it("rejects malformed successful mutation envelopes", async () => {
    vi.stubGlobal("fetch", vi.fn().mockResolvedValue(response({ ok: true, command: "config", action: "save" })));
    await expect(savePersistentConfig(["rail"], false)).rejects.toMatchObject({ detail: { code: "invalid_response" } });
  });
});
