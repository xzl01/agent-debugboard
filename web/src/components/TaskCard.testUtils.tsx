import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { vi } from "vitest";
import {
  createAutomationTaskLock,
  type AutomationTaskControl,
  type AutomationTaskOwner,
} from "@/lib/automationTask";
import { LanguageProvider } from "@/lib/i18n";
import { defaultScript, serializeTestScript, type TestScript } from "@/lib/testScript";
import { TaskCard } from "./TaskCard";
import { createHash } from "node:crypto";
import { taskIrSection } from "@/lib/taskV2";
import { parseTaskBlob } from "@/lib/taskBlob";

export interface FetchCall {
  readonly url: string;
  readonly method: string;
  readonly body?: string;
  readonly binary?: Uint8Array;
}

export const EMPTY_LIST_PAYLOAD = storedListPayload([], "");

export const TWO_RECORD_BLOB = [
  "# linkr-task.v1",
  "# task t1",
  '{"method":"PUT","path":"/api/v1/power/5v_out","body":"{\\"state\\":\\"off\\"}"}',
  '{"method":"PUT","path":"/api/v1/gpio/CON_MAS","body":"{\\"direction\\":\\"input\\"}"}',
  "",
].join("\n");

export function storedListPayload(tasks: readonly { id: string; name: string; request_count: number }[], blob: string) {
  const digest = (value: string) => createHash("sha256").update(value).digest("hex");
  return {
    schema: "radxa-linkr-debugger.v1",
    ok: true,
    command: "tasks",
    action: "list",
    task_store_id: "12345678-1234-4234-8234-123456789abc",
    revision: "1", catalog_fingerprint: "a".repeat(64), full_ir_bytes: new TextEncoder().encode(blob).length, full_ir_digest: digest(blob),
    tasks: tasks.map((task, slot) => {
      let section = blob;
      try { section = taskIrSection(blob, task.id); } catch { /* Deliberately malformed IR is exercised at read time. */ }
      return { ...task, slot, source_state: "absent", source_bytes: 0, source_digest: null, ir_bytes: new TextEncoder().encode(section).length, task_ir_digest: digest(section) };
    }),
    blob,
  };
}

// Mirrors the firmware flash catalog in linkr_debugger_task_catalog.c: six
// tasks with object bodies on the wire and typed cleanup records.
function firmwareCatalogTask(
  kind: "maskrom" | "edl",
  rail: "5v_out" | "12v_out" | "20v_out",
): Record<string, unknown> {
  const gpioInput = { direction: "input" };
  const gpioPath = "/api/v1/gpio/CON_MAS";
  const powerPath = `/api/v1/power/${rail}`;
  const request = (path: string, body: Record<string, unknown>, wait_ms: number) => ({
    method: "PUT",
    path,
    body,
    wait_ms,
  });
  return {
    id: `builtin/${kind}/${rail}`,
    name: `${kind === "edl" ? "Qualcomm EDL" : "Rockchip MASKROM"} via ${rail}`,
    requests: [
      request(gpioPath, gpioInput, 0),
      request(powerPath, { state: "off" }, 1000),
      request(gpioPath, { direction: "output", value: kind === "edl" ? 1 : 0 }, 20),
      request(powerPath, { state: "on" }, 500),
      request(gpioPath, gpioInput, 0),
    ],
    cleanup: request(gpioPath, gpioInput, 0),
  };
}

export function firmwareCatalogPayload(): Record<string, unknown> {
  const rails = ["5v_out", "12v_out", "20v_out"] as const;
  return {
    schema: "radxa-linkr-debugger.v1",
    ok: true,
    command: "task",
    action: "catalog",
    version: 1,
    tasks: [
      ...rails.map((rail) => firmwareCatalogTask("maskrom", rail)),
      ...rails.map((rail) => firmwareCatalogTask("edl", rail)),
    ],
  };
}

const calls: FetchCall[] = [];
const sources = new Map<string, string>();
let listPayload: unknown = EMPTY_LIST_PAYLOAD;
let catalogPayload: unknown = firmwareCatalogPayload();
let catalogStatus = 200;
const putFailures = new Map<string, { status: number; message: string; bodyIncludes?: string }>();
let deferredPut: {
  readonly promise: Promise<Response>;
  readonly resolve: (response: Response) => void;
} | null = null;

let lock = createAutomationTaskLock();
const taskControl: AutomationTaskControl = {
  get owner() {
    return lock.owner();
  },
  acquire: (owner) => lock.acquire(owner),
  release: (owner) => lock.release(owner),
};

let confirmResult = true;
const confirmMessages: string[] = [];

let root: Root | null = null;
let host: HTMLDivElement | null = null;

function jsonResponse(data: unknown, status = 200): Response {
  return new Response(JSON.stringify(data), {
    status,
    headers: { "Content-Type": "application/json" },
  });
}

