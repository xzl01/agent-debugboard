import { act } from "react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { IDBFactory } from "fake-indexeddb";
import { getTaskIr, getTaskMetadata, getTaskSource } from "@/lib/api";
import { buildTaskBlob } from "@/lib/taskRequests";
import { defaultScript, serializeTestScript, type TestScript } from "@/lib/testScript";
import { workflowLibrary } from "@/lib/workflowLibrary";
import { downloadBlob } from "@/lib/utils";
import { catalogRowIds, fetchCalls, flush, flushUntil, mountTaskCard, putCalls, rowFor, setupTaskCardHarness, storedListPayload, stubConfirmResult, stubTaskList, stubTaskSource, teardownTaskCardHarness, unmountTaskCard } from "./TaskCard.testUtils";

vi.mock("@/lib/utils", async (original) => ({ ...await original<typeof import("@/lib/utils")>(), downloadBlob: vi.fn() }));
function script(name: string): TestScript {
  return { ...defaultScript(name), created: "2026-10-22T00:00:00.000Z", steps: [{ id: "loop", type: "loop", params: { count: 2, steps: [
    { id: "p", type: "power_on", params: { rail: "5v_out" } }, { id: "d", type: "delay", params: { ms: 10 } },
  ] } }] };
}
function button(host: ParentNode, text: string) { const found = [...host.querySelectorAll<HTMLButtonElement>("button")].find((item) => item.textContent?.trim() === text); if (!found) throw new Error(`Missing ${text}`); return found; }
function type(element: HTMLInputElement | HTMLTextAreaElement, value: string) { act(() => { Object.getOwnPropertyDescriptor(element instanceof HTMLTextAreaElement ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype, "value")!.set!.call(element, value); element.dispatchEvent(new Event("input", { bubbles: true })); }); }
const mutations = () => fetchCalls().filter((call) => call.url === "/api/v1/tasks/mutations");
beforeEach(async () => { await workflowLibrary.close(); setupTaskCardHarness(); vi.stubGlobal("indexedDB", new IDBFactory()); vi.mocked(downloadBlob).mockClear(); });
afterEach(async () => { teardownTaskCardHarness(); await workflowLibrary.close(); });

