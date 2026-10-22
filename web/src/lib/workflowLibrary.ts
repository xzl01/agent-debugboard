import { parseTestScript, serializeTestScript, type TestScript } from "./testScript.ts";
import { taskByteLength, taskIdValid } from "./taskBlob.ts";
import { compileTaskSource, TASK_SOURCE_MAX_BYTES, taskHexBytes, taskRevision, taskSha256, validTaskStoreId, type TaskSlotMetadata, type TaskStoreMetadata } from "./taskV2.ts";

export const WORKFLOW_DATABASE_NAME = "radxa-linkr-debugger-workflows";
export const WORKFLOW_DATABASE_VERSION = 2;
export type WorkflowStorageErrorCode = "storage_unavailable" | "storage_quota" | "storage_blocked" | "storage_error" | "storage_corrupt" | "digest_mismatch" | "identity_mismatch";
export class WorkflowStorageError extends Error {
  readonly code: WorkflowStorageErrorCode;
  readonly cause?: unknown;
  constructor(code: WorkflowStorageErrorCode, message: string, cause?: unknown) { super(message); this.name = "WorkflowStorageError"; this.code = code; this.cause = cause; }
}
export interface WorkflowRecord {
  readonly workflow_id: string;
  readonly name: string;
  readonly source: string;
  readonly source_digest: string;
  readonly baseline: string | null;
  readonly created_at: string;
  readonly updated_at: string;
  readonly local_only: boolean;
}
export interface DeploymentRecord {
  readonly task_store_id: string;
  readonly task_id: string;
  readonly workflow_id: string | null;
  readonly source_digest: string | null;
  readonly task_ir_digest: string;
  readonly catalog_fingerprint: string;
  readonly revision: string;
  readonly source_state: "board" | "local-only" | "ir-only";
}
export interface WorkflowDraft {
  readonly workflow_id?: string;
  readonly source: string | TestScript;
  readonly baseline?: string | null;
}
function storageError(cause: unknown): WorkflowStorageError {
  if (cause instanceof WorkflowStorageError) return cause;
  const name = cause && typeof cause === "object" && "name" in cause ? String(cause.name) : "";
  return new WorkflowStorageError(name === "QuotaExceededError" ? "storage_quota" : ["SecurityError", "NotAllowedError", "InvalidStateError"].includes(name) ? "storage_unavailable" : "storage_error", `Workflow storage failed: ${String(cause)}`, cause);
}
function corrupt(message: string): never { throw new WorkflowStorageError("storage_corrupt", message); }
function asRecord(value: unknown): Record<string, unknown> {
  if (!value || typeof value !== "object" || Array.isArray(value)) corrupt("Invalid workflow record");
  return value as Record<string, unknown>;
}
function timestamp(value: unknown): value is string { return typeof value === "string" && Number.isFinite(Date.parse(value)); }
async function validateWorkflow(value: unknown): Promise<WorkflowRecord> {
  const row = asRecord(value);
  if (typeof row.workflow_id !== "string" || !row.workflow_id || typeof row.source !== "string" || typeof row.name !== "string" || !timestamp(row.created_at) || !timestamp(row.updated_at) || typeof row.local_only !== "boolean" || (row.baseline !== null && typeof row.baseline !== "string")) corrupt("Malformed workflow fields");
  try {
    const script = parseTestScript(row.source);
    if (serializeTestScript(script) !== row.source || script.name !== row.name || row.local_only !== (taskByteLength(row.source) > TASK_SOURCE_MAX_BYTES)) corrupt("Noncanonical workflow record");
    if (row.baseline !== null && serializeTestScript(parseTestScript(row.baseline as string)) !== row.baseline) corrupt("Noncanonical workflow baseline");
  } catch (cause) { if (cause instanceof WorkflowStorageError) throw cause; corrupt(`Invalid workflow source: ${String(cause)}`); }
  if (await taskSha256(row.source as string) !== row.source_digest) throw new WorkflowStorageError("digest_mismatch", "Local workflow source digest differs");
  return row as unknown as WorkflowRecord;
}
function validateDeployment(value: unknown): DeploymentRecord {
  const row = asRecord(value);
  if (!validTaskStoreId(row.task_store_id) || typeof row.task_id !== "string" || !taskIdValid(row.task_id) || (row.workflow_id !== null && (typeof row.workflow_id !== "string" || !row.workflow_id))) corrupt("Invalid deployment identity");
  try { taskHexBytes(row.task_ir_digest); taskHexBytes(row.catalog_fingerprint); taskRevision(row.revision); if (row.source_digest !== null) taskHexBytes(row.source_digest); }
  catch { corrupt("Invalid deployment digest/revision"); }
  if (!["board", "local-only", "ir-only"].includes(row.source_state as string)) corrupt("Invalid deployment source state");
  if (row.source_state === "ir-only" ? row.workflow_id !== null || row.source_digest !== null : row.workflow_id === null || row.source_digest === null) corrupt("Deployment source association differs");
  return row as unknown as DeploymentRecord;
}
function requestResult<T>(request: IDBRequest<T>): Promise<T> {
  return new Promise((resolve, reject) => { request.onsuccess = () => resolve(request.result); request.onerror = () => reject(request.error); });
}
function transactionDone(transaction: IDBTransaction): Promise<void> {
  return new Promise((resolve, reject) => { transaction.oncomplete = () => resolve(); transaction.onabort = () => reject(transaction.error ?? new Error("Transaction aborted")); transaction.onerror = () => { /* onabort reports the final transaction outcome */ }; });
}

