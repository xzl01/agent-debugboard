import { parseTaskBlob, taskByteLength, taskIdValid } from "./taskBlob.ts";
import { buildTaskBlob } from "./taskRequests.ts";
import { parseTestScript, serializeTestScript, type TestScript } from "./testScript.ts";
import { sha256Hex } from "./sha256.ts";

export const TASK_SOURCE_MAX_BYTES = 4000;
export const TASK_IR_MAX_BYTES = 4000;
export const TASK_MUTATION_MAX_BYTES = 12288;
export const TASK_MUTATION_CONTENT_TYPE = "application/json";
export const TASK_MUTATION_SCHEMA = "radxa-linkr-debugger.v1";
export const TASK_IR_CONTENT_TYPE = "application/vnd.radxa.linkr-task-ir.v1";
export const TASK_SOURCE_CONTENT_TYPE = "application/vnd.radxa.linkr-test.v1+ndjson";
const encoder = new TextEncoder();
const decoder = new TextDecoder("utf-8", { fatal: true });
const UINT64_MAX = (1n << 64n) - 1n;
const BASE64_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/** Canonical RFC 4648 base64 with padding and zero pad bits. */
export function taskBase64(bytes: Uint8Array): string {
  let out = "";
  for (let i = 0; i < bytes.length; i += 3) {
    const remain = bytes.length - i;
    const chunk = (bytes[i] << 16) | ((remain > 1 ? bytes[i + 1] : 0) << 8) | (remain > 2 ? bytes[i + 2] : 0);
    out += BASE64_ALPHABET[(chunk >> 18) & 63];
    out += BASE64_ALPHABET[(chunk >> 12) & 63];
    out += remain > 1 ? BASE64_ALPHABET[(chunk >> 6) & 63] : "=";
    out += remain > 2 ? BASE64_ALPHABET[chunk & 63] : "=";
  }
  return out;
}

