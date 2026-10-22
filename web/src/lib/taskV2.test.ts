import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import { buildTaskBlob } from "./taskRequests.ts";
import { parseTaskBlob } from "./taskBlob.ts";
import { parseTestScript, parseTestScriptDraft, serializeTestScript, serializeTestScriptDraft, type TestScript } from "./testScript.ts";
import { compileTaskSource, createTaskV2Client, encodeTaskMutation, exportRawTaskIr, exportWorkflowSource, importRawTaskIr, importWorkflowSource, parseTaskStoreMetadata, taskBase64, taskHexBytes, taskIrSection, taskRevision, taskRevisionString, taskSha256, TASK_IR_CONTENT_TYPE, TASK_MUTATION_CONTENT_TYPE, TASK_MUTATION_MAX_BYTES, TASK_SOURCE_CONTENT_TYPE, TASK_STORE_ERROR_CODES, type TaskStoreMetadata } from "./taskV2.ts";

const storeId = "12345678-1234-4567-890a-123456789abc";
const fingerprint = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
const script = (name = "A"): TestScript => ({ schema: "linkr-test.v1", name, version: "1.0", created: "2026-10-22T00:00:00.000Z", steps: [{ id: "s1", type: "power_on", params: { rail: "5v_out" } }] });
async function fixture(names = ["A", "B"]) {
  const compiled = await Promise.all(names.map((id) => compileTaskSource(id, script(id))));
  const blob = `# linkr-task.v1\n${compiled.map((task) => task.ir.slice(15)).join("")}`;
  const metadata: TaskStoreMetadata = { task_store_id: storeId, revision: "9007199254740993", catalog_fingerprint: fingerprint, full_ir_bytes: Buffer.byteLength(blob), full_ir_digest: await taskSha256(blob), tasks: compiled.map((task, slot) => ({ slot, id: names[slot], request_count: 1, source_state: "present", source_bytes: Buffer.byteLength(task.source), source_digest: task.source_digest, ir_bytes: Buffer.byteLength(task.ir), task_ir_digest: task.task_ir_digest })) };
  const wire = { schema: "radxa-linkr-debugger.v1", ok: true, command: "tasks", action: "list", ...metadata };
  return { compiled, blob, metadata, wire };
}
const raw = (text: string, type = TASK_IR_CONTENT_TYPE) => new Response(text, { headers: { "Content-Type": type } });
const json = (data: unknown, status = 200) => new Response(JSON.stringify(data), { status, headers: { "Content-Type": "application/json" } });
function sequence(responses: Response[]) {
  const calls: Array<[unknown, RequestInit | undefined]> = [];
  const fetcher: typeof fetch = async (url, init) => { calls.push([url, init]); const response = responses.shift(); assert.ok(response, "unexpected extra request"); return response; };
  return { client: createTaskV2Client("/api/v1", fetcher), calls };
}

