import { describe, expect, it, vi } from "vitest";
import { dispatchTaskRequest } from "./api";
import {
  parseTaskBlob,
  parseTaskListResponse,
  runTaskRequests,
  selectTaskRequests,
  sleepTaskDelay,
  TaskDataError,
  TASK_BLOB_MAX_BYTES,
  TASK_ID_MAX_BYTES,
  TASK_LINE_MAX_BYTES,
  TASK_MAX_TASKS,
  TASK_NAME_MAX_BYTES,
  type ParsedTask,
} from "./taskRunner";
import { TASK_MAX_REQUESTS, type TaskRequest } from "./taskRequests";

const FROZEN_SCHEMA = "radxa-linkr-debugger.v1";
const textEncoder = new TextEncoder();
const byteLength = (value: string): number => textEncoder.encode(value).length;

const MASKROM_BLOB = [
  "# linkr-task.v1",
  "# task rockchip-maskrom-5v_out",
  '{"method":"PUT","path":"/api/v1/gpio/CON_MAS","body":"{\\"direction\\":\\"input\\"}"}',
  '{"method":"PUT","path":"/api/v1/power/5v_out","body":"{\\"state\\":\\"off\\"}","wait_ms":1000}',
  "",
].join("\n");

function taskListResponse(blob: string): Record<string, unknown> {
  return {
    schema: FROZEN_SCHEMA,
    ok: true,
    command: "task",
    action: "list",
    task_count: blob === "" ? 0 : 1,
    tasks: blob === "" ? [] : [{ id: "t1", name: "t1", request_count: 1 }],
    blob,
  };
}

function taskListResponseWithTasks(tasks: readonly Record<string, unknown>[], blob: string): unknown {
  return {
    schema: FROZEN_SCHEMA,
    ok: true,
    command: "task",
    action: "list",
    task_count: tasks.length,
    tasks,
    blob,
  };
}