export const TASK_STORE_ERROR_CODES = ["revision_conflict", "catalog_conflict", "invalid_mutation", "invalid_task", "body_too_large", "task_source_absent", "busy", "storage_full", "readback_failed", "storage_error"] as const;
export type TaskV2ErrorCode = typeof TASK_STORE_ERROR_CODES[number] | "invalid_response" | "digest_mismatch" | "source_ir_mismatch" | "network_error";
export class TaskV2Error extends Error {
  readonly code: TaskV2ErrorCode;
  readonly response?: unknown;
  readonly status?: number;
  constructor(code: TaskV2ErrorCode, message: string, response?: unknown, status?: number) {
    super(message);
    this.name = "TaskV2Error";
    this.code = code; this.response = response; this.status = status;
  }
}
function invalid(message: string): never { throw new TaskV2Error("invalid_response", message); }
function record(value: unknown): Record<string, unknown> {
  if (!value || typeof value !== "object" || Array.isArray(value)) invalid("Expected object");
  return value as Record<string, unknown>;
}
export function taskRevision(value: unknown): bigint {
  if (typeof value !== "string" || !/^(0|[1-9][0-9]*)$/.test(value) || value.length > 20) invalid("Revision must be a uint64 decimal string");
  const revision = BigInt(value);
  if (revision > UINT64_MAX) invalid("Revision exceeds uint64");
  return revision;
}
export function taskRevisionString(value: bigint): string {
  if (typeof value !== "bigint" || value < 0n || value > UINT64_MAX) invalid("Revision exceeds uint64");
  return value.toString(10);
}
export function taskHexBytes(value: unknown): Uint8Array<ArrayBuffer> {
  if (typeof value !== "string" || !/^[0-9a-f]{64}$/.test(value)) invalid("Expected 32-byte lowercase hex digest");
  return Uint8Array.from(value.match(/../g)!, (byte) => parseInt(byte, 16));
}
export function taskBytesHex(bytes: Uint8Array): string {
  return Array.from(bytes, (byte) => byte.toString(16).padStart(2, "0")).join("");
}
export async function taskSha256(bytes: Uint8Array | string): Promise<string> {
  const input = typeof bytes === "string" ? encoder.encode(bytes) : Uint8Array.from(bytes);
  return sha256Hex(input);
}
export function validTaskStoreId(value: unknown): value is string {
  return typeof value === "string" && /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/.test(value);
}
export interface TaskSlotMetadata {
  readonly slot: number;
  readonly id: string;
  readonly request_count: number;
  readonly source_state: "present" | "absent";
  readonly source_bytes: number;
  readonly source_digest: string | null;
  readonly ir_bytes: number;
  readonly task_ir_digest: string;
}
export interface TaskStorageMetadata {
  readonly total_bytes: number;
  readonly used_bytes: number;
  readonly free_bytes: number;
  readonly task_bytes: number;
  readonly reserve_bytes: number;
  readonly measured: boolean;
  readonly save_allowed: boolean;
}
export interface TaskStoreMetadata {
  readonly task_store_id: string;
  readonly revision: string;
  readonly catalog_fingerprint: string;
  readonly full_ir_bytes: number;
  readonly full_ir_digest: string;
  readonly storage?: TaskStorageMetadata;
  readonly tasks: readonly TaskSlotMetadata[];
}
function size(value: unknown, max: number): number {
  if (typeof value !== "number" || !Number.isSafeInteger(value) || value < 0 || value > max) invalid("Invalid metadata count/length");
  return value;
}
function digest(value: unknown): string { taskHexBytes(value); return value as string; }
function identity(data: Record<string, unknown>) {
  if (!validTaskStoreId(data.task_store_id)) invalid("Invalid task store identity");
  taskRevision(data.revision);
  return { task_store_id: data.task_store_id, revision: data.revision as string, catalog_fingerprint: digest(data.catalog_fingerprint) };
}
export function parseTaskSlotMetadata(value: unknown): TaskSlotMetadata {
  const data = record(value);
  if (typeof data.id !== "string" || !taskIdValid(data.id)) invalid("Invalid task ID");
  if (data.source_state !== "present" && data.source_state !== "absent") invalid("Invalid source state");
  const sourceBytes = size(data.source_bytes, TASK_SOURCE_MAX_BYTES);
  if (data.source_state === "absent" && (sourceBytes !== 0 || data.source_digest !== null)) invalid("IR-only task cannot claim source");
  if (data.source_state === "present" && sourceBytes === 0) invalid("Source cannot be empty");
  return { slot: size(data.slot, 3), id: data.id, request_count: size(data.request_count, 32), source_state: data.source_state,
    source_bytes: sourceBytes, source_digest: data.source_state === "present" ? digest(data.source_digest) : null,
    ir_bytes: size(data.ir_bytes, TASK_IR_MAX_BYTES), task_ir_digest: digest(data.task_ir_digest) };
}
function envelope(value: unknown, action: string): Record<string, unknown> {
  const data = record(value);
  if (data.schema !== "radxa-linkr-debugger.v1" || data.ok !== true || data.command !== "tasks" || data.action !== action) invalid("Invalid tasks envelope");
  return data;
}
function parseStorage(value: unknown): TaskStorageMetadata | undefined {
  if (value === undefined) return undefined;
  const data = record(value);
  const total = size(data.total_bytes, 1 << 24);
  const used = size(data.used_bytes, 1 << 24);
  const free = size(data.free_bytes, 1 << 24);
  const task = size(data.task_bytes, 1 << 24);
  const reserve = size(data.reserve_bytes, 1 << 24);
  if (typeof data.measured !== "boolean" || typeof data.save_allowed !== "boolean") invalid("Invalid storage gate");
  if (used + free !== total || task > total) invalid("Invalid storage accounting");
  return { total_bytes: total, used_bytes: used, free_bytes: free, task_bytes: task, reserve_bytes: reserve, measured: data.measured, save_allowed: data.save_allowed };
}

