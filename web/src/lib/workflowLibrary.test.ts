import assert from "node:assert/strict";
import test from "node:test";
import { IDBFactory, IDBObjectStore } from "fake-indexeddb";
import { createWorkflowLibrary, deploymentMatches, WORKFLOW_DATABASE_VERSION, type DeploymentRecord } from "./workflowLibrary.ts";
import { compileTaskSource, type TaskStoreMetadata } from "./taskV2.ts";
import { serializeTestScript, serializeTestScriptDraft, type TestScript } from "./testScript.ts";

const storeA = "12345678-1234-4567-890a-123456789abc";
const storeB = "22345678-1234-4567-890a-123456789abc";
const time = "2026-10-22T00:00:00.000Z";
const source = (name = "A"): TestScript => ({ schema: "linkr-test.v1", name, version: "1.0", created: time, steps: [{ id: "a", type: "power_on", params: { rail: "5v_out" } }] });
const validTaskDigest = (await compileTaskSource("A", source())).task_ir_digest;
const deployment = (workflowId: string, sourceDigest: string, storeId = storeA): DeploymentRecord => ({ task_store_id: storeId, task_id: "A", workflow_id: workflowId, source_digest: sourceDigest, task_ir_digest: validTaskDigest, catalog_fingerprint: "2".repeat(64), revision: "9007199254740993", source_state: "board" });
function database(factory: IDBFactory, version?: number): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => { const req = factory.open("test", version); req.onsuccess = () => resolve(req.result); req.onerror = () => reject(req.error); req.onupgradeneeded = () => req.result.createObjectStore("workflows", { keyPath: "workflow_id" }); });
}
async function tamper(factory: IDBFactory, store: string, value: unknown) {
  const db = await database(factory);
  await new Promise<void>((resolve, reject) => { const tx = db.transaction(store, "readwrite"); tx.objectStore(store).put(value); tx.oncomplete = () => resolve(); tx.onabort = () => reject(tx.error); });
  db.close();
}

test("migrates v1 workflow schema to two stores and preserves A/B across reopen", async () => {
  const factory = new IDBFactory();
  const old = await database(factory, 1); old.close();
  const library = createWorkflowLibrary({ indexedDB: factory, name: "test", now: () => time, newId: () => "A" });
  const a = await library.saveWorkflow({ source: source(), baseline: serializeTestScript(source("baseline")) });
  const b = await library.saveWorkflow({ workflow_id: "B", source: source("B") });
  await library.close();
  assert.deepEqual(await library.getWorkflow("A"), a); assert.deepEqual(await library.getWorkflow("B"), b);
  const db = await database(factory);
  assert.equal(db.version, WORKFLOW_DATABASE_VERSION); assert.deepEqual(Array.from(db.objectStoreNames), ["deployments", "workflows"]); db.close();
  await library.close();
});

test("draft buffers, incomplete current ranges and duplicate NDJSON cannot overwrite canonical library source", async () => {
  const library = createWorkflowLibrary({ indexedDB: new IDBFactory(), now: () => time });
  const original = await library.saveWorkflow({ workflow_id: "A", source: source() });
  const incomplete = { ...source(), steps: [{ id: "delay", type: "delay" as const, params: { ms: -1 } }] };
  const draft = serializeTestScriptDraft(incomplete);
  const incompleteRange = { ...source(), steps: [{ id: "read", type: "adc_read" as const, params: { channel: "5v_out" }, assert: {
    current_range: {} as { min_a: number; max_a: number },
  } }] };
  const duplicate = '{"schema":"linkr-test.v1","name":"Review"}\n{"id":"on","type":"power_on","params":{"rail":"20v_out","rail":"5v_out"}}\n';
  for (const input of [draft, incomplete, duplicate, incompleteRange]) {
    await assert.rejects(library.saveWorkflow({ workflow_id: "A", source: input }));
    assert.deepEqual(await library.getWorkflow("A"), original);
  }
  assert.equal(serializeTestScriptDraft(incomplete), draft);
  await library.close();
});

test("one workflow deploys to multiple devices without task-ID aliasing or digest confusion", async () => {
  const library = createWorkflowLibrary({ indexedDB: new IDBFactory(), now: () => time });
  const workflow = await library.saveWorkflow({ workflow_id: "workflow", source: source() });
  const a = deployment(workflow.workflow_id, workflow.source_digest);
  const b = deployment(workflow.workflow_id, workflow.source_digest, storeB);
  await library.saveDeployment(a); await library.saveDeployment(b);
  assert.deepEqual(await library.getDeployment(storeA, "A"), a); assert.deepEqual(await library.getDeployment(storeB, "A"), b);
  assert.equal((await library.listDeployments(storeA)).length, 1);
  const task = { slot: 0, id: "A", request_count: 1, source_state: "present" as const, source_bytes: 100, source_digest: workflow.source_digest, ir_bytes: 100, task_ir_digest: a.task_ir_digest };
  const metadata: TaskStoreMetadata = { task_store_id: storeA, revision: a.revision, catalog_fingerprint: a.catalog_fingerprint, full_ir_bytes: 100, full_ir_digest: "0".repeat(64), tasks: [task] };
  assert.equal(deploymentMatches(a, metadata, task), true);
  assert.deepEqual(await library.openDeployment(metadata, "A"), workflow);
  for (const changed of [{ task_store_id: storeB }, { revision: "9007199254740992" }, { catalog_fingerprint: "3".repeat(64) }]) assert.equal(deploymentMatches(a, { ...metadata, ...changed }, task), false);
  assert.equal(deploymentMatches(a, { ...metadata, revision: "9007199254740994" }, task), true);
  assert.equal(deploymentMatches(a, metadata, { ...task, task_ir_digest: "4".repeat(64) }), false);
  await assert.rejects(library.saveDeployment({ ...a, source_digest: "f".repeat(64) }), { code: "identity_mismatch" });
  await assert.rejects(library.saveDeployment({ ...a, task_ir_digest: "f".repeat(64) }), { code: "digest_mismatch" });
  assert.deepEqual(await library.openDeployment({ ...metadata, revision: "9007199254740994" }, "A"), workflow);
  await assert.rejects(library.openDeployment({ ...metadata, revision: "9007199254740992" }, "A"), { code: "identity_mismatch" });
  await library.deleteWorkflow(workflow.workflow_id);
  assert.deepEqual(await library.listDeployments(), []); assert.equal(await library.getWorkflow(workflow.workflow_id), null);
  await library.close();
});