describe("Task-v2 editor and independent IR tools", () => {
  it("atomically saves A then B, retains both exact sources and reopens A from a fresh profile", async () => {
    const a = script("A"), b = script("B");
    let view = await mountTaskCard(true, { script: a });
    type(view.querySelector("input")!, "task-A"); act(() => button(view, "Store current workflow").click());
    await flushUntil(() => catalogRowIds(view).includes("task-A") && !button(view, "Store current workflow").disabled);
    unmountTaskCard(); view = await mountTaskCard(true, { script: b });
    type(view.querySelector("input")!, "task-B"); act(() => button(view, "Store current workflow").click());
    await flushUntil(() => catalogRowIds(view).includes("task-B") && !button(view, "Store current workflow").disabled);
    expect((await getTaskMetadata()).tasks.map((task) => task.id)).toEqual(["task-A", "task-B"]);
    expect((await getTaskSource("task-A")).source).toBe(serializeTestScript(a));
    expect((await getTaskSource("task-B")).source).toBe(serializeTestScript(b));
    unmountTaskCard(); await workflowLibrary.close(); vi.stubGlobal("indexedDB", new IDBFactory()); localStorage.clear();
    const open = vi.fn(); view = await mountTaskCard(true, { onOpenWorkflow: open });
    act(() => button(rowFor(view, "task-A"), "Open workflow source").click());
    await flushUntil(() => open.mock.calls.length === 1);
    expect(serializeTestScript(open.mock.calls[0][0])).toBe(serializeTestScript(a));
    expect(putCalls()).toEqual([]); expect(mutations()).toHaveLength(2);
  });
  it("does not deploy when overwrite is cancelled", async () => {
    const original = script("Original"); const ir = buildTaskBlob("same", original);
    stubTaskList(storedListPayload([{ id: "same", name: "same", request_count: 2 }], ir)); stubTaskSource("same", original);
    const view = await mountTaskCard(true, { script: script("Changed") }); stubConfirmResult(false);
    type(view.querySelector("input")!, "same"); act(() => button(view, "Store current workflow").click()); await flush();
    expect(mutations()).toEqual([]); expect((await getTaskSource("same")).source).toBe(serializeTestScript(original));
  });
  it("reports a stale revision without retrying or overwriting the store", async () => {
    const original = script("A"); const ir = buildTaskBlob("A", original);
    const payload = storedListPayload([{ id: "A", name: "A", request_count: 2 }], ir);
    stubTaskList(payload); const view = await mountTaskCard(true, { script: script("B") });
    stubTaskList({ ...payload, revision: "2" }); type(view.querySelector("input")!, "B");
    act(() => button(view, "Store current workflow").click());
    await flushUntil(() => view.textContent?.includes("no automatic retry") === true);
    expect(mutations()).toHaveLength(1); expect((await getTaskIr()).blob).toBe(ir); expect(putCalls()).toEqual([]);
  });
  it("honestly labels IR-only and built-in rows, exports IR and offers no false lossless open", async () => {
    const ir = buildTaskBlob("IR-only", script("IR"));
    stubTaskList(storedListPayload([{ id: "IR-only", name: "IR-only", request_count: 2 }], ir));
    const view = await mountTaskCard(true, { onOpenWorkflow: vi.fn() });
    const row = rowFor(view, "IR-only"); expect(row.textContent).toContain("no editable source");
    expect(row.textContent).not.toContain("Open workflow source");
    expect(rowFor(view, "MASKROM via 5v_out").textContent).toContain("no lossless workflow source");
    act(() => button(row, "Export task IR").click()); await flushUntil(() => vi.mocked(downloadBlob).mock.calls.length === 1);
    expect(vi.mocked(downloadBlob).mock.calls[0][1]).toBe(ir);
    expect(button(view, "Import task IR")).not.toBeNull(); expect(button(view, "Export all task IR")).not.toBeNull();
    expect(putCalls()).toEqual([]); expect(mutations()).toEqual([]);
  });
  it("previews file/text IR, requires a multi-task choice, and upserts only the selected task", async () => {
    const view = await mountTaskCard(); act(() => button(view, "Import task IR").click());
    const ir = buildTaskBlob("one", script("one")) + buildTaskBlob("two", script("two")).slice(16);
    type(document.querySelector("textarea")!, "not IR"); act(() => button(document.body, "Preview import").click());
    expect(document.querySelector('[role="dialog"] [role="alert"]')).not.toBeNull(); expect(mutations()).toEqual([]);
    type(document.querySelector("textarea")!, ir); act(() => button(document.body, "Preview import").click());
    expect(button(document.body, "Import selected — do not run").disabled).toBe(true);
    expect(document.querySelector('input[type="file"]')).not.toBeNull();
    act(() => document.querySelector<HTMLInputElement>('input[type="radio"][value="two"]')!.click());
    act(() => button(document.body, "Import selected — do not run").click());
    await flushUntil(() => catalogRowIds(view).includes("two"));
    expect((await getTaskMetadata()).tasks.map((task) => task.id)).toEqual(["two"]);
    expect(mutations()).toHaveLength(1); expect(JSON.parse(mutations()[0].body!).action).toBe("upsert"); expect(putCalls()).toEqual([]);
  });
  it("marks source above 4000 bytes as local-only and explicitly deploys IR without source", async () => {
    const big = script("x".repeat(4100)); const view = await mountTaskCard(true, { script: big });
    expect(view.textContent).toContain("exceeds 4000 bytes"); type(view.querySelector("input")!, "big");
    act(() => button(view, "Store current workflow").click());
    await flushUntil(() => catalogRowIds(view).includes("big") && !button(view, "Store current workflow").disabled);
    const body = JSON.parse(mutations()[0].body!) as Record<string, string>;
    expect(body.source_base64).toBeUndefined();
    expect(Buffer.from(body.ir_base64!, "base64").toString("utf8")).toContain("# task big\n");
    expect((await getTaskMetadata()).tasks[0].source_state).toBe("absent");
    expect((await workflowLibrary.listWorkflows())[0].local_only).toBe(true);
  });
  it("rediscovers and opens verified local-only exact source after task refresh and remount", async () => {
    const big = script("x".repeat(4100)); const open = vi.fn();
    let view = await mountTaskCard(true, { script: big, onOpenWorkflow: open });
    type(view.querySelector("input")!, "local-big"); act(() => button(view, "Store current workflow").click());
    await flushUntil(() => view.textContent?.includes("Exact source in this browser only") === true && !button(view, "Store current workflow").disabled);
    unmountTaskCard(); view = await mountTaskCard(true, { onOpenWorkflow: open });
    await flushUntil(() => rowFor(view, "local-big").textContent?.includes("Exact source in this browser only") === true);
    act(() => view.querySelector<HTMLButtonElement>('[aria-label="Refresh"]')!.click());
    await flushUntil(() => rowFor(view, "local-big").textContent?.includes("Exact source in this browser only") === true);
    const row = rowFor(view, "local-big"); expect(row.textContent).not.toContain("Editable source on board");
    act(() => button(row, "Open local workflow source").click()); await flushUntil(() => open.mock.calls.length === 1);
    expect(serializeTestScript(open.mock.calls[0][0])).toBe(serializeTestScript(big));
    expect(fetchCalls().filter((call) => /\/slots\/\d+\/source$/.test(call.url))).toEqual([]);
    expect(mutations()).toHaveLength(1); expect(putCalls()).toEqual([]);
  });
  it.each(["device UUID", "task digest"])("does not offer local source when the %s no longer matches", async (changed) => {
    const big = script("x".repeat(4100)); let view = await mountTaskCard(true, { script: big, onOpenWorkflow: vi.fn() });
    type(view.querySelector("input")!, "local-big"); act(() => button(view, "Store current workflow").click());
    await flushUntil(() => view.textContent?.includes("Exact source in this browser only") === true && !button(view, "Store current workflow").disabled);
    const { metadata, blob } = await getTaskIr();
    unmountTaskCard();
    stubTaskList({ ...storedListPayload([], blob), ...metadata,
      ...(changed === "device UUID" ? { task_store_id: "22345678-1234-4234-8234-123456789abc" } : { tasks: metadata.tasks.map((task) => ({ ...task, task_ir_digest: "f".repeat(64) })) }) });
    const open = vi.fn(); view = await mountTaskCard(true, { onOpenWorkflow: open }); await flush();
    const row = rowFor(view, "local-big"); expect(row.textContent).not.toContain("Open local workflow source");
    expect(row.textContent).not.toContain("Exact source in this browser only");
    expect(open).not.toHaveBeenCalled(); expect(mutations()).toHaveLength(1); expect(putCalls()).toEqual([]);
  });
  it("rechecks local deployment identity on click if the device changed after refresh", async () => {
    const big = script("x".repeat(4100)); const open = vi.fn(); const view = await mountTaskCard(true, { script: big, onOpenWorkflow: open });
    type(view.querySelector("input")!, "local-big"); act(() => button(view, "Store current workflow").click());
    await flushUntil(() => view.textContent?.includes("Exact source in this browser only") === true && !button(view, "Store current workflow").disabled);
    const { metadata, blob } = await getTaskIr();
    stubTaskList({ ...storedListPayload([], blob), ...metadata, task_store_id: "22345678-1234-4234-8234-123456789abc" });
    act(() => button(rowFor(view, "local-big"), "Open local workflow source").click());
    await flushUntil(() => view.textContent?.includes("No verified local source matches") === true);
    expect(open).not.toHaveBeenCalled(); expect(rowFor(view, "local-big").textContent).not.toContain("Open local workflow source");
  });
  it("prefers fresh board source over a browser-local deployment association", async () => {
    const big = script("x".repeat(4100)); let view = await mountTaskCard(true, { script: big });
    type(view.querySelector("input")!, "local-big"); act(() => button(view, "Store current workflow").click());
    await flushUntil(() => view.textContent?.includes("Exact source in this browser only") === true && !button(view, "Store current workflow").disabled);
    unmountTaskCard(); const boardSource = script("Fresh board source"); stubTaskSource("local-big", boardSource);
    const open = vi.fn(); view = await mountTaskCard(true, { onOpenWorkflow: open });
    const row = rowFor(view, "local-big"); expect(row.textContent).toContain("Editable source on board");
    expect(row.textContent).not.toContain("Open local workflow source");
    act(() => button(row, "Open workflow source").click()); await flushUntil(() => open.mock.calls.length === 1);
    expect(serializeTestScript(open.mock.calls[0][0])).toBe(serializeTestScript(boardSource));
  });
  it.each(["delay", "current_range"])("renders invalid %s draft without crashing and refuses deployment before any mutation", async (kind) => {
    const invalid: TestScript = { ...defaultScript("Invalid draft"), steps: kind === "delay"
      ? [{ id: "d", type: "delay", params: { ms: -1 } }]
      : [{ id: "adc", type: "adc_read", params: { channel: "5v_out" }, assert: { current_range: {} as { min_a: number; max_a: number } } }] };
    const view = await mountTaskCard(true, { script: invalid });
    expect(view.textContent).toContain("Draft needs correction");
    act(() => button(view, "Store current workflow").click()); await flush();
    expect(view.querySelector('[role="alert"]')?.textContent).toContain("Draft needs correction");
    expect(mutations()).toEqual([]); expect(await workflowLibrary.listWorkflows()).toEqual([]);
  });
  it("disables lossless opening when verified source cannot reproduce the deployed IR", async () => {
    const source = script("Source"); const different: TestScript = { ...defaultScript("Different IR"), steps: [{ id: "p", type: "power_off", params: { rail: "12v_out" } }] };
    stubTaskList(storedListPayload([{ id: "mismatch", name: "mismatch", request_count: 1 }], buildTaskBlob("mismatch", different)));
    stubTaskSource("mismatch", source); const open = vi.fn(); const view = await mountTaskCard(true, { onOpenWorkflow: open });
    act(() => button(rowFor(view, "mismatch"), "Open workflow source").click());
    await flushUntil(() => view.textContent?.includes("Source verification failed") === true);
    expect(button(rowFor(view, "mismatch"), "Open workflow source").disabled).toBe(true);
    expect(button(rowFor(view, "mismatch"), "Run task").disabled).toBe(false);
    expect(open).not.toHaveBeenCalled(); expect(mutations()).toEqual([]); expect(putCalls()).toEqual([]);
  });
  it("requires explicit whole-store replacement confirmation, preserves cancelled state, then imports only on approval", async () => {
    const original = buildTaskBlob("keep", script("Keep"));
    stubTaskList(storedListPayload([{ id: "keep", name: "keep", request_count: 2 }], original));
    const view = await mountTaskCard(); act(() => button(view, "Import task IR").click());
    type(document.querySelector("textarea")!, buildTaskBlob("replace", script("Replace")));
    act(() => button(document.body, "Preview import").click());
    act(() => [...document.querySelectorAll<HTMLInputElement>('input[type="radio"]')].at(-1)!.click());
    stubConfirmResult(false); act(() => button(document.body, "Import selected — do not run").click());
    await flushUntil(() => document.querySelector('[role="dialog"]')?.textContent?.includes("Import cancelled") === true);
    expect(mutations()).toEqual([]); expect((await getTaskIr()).blob).toBe(original);
    stubConfirmResult(true); act(() => button(document.body, "Import selected — do not run").click());
    await flushUntil(() => catalogRowIds(view).includes("replace"));
    expect(mutations()).toHaveLength(1); expect(JSON.parse(mutations()[0].body!).action).toBe("import");
    expect((await getTaskMetadata()).tasks.map((task) => task.id)).toEqual(["replace"]); expect(putCalls()).toEqual([]);
  });
});