function mockFetch(input: RequestInfo | URL, init?: RequestInit): Promise<Response> {
  const url = String(input);
  const method = init?.method ?? "GET";
  calls.push({
    url,
    method,
    body: typeof init?.body === "string" ? init.body : init?.body && ArrayBuffer.isView(init.body) ? new TextDecoder().decode(init.body as Uint8Array) : undefined,
    binary: init?.body && ArrayBuffer.isView(init.body) ? (init.body as Uint8Array) : undefined,
  });
  if (url === "/api/v1/tasks" && method === "GET") {
    return Promise.resolve(jsonResponse(listPayload));
  }
  if (url === "/api/v1/tasks/ir" && method === "GET") {
    return Promise.resolve(new Response((listPayload as { blob: string }).blob, { headers: { "Content-Type": "application/vnd.radxa.linkr-task-ir.v1" } }));
  }
  if (/\/api\/v1\/tasks\/slots\/[0-3]\/source$/.test(url)) {
    const slot = Number(url.split("/").at(-2));
    const task = (listPayload as ReturnType<typeof storedListPayload>).tasks.find((task) => task.slot === slot);
    const source = task && sources.get(task.id);
    return Promise.resolve(source ? new Response(source, { headers: { "Content-Type": "application/vnd.radxa.linkr-test.v1+ndjson" } }) : jsonResponse({ error: { code: "task_source_absent" } }, 404));
  }
  if (url === "/api/v1/tasks/mutations" && init?.body && ArrayBuffer.isView(init.body)) {
    const body = JSON.parse(new TextDecoder().decode(init.body as Uint8Array)) as {
      action: string;
      expected_revision: string;
      id?: string;
      source_base64?: string;
      ir_base64?: string;
    };
    const decode = (value: string) => Buffer.from(value, "base64").toString("utf8");
    const id = body.id ?? "";
    const source = body.source_base64 !== undefined ? decode(body.source_base64) : undefined;
    const ir = body.ir_base64 !== undefined ? decode(body.ir_base64) : "";
    const current = listPayload as ReturnType<typeof storedListPayload>;
    if (body.expected_revision !== current.revision) return Promise.resolve(jsonResponse({ error: { code: "revision_conflict", message: "Store revision changed" } }, 409));
    let blob = current.blob;
    if (body.action === "upsert") {
      if (source !== undefined) sources.set(id, source); else sources.delete(id);
      const sections = parseTaskBlob(blob).filter((task) => task.id !== id).map((task) => taskIrSection(blob, task.id).slice(16));
      blob = "# linkr-task.v1\n" + [...sections, taskIrSection(ir, id).slice(16)].join("");
    } else if (body.action === "delete") {
      const sections = parseTaskBlob(blob).filter((task) => task.id !== id).map((task) => taskIrSection(blob, task.id).slice(16));
      blob = sections.length ? "# linkr-task.v1\n" + sections.join("") : "";
    } else if (body.action === "clear") blob = "";
    else if (body.action === "import") blob = ir;
    const tasks = parseTaskBlob(blob);
    listPayload = { ...storedListPayload(tasks.map((task) => ({ id: task.id, name: task.id, request_count: task.requests.length })), blob), revision: String(BigInt(current.revision) + 1n) };
    attachSources();
    const next = listPayload as ReturnType<typeof storedListPayload>;
    return Promise.resolve(jsonResponse({ ...next, action: body.action, task: next.tasks.find((task) => task.id === id) ?? null }));
  }
  if (url === "/api/v1/tasks/catalog" && method === "GET") {
    return Promise.resolve(jsonResponse(catalogPayload, catalogStatus));
  }
  const failure = putFailures.get(`${method} ${url}`);
  if (failure && (failure.bodyIncludes === undefined || calls.at(-1)?.body?.includes(failure.bodyIncludes))) {
    return Promise.resolve(
      jsonResponse({ ok: false, error: { code: "rejected", message: failure.message } }, failure.status),
    );
  }
  if (method === "PUT" && deferredPut != null) {
    const pending = deferredPut;
    deferredPut = null;
    return pending.promise;
  }
  return Promise.resolve(jsonResponse({ ok: true, command: "task", action: "set" }));
}

export function setupTaskCardHarness(): void {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  calls.length = 0;
  sources.clear();
  putFailures.clear();
  deferredPut = null;
  listPayload = EMPTY_LIST_PAYLOAD;
  catalogPayload = firmwareCatalogPayload();
  catalogStatus = 200;
  lock = createAutomationTaskLock();
  confirmResult = true;
  confirmMessages.length = 0;
  vi.stubGlobal("fetch", vi.fn(mockFetch));
  vi.stubGlobal("confirm", vi.fn((message?: string) => {
    confirmMessages.push(message ?? "");
    return confirmResult;
  }));
}

export function teardownTaskCardHarness(): void {
  act(() => {
    root?.unmount();
  });
  host?.remove();
  root = null;
  host = null;
  localStorage.clear();
  vi.unstubAllGlobals();
}

export function stubTaskList(payload: unknown): void {
  listPayload = payload;
}