/** Injecting IDBFactory/name keeps failure, migration and multi-device tests offline. */
export function createWorkflowLibrary(options: { indexedDB?: IDBFactory | null; name?: string; now?: () => string; newId?: () => string } = {}) {
  const now = options.now ?? (() => new Date().toISOString());
  const newId = options.newId ?? (() => {
    if (globalThis.crypto?.randomUUID) return globalThis.crypto.randomUUID();
    const bytes = new Uint8Array(16);
    if (globalThis.crypto?.getRandomValues) globalThis.crypto.getRandomValues(bytes);
    else throw new WorkflowStorageError("storage_unavailable", "Secure random workflow identity is unavailable");
    bytes[6] = (bytes[6] & 15) | 64; bytes[8] = (bytes[8] & 63) | 128;
    const hex = Array.from(bytes, (byte) => byte.toString(16).padStart(2, "0")).join("");
    return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
  });
  let opening: Promise<IDBDatabase> | null = null;
  function open(): Promise<IDBDatabase> {
    const factory = options.indexedDB === undefined ? globalThis.indexedDB : options.indexedDB;
    if (!factory) return Promise.reject(new WorkflowStorageError("storage_unavailable", "IndexedDB is unavailable; workflow was not saved"));
    if (opening) return opening;
    opening = new Promise<IDBDatabase>((resolve, reject) => {
      let blocked = false;
      const req = factory.open(options.name ?? WORKFLOW_DATABASE_NAME, WORKFLOW_DATABASE_VERSION);
      req.onupgradeneeded = () => {
        const db = req.result;
        // v1 owns workflows; v2 adds compound device/task deployment identity.
        if (!db.objectStoreNames.contains("workflows")) db.createObjectStore("workflows", { keyPath: "workflow_id" });
        if (!db.objectStoreNames.contains("deployments")) db.createObjectStore("deployments", { keyPath: ["task_store_id", "task_id"] });
        const workflows = req.transaction!.objectStore("workflows");
        const deployments = req.transaction!.objectStore("deployments");
        if (workflows.keyPath !== "workflow_id" || JSON.stringify(deployments.keyPath) !== JSON.stringify(["task_store_id", "task_id"])) req.transaction!.abort();
      };
      req.onsuccess = () => {
        if (blocked) { req.result.close(); return; }
        const db = req.result;
        try {
          if (!db.objectStoreNames.contains("workflows") || !db.objectStoreNames.contains("deployments")) corrupt("Workflow database stores are missing");
          const tx = db.transaction(["workflows", "deployments"], "readonly");
          if (tx.objectStore("workflows").keyPath !== "workflow_id" || JSON.stringify(tx.objectStore("deployments").keyPath) !== JSON.stringify(["task_store_id", "task_id"])) corrupt("Workflow database identity keys differ");
        } catch (cause) { db.close(); reject(storageError(cause)); return; }
        db.onversionchange = () => { db.close(); opening = null; };
        resolve(db);
      };
      req.onerror = () => { opening = null; reject(storageError(req.error)); };
      req.onblocked = () => { blocked = true; opening = null; reject(new WorkflowStorageError("storage_blocked", "Close other workflow-library tabs to upgrade storage")); };
    }).catch((cause) => { opening = null; throw storageError(cause); });
    return opening;
  }
  async function transaction<T>(stores: string[], mode: IDBTransactionMode, run: (tx: IDBTransaction) => Promise<T>): Promise<T> {
    try {
      const tx = (await open()).transaction(stores, mode);
      const done = transactionDone(tx);
      try { const result = await run(tx); await done; return result; }
      catch (cause) { try { tx.abort(); } catch { /* already aborted/complete */ } await done.catch(() => {}); throw cause; }
    } catch (cause) { throw storageError(cause); }
  }
  async function prepare(draft: WorkflowDraft, existing?: WorkflowRecord): Promise<WorkflowRecord> {
    const script = parseTestScript(typeof draft.source === "string" ? draft.source : serializeTestScript(draft.source));
    const source = serializeTestScript(script);
    const baseline = draft.baseline === undefined ? existing?.baseline ?? null : draft.baseline === null ? null : serializeTestScript(parseTestScript(draft.baseline));
    return { workflow_id: draft.workflow_id ?? newId(), source, source_digest: await taskSha256(source), name: script.name, baseline, created_at: existing?.created_at ?? now(), updated_at: now(), local_only: taskByteLength(source) > TASK_SOURCE_MAX_BYTES };
  }
  async function getWorkflow(id: string): Promise<WorkflowRecord | null> {
    const row = await transaction(["workflows"], "readonly", (tx) => requestResult(tx.objectStore("workflows").get(id)));
    return row === undefined ? null : validateWorkflow(row);
  }
  async function listWorkflows(): Promise<readonly WorkflowRecord[]> {
    const rows = await transaction(["workflows"], "readonly", (tx) => requestResult(tx.objectStore("workflows").getAll()));
    return Promise.all(rows.map(validateWorkflow));
  }
  async function saveWorkflow(draft: WorkflowDraft): Promise<WorkflowRecord> {
    const existing = draft.workflow_id ? await getWorkflow(draft.workflow_id) : undefined;
    const workflow = await prepare(draft, existing ?? undefined);
    await transaction(["workflows"], "readwrite", async (tx) => { await requestResult(tx.objectStore("workflows").put(workflow)); });
    return workflow;
  }
  async function getDeployment(storeId: string, taskId: string): Promise<DeploymentRecord | null> {
    if (!validTaskStoreId(storeId) || !taskIdValid(taskId)) throw new WorkflowStorageError("identity_mismatch", "A deployment needs an exact device-store/task identity");
    const row = await transaction(["deployments"], "readonly", (tx) => requestResult(tx.objectStore("deployments").get([storeId, taskId])));
    return row === undefined ? null : validateDeployment(row);
  }
  async function listDeployments(storeId?: string): Promise<readonly DeploymentRecord[]> {
    if (storeId !== undefined && !validTaskStoreId(storeId)) throw new WorkflowStorageError("identity_mismatch", "Invalid store identity");
    const rows = await transaction(["deployments"], "readonly", (tx) => requestResult(tx.objectStore("deployments").getAll()));
    return rows.map(validateDeployment).filter((row) => storeId === undefined || row.task_store_id === storeId);
  }
  async function saveDeployment(deployment: DeploymentRecord, workflow?: WorkflowRecord): Promise<void> {
    validateDeployment(deployment);
    const verified = workflow ? await validateWorkflow(workflow) : deployment.workflow_id ? await getWorkflow(deployment.workflow_id) : null;
    if (deployment.workflow_id) {
      if (!verified || verified.source_digest !== deployment.source_digest || verified.workflow_id !== deployment.workflow_id || (verified.local_only && deployment.source_state === "board")) throw new WorkflowStorageError("identity_mismatch", "Deployment workflow/digest does not match local source");
      const compiled = await compileTaskSource(deployment.task_id, verified.source);
      if (compiled.task_ir_digest !== deployment.task_ir_digest) throw new WorkflowStorageError("digest_mismatch", "Local workflow cannot reproduce deployment IR");
    }
    // Validate before starting IDB: digesting inside a transaction lets it auto-close.
    await transaction(["workflows", "deployments"], "readwrite", async (tx) => {
      const source = workflow ?? (deployment.workflow_id ? await requestResult(tx.objectStore("workflows").get(deployment.workflow_id)) as WorkflowRecord | undefined : undefined);
      if (deployment.workflow_id && (!source || source.workflow_id !== deployment.workflow_id || source.source_digest !== deployment.source_digest)) throw new WorkflowStorageError("identity_mismatch", "Deployment workflow/digest does not match local source");
      if (workflow) await requestResult(tx.objectStore("workflows").put(workflow));
      await requestResult(tx.objectStore("deployments").put(deployment));
    });
  }
  async function deleteWorkflow(id: string): Promise<void> {
    await transaction(["workflows", "deployments"], "readwrite", async (tx) => {
      const store = tx.objectStore("deployments");
      const rows = await requestResult(store.getAll());
      for (const row of rows) if (row.workflow_id === id) await requestResult(store.delete([row.task_store_id, row.task_id]));
      await requestResult(tx.objectStore("workflows").delete(id));
    });
  }
  async function deleteDeployment(storeId: string, taskId: string): Promise<void> {
    if (!validTaskStoreId(storeId) || !taskIdValid(taskId)) throw new WorkflowStorageError("identity_mismatch", "Invalid deployment identity");
    await transaction(["deployments"], "readwrite", async (tx) => { await requestResult(tx.objectStore("deployments").delete([storeId, taskId])); });
  }
  async function openDeployment(metadata: TaskStoreMetadata, taskId: string): Promise<WorkflowRecord | null> {
    const deployment = await getDeployment(metadata.task_store_id, taskId);
    if (!deployment || deployment.source_state === "ir-only") return null;
    const task = metadata.tasks.find((candidate) => candidate.id === taskId);
    if (!task || !deploymentMatches(deployment, metadata, task)) throw new WorkflowStorageError("identity_mismatch", "Deployment metadata no longer matches this device/task");
    const workflow = await getWorkflow(deployment.workflow_id!);
    if (!workflow || workflow.source_digest !== deployment.source_digest || (await compileTaskSource(taskId, workflow.source)).task_ir_digest !== task.task_ir_digest) throw new WorkflowStorageError("digest_mismatch", "Local source cannot reproduce deployed task IR");
    return workflow;
  }
  async function close(): Promise<void> { if (opening) (await opening).close(); opening = null; }
  return { getWorkflow, listWorkflows, saveWorkflow, getDeployment, listDeployments, saveDeployment, openDeployment, deleteWorkflow, deleteDeployment, close };
}

/** Never use task IDs alone to reopen local source on another physical device. */
export function deploymentMatches(deployment: DeploymentRecord, metadata: TaskStoreMetadata, task: TaskSlotMetadata): boolean {
  validateDeployment(deployment);
  return metadata.tasks.some((candidate) => candidate.id === task.id && candidate.slot === task.slot && candidate.task_ir_digest === task.task_ir_digest)
    && deployment.task_store_id === metadata.task_store_id && deployment.task_id === task.id && deployment.task_ir_digest === task.task_ir_digest
    // A sibling task mutation advances the store revision without changing this task.
    && deployment.catalog_fingerprint === metadata.catalog_fingerprint && taskRevision(metadata.revision) >= taskRevision(deployment.revision)
    && (deployment.source_state !== "board" || deployment.source_digest === task.source_digest);
}

export const workflowLibrary = createWorkflowLibrary();