export function parseTaskStoreMetadata(value: unknown): TaskStoreMetadata {
  const data = envelope(value, "list");
  if (!Array.isArray(data.tasks) || data.tasks.length > 4) invalid("Invalid task slots");
  const tasks = data.tasks.map(parseTaskSlotMetadata);
  if (new Set(tasks.map((task) => task.slot)).size !== tasks.length || new Set(tasks.map((task) => task.id)).size !== tasks.length) invalid("Duplicate task slots/IDs");
  const storage = parseStorage(data.storage);
  return { ...identity(data), full_ir_bytes: size(data.full_ir_bytes, TASK_IR_MAX_BYTES), full_ir_digest: digest(data.full_ir_digest), ...(storage ? { storage } : {}), tasks };
}

export type TaskMutation = {
  readonly expectedRevision: string | bigint;
  readonly catalogFingerprint: string;
} & (
  | { readonly action: "upsert"; readonly id: string; readonly source?: string; readonly ir: string }
  | { readonly action: "delete"; readonly id: string }
  | { readonly action: "clear" }
  | { readonly action: "import"; readonly ir: string }
);
/**
 * Stable text/JSON mutation contract: a UTF-8 JSON object with the string
 * fields schema, action, expected_revision, catalog_fingerprint and the
 * action-scoped optional fields id, source_base64 and ir_base64. The base64
 * fields are strict canonical RFC 4648 strings carrying exact source/IR
 * bytes. Field order is stable.
 */