describe("parseTaskListResponse", () => {
  it("accepts the frozen empty-storage response", () => {
    // Given: the firmware empty-storage envelope
    // When: parsing it
    const data = parseTaskListResponse({
      schema: "radxa-linkr-debugger.v1",
      ok: true,
      command: "task",
      action: "list",
      task_count: 0,
      tasks: [],
      blob: "",
    });
    // Then: it is a valid empty state, not an error
    expect(data).toEqual({ tasks: [], blob: "" });
  });

  it("accepts a populated list with the exact stored blob", () => {
    const data = parseTaskListResponse(taskListResponse(MASKROM_BLOB));
    expect(data.blob).toBe(MASKROM_BLOB);
    expect(data.tasks).toEqual([{ id: "t1", name: "t1", request_count: 1 }]);
  });

  it("rejects an envelope with a wrong or missing schema literal", () => {
    // Given: envelopes whose schema is not the frozen repository literal
    const wrong = { ...taskListResponse(""), schema: "wrong.v1" };
    const missing = { ...taskListResponse("") };
    delete missing.schema;
    // When/Then: both are rejected as invalid responses
    expect(() => parseTaskListResponse(wrong)).toThrow(/schema/);
    expect(() => parseTaskListResponse(missing)).toThrow(/schema/);
  });

  it("rejects an envelope without ok:true", () => {
    const failed = { ...taskListResponse(""), ok: false };
    const missing = { ...taskListResponse("") };
    delete missing.ok;
    expect(() => parseTaskListResponse(failed)).toThrow(/ok/);
    expect(() => parseTaskListResponse(missing)).toThrow(/ok/);
  });

  it("rejects a response from another command", () => {
    expect(() =>
      parseTaskListResponse({ ...taskListResponse(""), command: "orch" }),
    ).toThrow(TaskDataError);
    expect(() =>
      parseTaskListResponse({ ...taskListResponse(""), command: "orch" }),
    ).toThrow(/command/);
  });

  it("rejects a wrong action", () => {
    expect(() =>
      parseTaskListResponse({ ...taskListResponse(""), action: "store" }),
    ).toThrow(/action/);
  });

  it("rejects a missing blob field", () => {
    const missing = taskListResponse("");
    delete missing.blob;
    expect(() => parseTaskListResponse(missing)).toThrow(/blob/);
  });

  it("rejects malformed task summaries", () => {
    expect(() =>
      parseTaskListResponse(
        taskListResponseWithTasks([{ id: "t1", name: "t1", request_count: "two" }], ""),
      ),
    ).toThrow(/request_count/);
  });

  it("rejects duplicate task ids even when the summaries differ", () => {
    // Given: two summaries sharing one id but different names and counts
    const summaries = [
      { id: "t1", name: "power-off", request_count: 1 },
      { id: "t1", name: "gpio-reset", request_count: 2 },
    ];
    // When/Then: the envelope is rejected before any task is selectable
    expect(() =>
      parseTaskListResponse(taskListResponseWithTasks(summaries, MASKROM_BLOB)),
    ).toThrow(/duplicate/);
  });

  it("rejects more task summaries than the firmware task limit", () => {
    // Given: one summary more than the firmware task capacity
    const summaries = Array.from({ length: TASK_MAX_TASKS + 1 }, (_, index) => ({
      id: `t${index}`,
      name: `t${index}`,
      request_count: 1,
    }));
    // When/Then: the envelope is rejected
    expect(() =>
      parseTaskListResponse(taskListResponseWithTasks(summaries, MASKROM_BLOB)),
    ).toThrow(new RegExp(String(TASK_MAX_TASKS)));
  });

  it("accepts exactly the firmware task summary boundary values", () => {
    // Given: summaries at the exact firmware id/name/count limits
    const summaries = Array.from({ length: TASK_MAX_TASKS }, (_, index) => ({
      id: `${"i".repeat(TASK_ID_MAX_BYTES - 1)}${index}`,
      name: "n".repeat(TASK_NAME_MAX_BYTES),
      request_count: TASK_MAX_REQUESTS,
    }));
    // When: parsing the envelope
    const data = parseTaskListResponse(taskListResponseWithTasks(summaries, MASKROM_BLOB));
    // Then: all boundary summaries are accepted
    expect(data.tasks).toHaveLength(TASK_MAX_TASKS);
    expect(data.tasks[0]?.id).toBe(summaries[0]?.id);
  });

  it("rejects summaries past the firmware id, name, and count bounds", () => {
    const base = { id: "t1", name: "t1", request_count: 1 };
    const cases: Record<string, unknown>[] = [
      { ...base, id: "i".repeat(TASK_ID_MAX_BYTES + 1) },
      { ...base, id: "" },
      { ...base, name: "n".repeat(TASK_NAME_MAX_BYTES + 1) },
      { ...base, request_count: TASK_MAX_REQUESTS + 1 },
      { ...base, request_count: -1 },
      { ...base, request_count: 1.5 },
    ];
    for (const summary of cases) {
      expect(() =>
        parseTaskListResponse(taskListResponseWithTasks([summary], "")),
      ).toThrow(TaskDataError);
    }
  });

  it("measures the blob limit in UTF-8 bytes, not code units", () => {
    // Given: a blob under 4096 code units but over 4096 UTF-8 bytes
    const oversized = "界".repeat(Math.floor(TASK_BLOB_MAX_BYTES / 3) + 1);
    expect(oversized.length).toBeLessThanOrEqual(TASK_BLOB_MAX_BYTES);
    expect(byteLength(oversized)).toBeGreaterThan(TASK_BLOB_MAX_BYTES);
    // When/Then: the envelope is rejected by byte length
    expect(() => parseTaskListResponse(taskListResponse(oversized))).toThrow(/4096/);
  });
});

const POWER_ON_LINE = '{"method":"PUT","path":"/api/v1/power/5v_out","body":"{\\"state\\":\\"on\\"}"}';