test("large source is locally saved/exportable, IR-only deployments never claim source", async () => {
  const library = createWorkflowLibrary({ indexedDB: new IDBFactory(), now: () => time });
  const big = await library.saveWorkflow({ workflow_id: "big", source: source("x".repeat(4100)) });
  assert.equal(big.local_only, true); assert.ok(big.source.length > 4000);
  const compiled = await compileTaskSource("A", big.source); assert.equal(compiled.local_only, true);
  await library.saveDeployment({ ...deployment(big.workflow_id, big.source_digest), source_state: "local-only" });
  const irOnly: DeploymentRecord = { ...deployment("unused", "1".repeat(64), storeB), workflow_id: null, source_digest: null, source_state: "ir-only" };
  await library.saveDeployment(irOnly); assert.deepEqual(await library.getDeployment(storeB, "A"), irOnly);
  await assert.rejects(library.saveDeployment({ ...irOnly, workflow_id: big.workflow_id }), { code: "storage_corrupt" });
  await library.close();
});

test("private/unavailable/quota errors are surfaced and failed cross-store save rolls back", async () => {
  await assert.rejects(createWorkflowLibrary({ indexedDB: null }).saveWorkflow({ source: source() }), { code: "storage_unavailable" });
  const denied = { open() { throw new DOMException("private mode", "SecurityError"); } } as unknown as IDBFactory;
  await assert.rejects(createWorkflowLibrary({ indexedDB: denied }).listWorkflows(), { code: "storage_unavailable" });
  const factory = new IDBFactory();
  const library = createWorkflowLibrary({ indexedDB: factory, name: "test", now: () => time });
  const original = await library.saveWorkflow({ workflow_id: "A", source: source() });
  const updated = { ...original, updated_at: "2026-10-22T01:00:00.000Z" };
  const put = IDBObjectStore.prototype.put;
  IDBObjectStore.prototype.put = function (...args: Parameters<typeof put>) {
    if (this.name === "deployments") throw new DOMException("full", "QuotaExceededError");
    return put.apply(this, args);
  };
  try { await assert.rejects(library.saveDeployment(deployment(original.workflow_id, original.source_digest), updated), { code: "storage_quota" }); }
  finally { IDBObjectStore.prototype.put = put; }
  assert.deepEqual(await library.getWorkflow("A"), original); assert.deepEqual(await library.listDeployments(), []);
  await library.close();
});

test("corrupt records and changed exact-source digests fail closed without silently deleting source", async () => {
  const factory = new IDBFactory();
  const library = createWorkflowLibrary({ indexedDB: factory, name: "test", now: () => time });
  const workflow = await library.saveWorkflow({ workflow_id: "A", source: source() });
  await tamper(factory, "workflows", { ...workflow, source_digest: "f".repeat(64) });
  await assert.rejects(library.getWorkflow("A"), { code: "digest_mismatch" });
  await tamper(factory, "workflows", { ...workflow, source: "corrupt" });
  await assert.rejects(library.listWorkflows(), { code: "storage_corrupt" });
  await tamper(factory, "deployments", { ...deployment("A", workflow.source_digest), revision: 9007199254740993 });
  await assert.rejects(library.getDeployment(storeA, "A"), { code: "storage_corrupt" });
  await library.close();
});

test("HTTP-origin workflow IDs do not require crypto.randomUUID", async () => {
  const descriptor = Object.getOwnPropertyDescriptor(globalThis, "crypto")!;
  const getRandomValues = globalThis.crypto.getRandomValues.bind(globalThis.crypto);
  Object.defineProperty(globalThis, "crypto", { configurable: true, value: { getRandomValues } });
  const library = createWorkflowLibrary({ indexedDB: new IDBFactory(), now: () => time });
  try {
    const workflow = await library.saveWorkflow({ source: source() });
    assert.match(workflow.workflow_id, /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
    assert.deepEqual(await library.getWorkflow(workflow.workflow_id), workflow);
  } finally { Object.defineProperty(globalThis, "crypto", descriptor); await library.close(); }
});