export function encodeTaskMutation(mutation: TaskMutation): Uint8Array<ArrayBuffer> {
  const revision = typeof mutation.expectedRevision === "bigint" ? taskRevision(taskRevisionString(mutation.expectedRevision)) : taskRevision(mutation.expectedRevision);
  taskHexBytes(mutation.catalogFingerprint);
  const id = "id" in mutation ? mutation.id : "";
  if ("id" in mutation && !taskIdValid(id)) throw new TaskV2Error("invalid_mutation", "Invalid task ID");
  const source = mutation.action === "upsert" ? mutation.source : undefined;
  const ir = "ir" in mutation ? mutation.ir : "";
  const sourceBytes = encoder.encode(source ?? ""), irBytes = encoder.encode(ir);
  if (sourceBytes.length > TASK_SOURCE_MAX_BYTES || irBytes.length > TASK_IR_MAX_BYTES) throw new TaskV2Error("body_too_large", "Task source/IR exceeds 4000 bytes");
  if (source !== undefined && (!sourceBytes.length || /[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/.test(source))) throw new TaskV2Error("invalid_task", "Invalid source control bytes");
  if (ir) {
    try {
      const tasks = parseTaskBlob(ir);
      if (mutation.action === "upsert" && (tasks.length !== 1 || tasks[0].id !== id)) throw new Error("Upsert IR ID differs");
    } catch (cause) { throw new TaskV2Error("invalid_task", String(cause)); }
  } else if (mutation.action === "upsert" || mutation.action === "import") throw new TaskV2Error("invalid_task", "Task IR is required");
  const body: Record<string, string> = {
    schema: TASK_MUTATION_SCHEMA,
    action: mutation.action,
    expected_revision: taskRevisionString(revision),
    catalog_fingerprint: mutation.catalogFingerprint,
  };
  if ("id" in mutation) body.id = id;
  if (source !== undefined) body.source_base64 = taskBase64(sourceBytes);
  if (ir) body.ir_base64 = taskBase64(irBytes);
  const bytes = encoder.encode(JSON.stringify(body));
  if (bytes.length > TASK_MUTATION_MAX_BYTES) throw new TaskV2Error("body_too_large", "Task mutation exceeds 12288 bytes");
  return bytes;
}

/** Exact ASCII marker + exact section through its final request LF (no reserialization). */
export function taskIrSection(fullIr: string, id: string): string {
  const tasks = parseTaskBlob(fullIr);
  if (!tasks.some((task) => task.id === id)) throw new TaskV2Error("invalid_task", "Task not found in IR");
  if (!fullIr.startsWith("# linkr-task.v1\n") || fullIr.includes("\r")) throw new TaskV2Error("invalid_task", "IR must use canonical LF framing");
  const start = fullIr.indexOf(`# task ${id}\n`, 15);
  if (start < 0 || (start > 0 && fullIr[start - 1] !== "\n")) throw new TaskV2Error("invalid_task", "Invalid task framing");
  const next = fullIr.indexOf("\n# task ", start);
  const section = fullIr.slice(start, next < 0 ? undefined : next + 1);
  const lines = section.split("\n");
  let finalRequest = -1;
  for (let i = 1; i < lines.length; i++) if (lines[i] && !lines[i].startsWith("#")) finalRequest = i;
  const end = finalRequest < 0 ? 0 : finalRequest;
  if (!section.endsWith("\n")) throw new TaskV2Error("invalid_task", "Missing final LF");
  return `# linkr-task.v1\n${lines.slice(0, end + 1).join("\n")}\n`;
}
export interface CompiledTaskSource {
  readonly script: TestScript;
  readonly source: string;
  readonly source_digest: string;
  readonly ir: string;
  readonly task_ir_digest: string;
  readonly local_only: boolean;
}
export async function compileTaskSource(id: string, input: string | TestScript): Promise<CompiledTaskSource> {
  if (!taskIdValid(id)) throw new TaskV2Error("invalid_task", "Invalid task ID");
  try {
    const script = parseTestScript(typeof input === "string" ? input : serializeTestScript(input));
    const source = serializeTestScript(script);
    const ir = buildTaskBlob(id, script);
    parseTaskBlob(ir);
    return { script, source, ir, source_digest: await taskSha256(source), task_ir_digest: await taskSha256(taskIrSection(ir, id)), local_only: taskByteLength(source) > TASK_SOURCE_MAX_BYTES };
  } catch (cause) {
    if (cause instanceof TaskV2Error) throw cause;
    throw new TaskV2Error("invalid_task", cause instanceof Error ? cause.message : String(cause));
  }
}
export interface TaskMutationResult {
  readonly task_store_id: string;
  readonly revision: string;
  readonly catalog_fingerprint: string;
  readonly action: TaskMutation["action"];
  readonly task: TaskSlotMetadata | null;
}
export interface TaskSnapshot { readonly metadata: TaskStoreMetadata; readonly blob: string }
export interface TaskSourceSnapshot { readonly metadata: TaskStoreMetadata; readonly task: TaskSlotMetadata; readonly source: string; readonly script: TestScript }

export function createTaskV2Client(base = "/api/v1", fetcher: typeof fetch = (...args) => fetch(...args)) {
  async function response(path: string, init?: RequestInit): Promise<Response> {
    let res: Response;
    try { res = await fetcher(base + path, init); }
    catch (cause) { if (init?.signal?.aborted) throw cause; throw new TaskV2Error("network_error", String(cause)); }
    if (!res.ok) {
      const data: unknown = await res.json().catch(() => null);
      const body = data && typeof data === "object" ? data as Record<string, unknown> : {};
      const error = body.error && typeof body.error === "object" ? body.error as Record<string, unknown> : body;
      const code = TASK_STORE_ERROR_CODES.find((code) => code === error.code) ?? "invalid_response";
      throw new TaskV2Error(code, typeof error.message === "string" ? error.message : `HTTP ${res.status}`, data, res.status);
    }
    return res;
  }
  async function json(path: string, init?: RequestInit): Promise<unknown> {
    const res = await response(path, init);
    try { return await res.json(); } catch { return invalid("Invalid JSON response"); }
  }
  async function list(signal?: AbortSignal): Promise<TaskStoreMetadata> { return parseTaskStoreMetadata(await json("/tasks", { signal })); }
  async function raw(path: string, contentType: string, signal?: AbortSignal): Promise<Uint8Array> {
    const res = await response(path, { signal });
    if (res.headers.get("content-type")?.split(";")[0] !== contentType) invalid("Unexpected task content type");
    return new Uint8Array(await res.arrayBuffer());
  }
  async function readIr(signal?: AbortSignal): Promise<TaskSnapshot> {
    for (let attempt = 0; attempt < 2; attempt++) {
      const metadata = await list(signal);
      const bytes = await raw("/tasks/ir", TASK_IR_CONTENT_TYPE, signal);
      if (bytes.length !== metadata.full_ir_bytes || await taskSha256(bytes) !== metadata.full_ir_digest) continue;
      let blob: string;
      try { blob = decoder.decode(bytes); } catch { return invalid("IR is not UTF-8"); }
      const tasks = parseTaskBlob(blob);
      if (tasks.length !== metadata.tasks.length) invalid("IR task count differs from metadata");
      for (const task of metadata.tasks) {
        const section = taskIrSection(blob, task.id);
        if (taskByteLength(section) !== task.ir_bytes || await taskSha256(section) !== task.task_ir_digest || tasks.find((candidate) => candidate.id === task.id)?.requests.length !== task.request_count) throw new TaskV2Error("digest_mismatch", "Task section differs from metadata");
      }
      return { metadata, blob };
    }
    throw new TaskV2Error("digest_mismatch", "Raw IR changed during both reads");
  }
  async function readSource(id: string, signal?: AbortSignal): Promise<TaskSourceSnapshot> {
    for (let attempt = 0; attempt < 2; attempt++) {
      const metadata = await list(signal);
      const task = metadata.tasks.find((task) => task.id === id);
      if (!task || task.source_state !== "present") throw new TaskV2Error("task_source_absent", "Task has no lossless workflow source");
      let bytes: Uint8Array;
      try { bytes = await raw(`/tasks/slots/${task.slot}/source`, TASK_SOURCE_CONTENT_TYPE, signal); }
      catch (cause) { if (cause instanceof TaskV2Error && cause.code === "task_source_absent" && attempt === 0) continue; throw cause; }
      if (bytes.length !== task.source_bytes || await taskSha256(bytes) !== task.source_digest) continue;
      let source: string;
      try { source = decoder.decode(bytes); } catch { return invalid("Source is not UTF-8"); }
      let compiled: CompiledTaskSource;
      try { compiled = await compileTaskSource(id, source); }
      catch (cause) { throw new TaskV2Error("source_ir_mismatch", `Stored source cannot compile: ${String(cause)}`); }
      if (compiled.source !== source || compiled.task_ir_digest !== task.task_ir_digest) throw new TaskV2Error("source_ir_mismatch", "Exact source cannot reproduce deployed task IR");
      return { metadata, task, source, script: compiled.script };
    }
    throw new TaskV2Error("digest_mismatch", "Source changed during both reads");
  }
  async function mutate(mutation: TaskMutation, signal?: AbortSignal): Promise<TaskMutationResult> {
    // Never send a source/IR association without independently compiling locally.
    if (mutation.action === "upsert" && mutation.source !== undefined) {
      const compiled = await compileTaskSource(mutation.id, mutation.source);
      if (compiled.source !== mutation.source || compiled.task_ir_digest !== await taskSha256(taskIrSection(mutation.ir, mutation.id))) throw new TaskV2Error("source_ir_mismatch", "Source/IR digest differs before upload");
    }
    const data = envelope(await json("/tasks/mutations", { method: "POST", headers: { "Content-Type": TASK_MUTATION_CONTENT_TYPE }, body: encodeTaskMutation(mutation), signal }), mutation.action);
    return { ...identity(data), action: mutation.action, task: data.task === null ? null : parseTaskSlotMetadata(data.task) };
  }
  async function upsertSource(metadata: TaskStoreMetadata, id: string, input: string | TestScript, signal?: AbortSignal) {
    const compiled = await compileTaskSource(id, input);
    const result = await mutate({ action: "upsert", id, ir: compiled.ir, ...(compiled.local_only ? {} : { source: compiled.source }), expectedRevision: metadata.revision, catalogFingerprint: metadata.catalog_fingerprint }, signal);
    return { ...compiled, result };
  }
  return { list, readIr, readSource, mutate, upsertSource };
}

/** Workflow and raw-IR files intentionally have independent import/export APIs. */
export function importWorkflowSource(source: string): TestScript { return parseTestScript(source); }
export function exportWorkflowSource(script: TestScript): string { return serializeTestScript(script); }
export function importRawTaskIr(ir: string): string { parseTaskBlob(ir); return ir; }
export function exportRawTaskIr(ir: string): string { return importRawTaskIr(ir); }