function blobWithRequestLine(line: string): string {
  return `# linkr-task.v1\n# task t1\n${line}\n`;
}

function requestLine(path: string, body = "{}"): string {
  return JSON.stringify({ method: "PUT", path, body });
}

function requestLineWithWait(pad: string): string {
  return JSON.stringify({
    method: "PUT",
    path: "/api/v1/power/5v_out",
    body: `{"state":"on","pad":"${pad}"}`,
    wait_ms: 0,
  });
}

function requestLineOfByteLength(target: number): string {
  let pad = "";
  while (byteLength(requestLineWithWait(pad)) < target) pad += "x";
  return requestLineWithWait(pad);
}

function paddedBlob(targetBytes: number): string {
  let blob = `# linkr-task.v1\n# task t1\n${POWER_ON_LINE}\n`;
  while (byteLength(blob) < targetBytes) {
    const remaining = targetBytes - byteLength(blob);
    if (remaining === 1) {
      blob += "\n";
    } else {
      const content = Math.min(remaining - 2, TASK_LINE_MAX_BYTES - 2);
      blob += `#${"x".repeat(content)}\n`;
    }
  }
  return blob;
}

describe("parseTaskBlob", () => {
  it("parses a valid blob into ordered task records", () => {
    const tasks = parseTaskBlob(MASKROM_BLOB);
    expect(tasks).toHaveLength(1);
    expect(tasks[0]?.id).toBe("rockchip-maskrom-5v_out");
    expect(tasks[0]?.requests).toEqual<TaskRequest[]>([
      { method: "PUT", path: "/api/v1/gpio/CON_MAS", body: '{"direction":"input"}' },
      {
        method: "PUT",
        path: "/api/v1/power/5v_out",
        body: '{"state":"off"}',
        wait_ms: 1000,
      },
    ]);
  });

  it("treats an empty blob as a valid empty state", () => {
    expect(parseTaskBlob("")).toEqual([]);
  });

  it("parses multiple task sections in order", () => {
    const blob = [
      "# linkr-task.v1",
      "# task first",
      '{"method":"PUT","path":"/api/v1/power/5v_out","body":"{\\"state\\":\\"on\\"}"}',
      "# task second",
      '{"method":"PUT","path":"/api/v1/switch/sd","body":"{\\"route\\":\\"target\\"}"}',
      "",
    ].join("\n");
    const tasks = parseTaskBlob(blob);
    expect(tasks.map((task) => task.id)).toEqual(["first", "second"]);
    expect(tasks[1]?.requests[0]?.path).toBe("/api/v1/switch/sd");
  });

  it("rejects duplicate task ids with different hardware actions before any PUT", () => {
    // Given: two sections sharing one id but carrying different control requests
    const blob = [
      "# linkr-task.v1",
      "# task t1",
      '{"method":"PUT","path":"/api/v1/power/5v_out","body":"{\\"state\\":\\"on\\"}"}',
      "# task t1",
      '{"method":"PUT","path":"/api/v1/power/5v_out","body":"{\\"state\\":\\"off\\"}"}',
      "",
    ].join("\n");
    // When/Then: parsing fails before any request can be dispatched
    expect(() => parseTaskBlob(blob)).toThrow(TaskDataError);
    expect(() => parseTaskBlob(blob)).toThrow(/duplicate task id/);
  });

  it("rejects a blob without the linkr-task.v1 marker", () => {
    expect(() => parseTaskBlob("# linkr-orch.v1\n# task t1\n")).toThrow(TaskDataError);
    expect(() => parseTaskBlob("# linkr-orch.v1\n# task t1\n")).toThrow(/linkr-task\.v1/);
  });

  it("rejects request lines before any task header", () => {
    const blob = [
      "# linkr-task.v1",
      '{"method":"PUT","path":"/api/v1/power/5v_out","body":"{\\"state\\":\\"on\\"}"}',
    ].join("\n");
    expect(() => parseTaskBlob(blob)).toThrow(/outside any task/);
  });

  it("rejects malformed request JSON", () => {
    const blob = "# linkr-task.v1\n# task t1\n{not json}\n";
    expect(() => parseTaskBlob(blob)).toThrow(/invalid request line/);
  });

  it("rejects non-PUT methods", () => {
    const blob = [
      "# linkr-task.v1",
      "# task t1",
      '{"method":"POST","path":"/api/v1/power/5v_out","body":"{\\"state\\":\\"on\\"}"}',
    ].join("\n");
    expect(() => parseTaskBlob(blob)).toThrow(/method PUT/);
  });

  it("rejects paths outside the power/gpio/switch allowlist", () => {
    const blob = [
      "# linkr-task.v1",
      "# task t1",
      '{"method":"PUT","path":"/api/v1/ota/confirm","body":"{}"}',
    ].join("\n");
    expect(() => parseTaskBlob(blob)).toThrow(/not an allowed control path/);
  });

  it("rejects lexical path bypasses before dispatch", async () => {
    // Given: paths that a browser URL or fetch implementation could normalize
    const paths = [
      "/api/v1/power/../config",
      "/api/v1/power/%2e%2e",
      "/api/v1/power/rail/extra",
      "/api/v1/power/rail?state=on",
      "/api/v1/power/rail#fragment",
      "/api/v1/power/rail\\config",
      "/api/v1/power/",
      "/api/v1/power/%2fconfig",
      "/api/v1/power/%25config",
      "/api/v1/power/rail name",
      "/api/v1/power/rail\tname",
      "/api/v1/power/rail\u0001name",
      "/api/v1/power/电源",
    ];
    const dispatch = vi.fn(async () => undefined);

    // When: each untrusted blob is parsed before execution
    for (const path of paths) {
      try {
        const tasks = parseTaskBlob(blobWithRequestLine(requestLine(path)));
        await runTaskRequests(tasks[0]?.requests ?? [], {
          dispatch,
          sleep: async () => undefined,
        });
      } catch (cause) {
        expect(cause).toBeInstanceOf(TaskDataError);
      }
    }

    // Then: no rejected path reaches the execution seam
    expect(dispatch).not.toHaveBeenCalled();
  });

  it("rejects unknown and duplicate request envelope fields", () => {
    // Given: raw JSON preserving duplicate keys that JSON.parse would discard
    const lines = [
      '{"method":"PUT","path":"/api/v1/power/rail","body":"{}","extra":true}',
      '{"method":"PUT","method":"PUT","path":"/api/v1/power/rail","body":"{}"}',
      '{"method":"PUT","path":"/api/v1/power/rail","path":"/api/v1/power/other","body":"{}"}',
      '{"method":"PUT","path":"/api/v1/power/rail","body":"{}","body":"{}"}',
      '{"method":"PUT","path":"/api/v1/power/rail","body":"{}","wait_ms":0,"wait_ms":1}',
    ];

    // When/Then: every non-strict envelope is rejected
    for (const line of lines) {
      expect(() => parseTaskBlob(blobWithRequestLine(line))).toThrow(TaskDataError);
    }
  });

  it("accepts 16 nested array or object body levels and rejects 17", () => {
    // Given: bodies immediately around the firmware nesting boundary
    const nestedArray = (depth: number) => `${"[".repeat(depth)}0${"]".repeat(depth)}`;
    const nestedObject = (depth: number) => `${'{"v":'.repeat(depth)}0${"}".repeat(depth)}`;

    // When/Then: 16 containers are valid and the 17th is rejected
    for (const body of [nestedArray(16), nestedObject(16)]) {
      expect(parseTaskBlob(blobWithRequestLine(requestLine("/api/v1/power/rail", body)))).toHaveLength(1);
    }
    for (const body of [nestedArray(17), nestedObject(17)]) {
      expect(() => parseTaskBlob(blobWithRequestLine(requestLine("/api/v1/power/rail", body)))).toThrow(TaskDataError);
    }
  });

  it("rejects a non-JSON body string", () => {
    const blob = [
      "# linkr-task.v1",
      "# task t1",
      '{"method":"PUT","path":"/api/v1/power/5v_out","body":"not json"}',
    ].join("\n");
    expect(() => parseTaskBlob(blob)).toThrow(/body is not valid JSON/);
  });

  it("rejects wait_ms above the client limit", () => {
    const blob = [
      "# linkr-task.v1",
      "# task t1",
      '{"method":"PUT","path":"/api/v1/power/5v_out","body":"{\\"state\\":\\"on\\"}","wait_ms":60001}',
    ].join("\n");
    expect(() => parseTaskBlob(blob)).toThrow(/wait_ms/);
  });

  it("rejects a task header without an id", () => {
    const blob = "# linkr-task.v1\n# task \n";
    expect(() => parseTaskBlob(blob)).toThrow(/task id/);
  });

  it("rejects legacy and suffixed markers instead of prefix-matching them", () => {
    // Given: blobs whose first marker line only starts with the frozen marker
    for (const marker of ["# linkr-task.v1-legacy", "# linkr-task.v1 ", "# linkr-orch.v1"]) {
      const blob = `${marker}\n# task t1\n${POWER_ON_LINE}\n`;
      // When/Then: the blob is rejected, never executed
      expect(() => parseTaskBlob(blob)).toThrow(TaskDataError);
      expect(() => parseTaskBlob(blob)).toThrow(/linkr-task\.v1/);
    }
  });

  it("accepts the firmware blob size boundary and rejects one byte past it", () => {
    // Given: valid blobs padded with comment lines to exact byte sizes
    const atLimit = paddedBlob(TASK_BLOB_MAX_BYTES);
    const overLimit = paddedBlob(TASK_BLOB_MAX_BYTES + 1);
    expect(byteLength(atLimit)).toBe(TASK_BLOB_MAX_BYTES);
    // When/Then: the limit is inclusive, one byte over is rejected
    expect(parseTaskBlob(atLimit)).toHaveLength(1);
    expect(() => parseTaskBlob(overLimit)).toThrow(/4096/);
  });

  it("measures the blob size limit in UTF-8 bytes, not code units", () => {
    // Given: a blob under 4096 code units but over 4096 UTF-8 bytes
    const commentLine = `#${"界".repeat(80)}`;
    expect(byteLength(commentLine)).toBeLessThanOrEqual(TASK_LINE_MAX_BYTES);
    const blob = ["# linkr-task.v1", "# task t1", ...Array<string>(17).fill(commentLine), ""].join("\n");
    expect(blob.length).toBeLessThanOrEqual(TASK_BLOB_MAX_BYTES);
    expect(byteLength(blob)).toBeGreaterThan(TASK_BLOB_MAX_BYTES);
    // When/Then: rejected by byte length
    expect(() => parseTaskBlob(blob)).toThrow(/4096/);
  });

  it("accepts the firmware task count boundary and rejects one task past it", () => {
    // Given: blobs at and past the firmware task capacity
    const section = (index: number) => `# task t${index}\n${POWER_ON_LINE}`;
    const atLimit = ["# linkr-task.v1", ...Array.from({ length: TASK_MAX_TASKS }, (_, i) => section(i)), ""].join("\n");
    const overLimit = ["# linkr-task.v1", ...Array.from({ length: TASK_MAX_TASKS + 1 }, (_, i) => section(i)), ""].join("\n");
    // When/Then: four tasks parse, five are rejected
    expect(parseTaskBlob(atLimit)).toHaveLength(TASK_MAX_TASKS);
    expect(() => parseTaskBlob(overLimit)).toThrow(new RegExp(String(TASK_MAX_TASKS)));
  });

  it("accepts the firmware per-task request boundary and rejects one request past it", () => {
    // Given: one task at and past the firmware request capacity
    const build = (count: number) =>
      ["# linkr-task.v1", "# task t1", ...Array<string>(count).fill(POWER_ON_LINE), ""].join("\n");
    // When/Then: 32 requests parse, 33 are rejected
    expect(parseTaskBlob(build(TASK_MAX_REQUESTS))[0]?.requests).toHaveLength(TASK_MAX_REQUESTS);
    expect(() => parseTaskBlob(build(TASK_MAX_REQUESTS + 1))).toThrow(new RegExp(String(TASK_MAX_REQUESTS)));
  });

  it("measures the task id limit in UTF-8 bytes and rejects forbidden characters", () => {
    // Given: task ids around the 31-byte firmware limit
    const blobFor = (id: string) => `# linkr-task.v1\n# task ${id}\n${POWER_ON_LINE}\n`;
    const multibyteOver = "界".repeat(11);
    expect(multibyteOver.length).toBeLessThanOrEqual(TASK_ID_MAX_BYTES);
    expect(byteLength(multibyteOver)).toBeGreaterThan(TASK_ID_MAX_BYTES);
    // When/Then: the boundary holds in bytes and firmware-forbidden characters reject
    expect(parseTaskBlob(blobFor("i".repeat(TASK_ID_MAX_BYTES)))[0]?.id).toBe("i".repeat(TASK_ID_MAX_BYTES));
    expect(() => parseTaskBlob(blobFor("i".repeat(TASK_ID_MAX_BYTES + 1)))).toThrow(/task id/);
    expect(() => parseTaskBlob(blobFor(multibyteOver))).toThrow(/task id/);
    expect(() => parseTaskBlob(blobFor("bad id"))).toThrow(/task id/);
    expect(() => parseTaskBlob(blobFor("bad#id"))).toThrow(/task id/);
  });

  it("accepts a 256-byte request line and rejects a 257-byte one", () => {
    // Given: request lines at and past the firmware line limit
    const atLimit = requestLineOfByteLength(TASK_LINE_MAX_BYTES);
    const overLimit = requestLineOfByteLength(TASK_LINE_MAX_BYTES + 1);
    expect(byteLength(atLimit)).toBe(TASK_LINE_MAX_BYTES);
    const build = (line: string) => `# linkr-task.v1\n# task t1\n${line}\n`;
    // When/Then: the line limit is inclusive
    expect(parseTaskBlob(build(atLimit))[0]?.requests).toHaveLength(1);
    expect(() => parseTaskBlob(build(overLimit))).toThrow(/256/);
  });

  it("enforces the firmware path byte limit", () => {
    // Given: paths at and past the 96-byte firmware limit
    const pathAt = `/api/v1/power/${"r".repeat(96 - "/api/v1/power/".length)}`;
    const pathOver = `${pathAt}r`;
    const build = (path: string) =>
      `# linkr-task.v1\n# task t1\n${JSON.stringify({ method: "PUT", path, body: '{"state":"on"}' })}\n`;
    // When/Then: 96 bytes parse, 97 are rejected
    expect(parseTaskBlob(build(pathAt))[0]?.requests[0]?.path).toBe(pathAt);
    expect(() => parseTaskBlob(build(pathOver))).toThrow(/96/);
  });

  it("enforces the firmware body byte limit", () => {
    // Given: bodies at and past the 192-byte firmware limit
    const bodyOfByteLength = (target: number): string => {
      let pad = "";
      const build = (p: string) => `{"state":"on","pad":"${p}"}`;
      while (byteLength(build(pad)) < target) pad += "x";
      return build(pad);
    };
    const bodyAt = bodyOfByteLength(192);
    const bodyOver = bodyOfByteLength(193);
    expect(byteLength(bodyAt)).toBe(192);
    const build = (body: string) =>
      `# linkr-task.v1\n# task t1\n${JSON.stringify({ method: "PUT", path: "/api/v1/gpio/GP13", body })}\n`;
    // When/Then: 192 bytes parse, 193 are rejected
    expect(parseTaskBlob(build(bodyAt))[0]?.requests[0]?.body).toBe(bodyAt);
    expect(() => parseTaskBlob(build(bodyOver))).toThrow(/192/);
  });

  it("accepts wait_ms at the firmware maximum and rejects negative or fractional waits", () => {
    // Given: wait metadata at the edges of the 0..60000 firmware range
    const build = (waitMs: number) =>
      `# linkr-task.v1\n# task t1\n${JSON.stringify({ method: "PUT", path: "/api/v1/power/5v_out", body: '{"state":"on"}', wait_ms: waitMs })}\n`;
    // When/Then: 60000 parses, -1 and 100.5 are rejected
    expect(parseTaskBlob(build(60000))[0]?.requests[0]?.wait_ms).toBe(60000);
    expect(() => parseTaskBlob(build(-1))).toThrow(/wait_ms/);
    expect(() => parseTaskBlob(build(100.5))).toThrow(/wait_ms/);
  });
});