// Consume the exact C literals asserted against the production serializer in
// the HTTP host test. No compiler, board, network or generated artifact needed.
function firmwareGoldenJson() {
  const source = readFileSync(new URL("../../../apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_http_net.c", import.meta.url), "utf8").replace(/\\\r?\n/g, "");
  const definitions = new Map(Array.from(source.matchAll(/^#define (TASK_TEST_\w+)\s+(.+)$/gm), (match) => [match[1], match[2]]));
  function expression(text: string): string {
    return Array.from(text.matchAll(/"(?:\\.|[^"\\])*"|[A-Za-z_]\w*/g), ([token]) => {
      if (token.startsWith('"')) return JSON.parse(token) as string;
      const value = definitions.get(token);
      assert.ok(value, `Unknown C golden constant ${token}`);
      return expression(value);
    }).join("");
  }
  const vectors = new Map(Array.from(source.matchAll(/static const char task_test_golden_(\w+)\[\] =\s*([\s\S]*?);/g), (match) => [match[1], JSON.parse(expression(match[2])) as Record<string, unknown>]));
  assert.equal(vectors.size, 4);
  return Object.fromEntries(vectors);
}

test("firmware golden metadata, mutation task/null and errors match the Web wire contract", async () => {
  const golden = firmwareGoldenJson();
  const metadata = parseTaskStoreMetadata(golden.list);
  assert.equal(metadata.revision, "9007199254740993");
  assert.deepEqual(metadata.tasks.map((task) => [task.slot, task.id, task.source_state, task.ir_bytes]), [[0, "A", "present", 100], [3, "B", "absent", 100]]);
  const a = await compileTaskSource("A", script("A"));
  const b = await compileTaskSource("B", script("B"));
  const blob = a.ir + b.ir.slice("# linkr-task.v1\n".length);
  assert.equal(metadata.full_ir_bytes, Buffer.byteLength(blob));
  assert.notEqual(metadata.tasks[1].ir_bytes, Buffer.byteLength(blob) - Buffer.byteLength(a.ir));
  const { client, calls } = sequence([json(golden.list), raw(blob), json(golden.list), raw(a.source, TASK_SOURCE_CONTENT_TYPE), json(golden.upsert), json(golden.delete), json(golden.conflict, 409)]);
  assert.deepEqual(await client.readIr(), { metadata, blob });
  assert.equal((await client.readSource("A")).source, a.source);
  const upsert = await client.upsertSource(metadata, "A", script("A"));
  assert.deepEqual(upsert.result.task, metadata.tasks[0]);
  assert.equal(upsert.result.catalog_fingerprint, metadata.catalog_fingerprint);
  const mutation = { action: "delete" as const, id: "B", expectedRevision: metadata.revision, catalogFingerprint: metadata.catalog_fingerprint };
  assert.equal((await client.mutate(mutation)).task, null);
  await assert.rejects(client.mutate(mutation), (cause: unknown) => {
    assert.equal((cause as { code: string }).code, "revision_conflict");
    assert.equal((cause as { status: number }).status, 409);
    assert.deepEqual((cause as { response: unknown }).response, golden.conflict);
    return true;
  });
  assert.equal(calls.length, 7);
  await assert.rejects(sequence([json(golden.list)]).client.readSource("B"), { code: "task_source_absent" });
  for (const field of ["catalog_fingerprint", "task"]) {
    const malformed = { ...golden.delete }; delete malformed[field];
    await assert.rejects(sequence([json(malformed)]).client.mutate(mutation), { code: "invalid_response" });
  }
});

test("JSON mutation golden body uses stable fields, decimal revision and hex catalog", () => {
  const bytes = encodeTaskMutation({ action: "delete", id: "A", expectedRevision: "9007199254740993", catalogFingerprint: fingerprint });
  const text = new TextDecoder().decode(bytes);
  assert.equal(text, `{"schema":"radxa-linkr-debugger.v1","action":"delete","expected_revision":"9007199254740993","catalog_fingerprint":"${fingerprint}","id":"A"}`);
  assert.deepEqual(JSON.parse(text), { schema: "radxa-linkr-debugger.v1", action: "delete", expected_revision: "9007199254740993", catalog_fingerprint: fingerprint, id: "A" });
  assert.equal(taskRevisionString(taskRevision("18446744073709551615")), "18446744073709551615");
  for (const value of [9007199254740993, "01", "-1", "18446744073709551616"]) assert.throws(() => taskRevision(value));
  assert.deepEqual(taskHexBytes(fingerprint), Uint8Array.from({ length: 32 }, (_, i) => i));
  assert.equal(TASK_MUTATION_CONTENT_TYPE, "application/json");
});

test("upsert JSON body carries id/source_base64/ir_base64 losslessly and enforces identity and limits", async () => {
  const compiled = await compileTaskSource("中文", script());
  const bytes = encodeTaskMutation({ action: "upsert", id: "中文", source: compiled.source, ir: compiled.ir, expectedRevision: 1n, catalogFingerprint: fingerprint });
  const parsed = JSON.parse(new TextDecoder().decode(bytes)) as Record<string, string>;
  assert.deepEqual(Object.keys(parsed), ["schema", "action", "expected_revision", "catalog_fingerprint", "id", "source_base64", "ir_base64"]);
  assert.equal(parsed.schema, "radxa-linkr-debugger.v1");
  assert.equal(parsed.action, "upsert");
  assert.equal(parsed.expected_revision, "1");
  assert.equal(parsed.catalog_fingerprint, fingerprint);
  assert.equal(parsed.id, "中文");
  assert.equal(parsed.source_base64, Buffer.from(compiled.source, "utf8").toString("base64"));
  assert.equal(parsed.ir_base64, Buffer.from(compiled.ir, "utf8").toString("base64"));
  assert.equal(Buffer.from(parsed.source_base64!, "base64").toString("utf8"), compiled.source);
  assert.equal(Buffer.from(parsed.ir_base64!, "base64").toString("utf8"), compiled.ir);
  assert.throws(() => encodeTaskMutation({ action: "upsert", id: "wrong", ir: compiled.ir, expectedRevision: "1", catalogFingerprint: fingerprint }), { code: "invalid_task" });
  assert.throws(() => encodeTaskMutation({ action: "upsert", id: "中文", ir: compiled.ir, source: "x".repeat(4001), expectedRevision: "1", catalogFingerprint: fingerprint }), { code: "body_too_large" });
  assert.throws(() => encodeTaskMutation({ action: "import", ir: "", expectedRevision: "1", catalogFingerprint: fingerprint }), { code: "invalid_task" });
});

test("4000-byte worst-case source and full IR round-trip exactly inside the bounded base64 body", () => {
  const unit = `\\\u0022中文`;
  const bytesOf = (text: string) => new Uint8Array(Buffer.from(text, "utf8"));
  const source = unit.repeat(4000 / Buffer.byteLength(unit, "utf8"));
  assert.equal(Buffer.byteLength(source, "utf8"), 4000);
  let ir = `# linkr-task.v1\n# task A\n{"method":"PUT","path":"/api/v1/gpio/GP7","body":"{}"}\n`;
  while (ir.length + 5 <= 4000) ir += "#pad\n";
  const remainder = 4000 - ir.length;
  if (remainder === 1) ir += "\n";
  else if (remainder > 1) ir += "#".repeat(remainder - 1) + "\n";
  assert.equal(ir.length, 4000);
  const bytes = encodeTaskMutation({ action: "upsert", id: "A", source, ir, expectedRevision: "18446744073709551615", catalogFingerprint: fingerprint });
  assert.ok(bytes.length > 2 * 5336, `body is only ${bytes.length} bytes`);
  assert.ok(bytes.length <= TASK_MUTATION_MAX_BYTES, `body is ${bytes.length} bytes`);
  const parsed = JSON.parse(new TextDecoder().decode(bytes)) as Record<string, string>;
  assert.deepEqual(Object.keys(parsed), ["schema", "action", "expected_revision", "catalog_fingerprint", "id", "source_base64", "ir_base64"]);
  assert.equal(parsed.expected_revision, "18446744073709551615");
  assert.equal(parsed.source_base64, Buffer.from(source, "utf8").toString("base64"));
  assert.equal(parsed.ir_base64, Buffer.from(ir, "utf8").toString("base64"));
  assert.deepEqual(Buffer.from(parsed.source_base64!, "base64"), Buffer.from(source, "utf8"));
  assert.deepEqual(Buffer.from(parsed.ir_base64!, "base64"), Buffer.from(ir, "utf8"));
  for (const length of [1, 2, 3]) {
    const odd = "x".repeat(length);
    assert.equal(Buffer.from(taskBase64(bytesOf(odd)), "base64").toString("utf8"), odd);
  }
  assert.equal(taskBase64(bytesOf("")), "");
  assert.equal(taskBase64(bytesOf("f")), "Zg==");
  assert.equal(taskBase64(bytesOf("fo")), "Zm8=");
  assert.equal(taskBase64(bytesOf("foo")), "Zm9v");
  assert.equal(taskBase64(bytesOf("foob")), "Zm9vYg==");
});

test("binary-like source stays rejected while strict UTF-8 control policy keeps exact bytes", async () => {
  const compiled = await compileTaskSource("A", script());
  const binaryLike = String.fromCharCode(...Array.from({ length: 64 }, (_, i) => (i * 37 + 11) & 0xff));
  assert.throws(() => encodeTaskMutation({ action: "upsert", id: "A", source: binaryLike, ir: compiled.ir, expectedRevision: "1", catalogFingerprint: fingerprint }), { code: "invalid_task" });
  const exact = "line1\nline2\t\"q\"\\\r中文";
  const bytes = encodeTaskMutation({ action: "upsert", id: "A", source: exact, ir: compiled.ir, expectedRevision: "1", catalogFingerprint: fingerprint });
  const parsed = JSON.parse(new TextDecoder().decode(bytes)) as Record<string, string>;
  assert.equal(Buffer.from(parsed.source_base64!, "base64").toString("utf8"), exact);
});

test("A/B metadata reads exact full IR and exact source compiled against per-task digest", async () => {
  const { metadata, wire, blob, compiled } = await fixture();
  assert.deepEqual(parseTaskStoreMetadata(wire), metadata);
  const { client, calls } = sequence([json(wire), raw(blob), json(wire), raw(compiled[1].source, TASK_SOURCE_CONTENT_TYPE)]);
  assert.deepEqual(await client.readIr(), { metadata, blob });
  const opened = await client.readSource("B");
  assert.equal(opened.source, compiled[1].source); assert.equal(opened.script.name, "B"); assert.equal(calls[3][0], "/api/v1/tasks/slots/1/source");
  assert.equal(taskIrSection(blob, "A"), compiled[0].ir);
  assert.equal(taskIrSection(blob, "B"), compiled[1].ir);
});

test("saving A then B uses atomic upserts, preserves both exact sources and refuses stale revision", async () => {
  let revision = 9007199254740993n;
  const slots = new Map<string, { source: string; ir: string }>();
  async function metadata() {
    const blob = slots.size ? `# linkr-task.v1\n${Array.from(slots.values(), (task) => task.ir.slice(15)).join("")}` : "";
    const tasks = await Promise.all(Array.from(slots.entries(), async ([id, task], slot) => ({ slot, id, request_count: parseTaskBlob(task.ir)[0].requests.length, source_state: "present", source_bytes: Buffer.byteLength(task.source), source_digest: await taskSha256(task.source), ir_bytes: Buffer.byteLength(task.ir), task_ir_digest: await taskSha256(taskIrSection(task.ir, id)) })));
    return { blob, wire: { schema: "radxa-linkr-debugger.v1", ok: true, command: "tasks", action: "list", task_store_id: storeId, revision: revision.toString(), catalog_fingerprint: fingerprint, full_ir_bytes: Buffer.byteLength(blob), full_ir_digest: await taskSha256(blob), tasks } };
  }
  const fetcher: typeof fetch = async (url, init) => {
    if (init?.method === "POST") {
      const body = JSON.parse(new TextDecoder().decode(init.body as Uint8Array)) as Record<string, string>;
      if (body.expected_revision !== revision.toString()) return json({ error: { code: "revision_conflict" }, revision: revision.toString() }, 409);
      assert.equal(body.action, "upsert");
      assert.deepEqual(Object.keys(body), ["schema", "action", "expected_revision", "catalog_fingerprint", "id", "source_base64", "ir_base64"]);
      const id = body.id;
      slots.set(id, { source: Buffer.from(body.source_base64!, "base64").toString("utf8"), ir: Buffer.from(body.ir_base64!, "base64").toString("utf8") }); revision++;
      const { wire } = await metadata();
      return json({ schema: wire.schema, ok: true, command: "tasks", action: "upsert", task_store_id: storeId, revision: revision.toString(), catalog_fingerprint: fingerprint, task: wire.tasks.find((task) => task.id === id) });
    }
    const { wire, blob } = await metadata();
    if (url === "/api/v1/tasks") return json(wire);
    if (url === "/api/v1/tasks/ir") return raw(blob);
    const slot = Number(String(url).match(/slots\/(\d)/)?.[1]);
    return raw(Array.from(slots.values())[slot].source, TASK_SOURCE_CONTENT_TYPE);
  };
  const client = createTaskV2Client("/api/v1", fetcher);
  const initial = await client.list();
  const a = await client.upsertSource(initial, "A", script("A"));
  const b = await client.upsertSource(await client.list(), "B", script("B"));
  const fresh = createTaskV2Client("/api/v1", fetcher);
  assert.deepEqual((await fresh.readIr()).metadata.tasks.map((task) => task.id), ["A", "B"]);
  assert.equal((await fresh.readSource("A")).source, a.source); assert.equal((await fresh.readSource("B")).source, b.source);
  await assert.rejects(client.upsertSource(initial, "A", script("changed")), { code: "revision_conflict" });
  assert.equal((await fresh.readSource("A")).source, a.source);
});

test("raw digest races re-list once and permanent mismatches stop after two attempts", async () => {
  const { wire, blob } = await fixture();
  const good = sequence([json(wire), raw(blob + "\n"), json({ ...wire, revision: "9007199254740994" }), raw(blob)]);
  assert.equal((await good.client.readIr()).metadata.revision, "9007199254740994"); assert.equal(good.calls.length, 4);
  const bad = sequence([json(wire), raw("wrong"), json(wire), raw("wrong")]);
  await assert.rejects(bad.client.readIr(), { code: "digest_mismatch" }); assert.equal(bad.calls.length, 4);
});

test("source digest races retry, IR-only is absent and recompilation mismatch disables lossless open", async () => {
  const { wire, compiled } = await fixture(["A"]);
  const raced = sequence([json(wire), raw("changed", TASK_SOURCE_CONTENT_TYPE), json(wire), raw(compiled[0].source, TASK_SOURCE_CONTENT_TYPE)]);
  assert.equal((await raced.client.readSource("A")).source, compiled[0].source);
  const mismatch = { ...wire, tasks: [{ ...wire.tasks[0], task_ir_digest: "0".repeat(64) }] };
  await assert.rejects(sequence([json(mismatch), raw(compiled[0].source, TASK_SOURCE_CONTENT_TYPE)]).client.readSource("A"), { code: "source_ir_mismatch" });
  const absent = { ...wire, tasks: [{ ...wire.tasks[0], source_state: "absent", source_bytes: 0, source_digest: null }] };
  await assert.rejects(sequence([json(absent)]).client.readSource("A"), { code: "task_source_absent" });
});

test("stale revision and stable firmware errors preserve response without mutation retries", async () => {
  for (const code of TASK_STORE_ERROR_CODES) {
    const body = { ok: false, revision: "9007199254740994", catalog_fingerprint: fingerprint, error: { code, message: code } };
    const { client, calls } = sequence([json(body, code === "storage_full" ? 507 : 409)]);
    await assert.rejects(client.mutate({ action: "clear", expectedRevision: "9007199254740993", catalogFingerprint: fingerprint }), (cause: unknown) => { assert.equal((cause as { code: string }).code, code); assert.deepEqual((cause as { response: unknown }).response, body); return true; });
    assert.equal(calls.length, 1);
  }
});

test("source is canonical and digest-checked before any upsert POST; >4000 remains local", async () => {
  const { wire, metadata, compiled } = await fixture(["A"]);
  const noPost = sequence([]);
  await assert.rejects(noPost.client.mutate({ action: "upsert", id: "A", source: compiled[0].source, ir: buildTaskBlob("A", { ...script(), steps: [{ id: "off", type: "power_off", params: { rail: "5v_out" } }] }), expectedRevision: metadata.revision, catalogFingerprint: fingerprint }), { code: "source_ir_mismatch" });
  assert.equal(noPost.calls.length, 0);
  const localScript = script("x".repeat(4100));
  const success = sequence([json({ ...wire, action: "upsert", revision: "9007199254740994", task: null })]);
  const deployed = await success.client.upsertSource(metadata, "A", localScript);
  assert.equal(deployed.local_only, true);
  const sent = JSON.parse(new TextDecoder().decode(success.calls[0][1]!.body as Uint8Array)) as Record<string, string>;
  assert.equal(sent.source_base64, undefined);
  assert.equal(sent.id, "A");
});

test("strict parser recursively rejects unknown header/flow/params/assertion/unit fields", () => {
  const header = { schema: "linkr-test.v1", name: "Strict", created: "2026-10-22T00:00:00.000Z" };
  const primitive = { id: "a", type: "adc_read", params: { channel: "5v_out" }, assert: { current_range: { min_a: 0, max_a: 1 } } };
  const cases = [
    [{ ...header, hidden: true }, primitive],
    [header, { ...primitive, hidden: true }],
    [header, { ...primitive, params: { ...primitive.params, hidden: true } }],
    [header, { ...primitive, assert: { hidden: true } }],
    [header, { ...primitive, assert: { current_range: { min_a: 0, max_a: 1, hidden: true } } }],
    [header, { id: "l", type: "loop", params: { count: 1, unit: { name: "U", hidden: true }, steps: [primitive] } }],
    [header, { id: "c", type: "condition", params: { check: primitive, then_steps: [{ ...primitive, id: "b", params: { channel: "5v_out", hidden: true } }], else_steps: [] } }],
  ];
  for (const lines of cases) assert.throws(() => parseTestScript(lines.map((line) => JSON.stringify(line)).join("\n")), /unknown field/);
});

test("draft buffers, transient numbers and duplicate dangerous fields never reach an upsert POST", async () => {
  const { metadata } = await fixture(["A"]);
  const noPost = sequence([]);
  const incomplete = { ...script(), steps: [{ id: "delay", type: "delay" as const, params: { ms: -1 } }] };
  const draft = serializeTestScriptDraft(incomplete);
  const duplicate = String.raw`{"schema":"linkr-test.v1","name":"Review"}` + "\n" + String.raw`{"id":"on","type":"power_on","params":{"rail":"20v_out"},"pa\u0072ams":{"rail":"5v_out"}}`;
  assert.deepEqual(parseTestScriptDraft(draft), incomplete);
  for (const input of [draft, incomplete, duplicate]) {
    await assert.rejects(noPost.client.upsertSource(metadata, "A", input), { code: "invalid_task" });
  }
  for (const input of [draft, duplicate]) assert.throws(() => importWorkflowSource(input));
  assert.equal(noPost.calls.length, 0);
  assert.equal(serializeTestScriptDraft(incomplete), draft);
});

test("created is stamped once and canonical source roundtrips byte-stably including nested key order", () => {
  const input = script(); delete input.created;
  const first = serializeTestScript(input);
  assert.ok(input.created); assert.equal(serializeTestScript(input), first);
  assert.equal(exportWorkflowSource(importWorkflowSource(first)), first);
  assert.equal(serializeTestScript(parseTestScript(first)), first);
  const blob = buildTaskBlob("A", script()); assert.equal(exportRawTaskIr(importRawTaskIr(blob)), blob);
  assert.throws(() => importRawTaskIr(first)); assert.throws(() => importWorkflowSource(blob));
  const defaults: TestScript = { ...script(), steps: [{ id: "gpio", type: "gpio_set", params: { pin: "GP13", direction: "input" } }, { id: "unit", type: "loop", params: { count: 1, unit: { name: " Unit " }, steps: [{ id: "nested", type: "gpio_set", params: { pin: "GP13", direction: "input" }, continue_on_error: false }] } }] };
  const canonical = serializeTestScript(defaults);
  assert.equal(serializeTestScript(parseTestScript(canonical)), canonical);
});