function attachSources() {
  const payload = listPayload as ReturnType<typeof storedListPayload>;
  listPayload = { ...payload, tasks: payload.tasks.map((task) => {
    const source = sources.get(task.id);
    return source ? { ...task, source_state: "present", source_bytes: new TextEncoder().encode(source).length, source_digest: createHash("sha256").update(source).digest("hex") } : task;
  }) };
}
export function stubTaskSource(id: string, script: TestScript): void { sources.set(id, serializeTestScript(script)); attachSources(); }

export function stubTaskCatalog(payload: unknown, status = 200): void {
  catalogPayload = payload;
  catalogStatus = status;
}

export function stubPutFailure(url: string, status: number, message: string, bodyIncludes?: string): void {
  putFailures.set(`PUT ${url}`, { status, message, bodyIncludes });
}

export function deferNextPutResponse(): () => void {
  if (deferredPut != null) throw new Error("a PUT response is already deferred");
  let resolve = (_response: Response): void => undefined;
  const promise = new Promise<Response>((done) => {
    resolve = done;
  });
  const pending = { promise, resolve };
  deferredPut = pending;
  return () => pending.resolve(jsonResponse({ ok: true, command: "task", action: "set" }));
}

export function stubConfirmResult(result: boolean): void {
  confirmResult = result;
}

export const confirmCalls = (): readonly string[] => confirmMessages;

export function holdAutomationLock(owner: AutomationTaskOwner): void {
  if (!lock.acquire(owner)) throw new Error(`automation lock already held by ${lock.owner() ?? "unknown"}`);
}

export const automationLockOwner = (): AutomationTaskOwner | null => lock.owner();

export const fetchCalls = (): readonly FetchCall[] => calls;
export const putCalls = (): FetchCall[] => calls.filter((call) => call.method === "PUT");
export const taskListGets = (): FetchCall[] =>
  calls.filter((call) => call.url === "/api/v1/tasks" && call.method === "GET");

export async function flush(): Promise<void> {
  await act(async () => {
    await new Promise((resolve) => setTimeout(resolve, 0));
  });
}

// Built-in sequences carry real wait_ms delays, so their runs complete over
// wall-clock time; poll with small bounded steps instead of a fixed sleep.
export async function flushUntil(predicate: () => boolean, timeoutMs = 5000): Promise<void> {
  const start = Date.now();
  while (!predicate()) {
    if (Date.now() - start > timeoutMs) throw new Error("timed out waiting for run progress");
    await act(async () => {
      await new Promise((resolve) => setTimeout(resolve, 25));
    });
  }
  await flush();
}

export async function mountTaskCard(connected = true, options: { script?: TestScript; onOpenWorkflow?: (script: TestScript) => void } = {}): Promise<HTMLDivElement> {
  localStorage.setItem("lang", "en");
  host = document.createElement("div");
  document.body.append(host);
  const created = createRoot(host);
  root = created;
  act(() => {
    created.render(
      <LanguageProvider>
        <TaskCard
          connected={connected}
          currentScript={options.script ?? defaultScript()}
          taskControl={taskControl}
          onOpenWorkflow={options.onOpenWorkflow}
        />
      </LanguageProvider>
    );
  });
  await flush();
  return host;
}

export function unmountTaskCard(): void {
  act(() => {
    root?.unmount();
  });
  root = null;
  host?.remove();
  host = null;
}

export function rowFor(view: HTMLElement, taskName: string): HTMLElement {
  const row = [...view.querySelectorAll<HTMLElement>("div.rounded-lg")].find((candidate) =>
    candidate.textContent?.includes(taskName),
  );
  if (!row) throw new TypeError(`row not found for ${taskName}`);
  return row;
}

export function storeButton(view: HTMLElement): HTMLButtonElement {
  const found = [...view.querySelectorAll("button")].find(
    (candidate) => candidate.textContent?.trim() === "Store current workflow",
  );
  if (!found) throw new TypeError("Store current workflow button not found");
  return found;
}

export function runButton(view: HTMLElement, taskName: string): HTMLButtonElement {
  const row = rowFor(view, taskName);
  const found = [...row.querySelectorAll("button")].find(
    (candidate) => candidate.textContent?.trim() === "Run task",
  );
  if (!found) throw new TypeError(`Run button not found for ${taskName}`);
  return found;
}

export function cancelButton(view: HTMLElement, taskName: string): HTMLButtonElement {
  const row = rowFor(view, taskName);
  const found = [...row.querySelectorAll("button")].find(
    (candidate) => candidate.textContent?.trim() === "Cancel",
  );
  if (!found) throw new TypeError(`Cancel button not found for ${taskName}`);
  return found;
}

export function deleteButton(view: HTMLElement, taskName: string): HTMLButtonElement {
  const row = rowFor(view, taskName);
  const found = [...row.querySelectorAll("button")].find(
    (candidate) => candidate.textContent?.trim() === "Delete",
  );
  if (!found) throw new TypeError(`Delete button not found for ${taskName}`);
  return found;
}

export function catalogRowIds(view: HTMLElement): (string | undefined)[] {
  return [...view.querySelectorAll<HTMLElement>("div.rounded-lg")].map(
    (row) => row.querySelector(".font-mono")?.textContent?.split(" ")[0],
  );
}