describe("dispatchTaskRequest", () => {
  it("does not fetch when called directly with a malicious control path", async () => {
    // Given: a direct caller bypassing stored-blob parsing
    const fetchMock = vi.spyOn(globalThis, "fetch");

    // When/Then: the dispatch seam independently rejects before fetch
    await expect(dispatchTaskRequest("/api/v1/power/../config", "{}")).rejects.toThrow();
    expect(fetchMock).not.toHaveBeenCalled();
    fetchMock.mockRestore();
  });

  it("preserves the original fetch abort rejection", async () => {
    // Given: an in-flight control request whose signal is cancelled
    const controller = new AbortController();
    const abortError = new DOMException("The operation was aborted.", "AbortError");
    const fetchMock = vi.spyOn(globalThis, "fetch").mockImplementation((_input, init) =>
      new Promise<Response>((_resolve, reject) => {
        init?.signal?.addEventListener("abort", () => reject(abortError), { once: true });
      })
    );

    try {
      // When: the request is aborted
      const pending = dispatchTaskRequest(
        "/api/v1/power/5v_out",
        '{"state":"off"}',
        controller.signal,
      );
      controller.abort();

      // Then: cancellation identity reaches the task runner unchanged
      await expect(pending).rejects.toBe(abortError);
    } finally {
      fetchMock.mockRestore();
    }
  });
});

describe("selectTaskRequests", () => {
  const tasks: ParsedTask[] = [
    {
      id: "present",
      requests: [
        { method: "PUT", path: "/api/v1/power/5v_out", body: '{"state":"on"}' },
      ],
    },
  ];

  it("returns the records of the selected task", () => {
    expect(selectTaskRequests(tasks, "present")).toHaveLength(1);
  });

  it("rejects an unknown task id", () => {
    expect(() => selectTaskRequests(tasks, "missing")).toThrow(TaskDataError);
    expect(() => selectTaskRequests(tasks, "missing")).toThrow(/missing/);
    try {
      selectTaskRequests(tasks, "missing");
    } catch (cause) {
      expect(cause).toBeInstanceOf(TaskDataError);
      if (!(cause instanceof TaskDataError)) throw cause;
      expect(cause.code).toBe("unknown_task");
      return;
    }
    throw new Error("expected selectTaskRequests to throw");
  });
});

describe("runTaskRequests", () => {
  const record = (path: string, waitMs?: number): TaskRequest => ({
    method: "PUT",
    path,
    body: '{"state":"on"}',
    ...(waitMs === undefined ? {} : { wait_ms: waitMs }),
  });

  it("dispatches records in order and applies client waits after each success", async () => {
    // Given: three records, the first and third carrying wait metadata
    const events: string[] = [];
    const result = await runTaskRequests(
      [record("/api/v1/power/5v_out", 100), record("/api/v1/gpio/GP13"), record("/api/v1/switch/sd", 50)],
      {
        dispatch: async (request) => {
          events.push(`dispatch ${request.path}`);
        },
        sleep: async (ms) => {
          events.push(`sleep ${ms}`);
        },
      },
    );
    // Then: waits happen client-side, after the successful request they follow
    expect(result).toEqual({ ok: true, completed: 3 });
    expect(events).toEqual([
      "dispatch /api/v1/power/5v_out",
      "sleep 100",
      "dispatch /api/v1/gpio/GP13",
      "dispatch /api/v1/switch/sd",
      "sleep 50",
    ]);
  });

  it("never sends wait_ms to the board API", async () => {
    const bodies: string[] = [];
    await runTaskRequests([record("/api/v1/power/5v_out", 250)], {
      dispatch: async (request) => {
        bodies.push(request.body);
      },
      sleep: async () => undefined,
    });
    expect(bodies).toEqual(['{"state":"on"}']);
    expect(bodies[0]).not.toContain("wait_ms");
  });

  it("stops at the first failure and never dispatches later records", async () => {
    // Given: a dispatcher that fails on the second record
    const dispatched: string[] = [];
    const sleeps: number[] = [];
    const result = await runTaskRequests(
      [record("/api/v1/power/5v_out", 10), record("/api/v1/gpio/GP13", 20), record("/api/v1/switch/sd")],
      {
        dispatch: async (request) => {
          dispatched.push(request.path);
          if (request.path === "/api/v1/gpio/GP13") throw new Error("gpio rejected");
        },
        sleep: async (ms) => {
          sleeps.push(ms);
        },
      },
    );
    // Then: the third record is never sent and no wait follows the failure
    expect(dispatched).toEqual(["/api/v1/power/5v_out", "/api/v1/gpio/GP13"]);
    expect(sleeps).toEqual([10]);
    expect(result).toEqual({
      ok: false,
      completed: 1,
      failedIndex: 2,
      failedPath: "/api/v1/gpio/GP13",
      error: "gpio rejected",
    });
  });

  it("reports progress with one-based indexes", async () => {
    const progress: string[] = [];
    await runTaskRequests([record("/api/v1/power/5v_out"), record("/api/v1/gpio/GP13")], {
      dispatch: async () => undefined,
      sleep: async () => undefined,
      onProgress: (p) => progress.push(`${p.index}/${p.total} ${p.path}`),
    });
    expect(progress).toEqual(["1/2 /api/v1/power/5v_out", "2/2 /api/v1/gpio/GP13"]);
  });

  it("converts non-Error dispatch failures to text", async () => {
    const result = await runTaskRequests([record("/api/v1/power/5v_out")], {
      dispatch: () => Promise.reject("string failure"),
      sleep: async () => undefined,
    });
    expect(result).toMatchObject({ ok: false, failedIndex: 1, error: "string failure" });
  });

  it("clears a pending delay timer as soon as cancellation aborts the wait", async () => {
    // Given: a completed request followed by a long client-side delay
    vi.useFakeTimers();
    const controller = new AbortController();
    try {
      const pending = runTaskRequests([record("/api/v1/power/5v_out", 5000)], {
        dispatch: async () => undefined,
        sleep: sleepTaskDelay,
        signal: controller.signal,
      });
      await Promise.resolve();
      await Promise.resolve();
      expect(vi.getTimerCount()).toBe(1);

      // When: cancellation interrupts the delay
      controller.abort();

      // Then: the run completes as cancelled and owns no stale timer
      await expect(pending).resolves.toEqual({
        ok: false,
        completed: 1,
        attempted: 1,
        cancelled: true,
      });
      expect(vi.getTimerCount()).toBe(0);
    } finally {
      vi.useRealTimers();
    }
  });
});
