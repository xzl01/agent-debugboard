import { describe, expect, it } from "vitest";
import type { BuiltInTask } from "./builtinTasks.ts";
import type { TaskRequest } from "./taskRequests.ts";
import { TaskDataError, type TaskListData } from "./taskRunner.ts";
import {
  executeTaskRun,
  resolveTaskRunSnapshot,
  type TaskRunSnapshot,
} from "./taskExecution.ts";

interface DispatchRecord {
  readonly path: string;
  readonly body: string;
  readonly signal?: AbortSignal;
}

const MASKROM_5V_REQUESTS: readonly TaskRequest[] = [
  { method: "PUT", path: "/api/v1/gpio/CON_MAS", body: '{"direction":"input"}', wait_ms: 0 },
  { method: "PUT", path: "/api/v1/power/5v_out", body: '{"state":"off"}', wait_ms: 1000 },
  { method: "PUT", path: "/api/v1/gpio/CON_MAS", body: '{"direction":"output","value":0}', wait_ms: 20 },
  { method: "PUT", path: "/api/v1/power/5v_out", body: '{"state":"on"}', wait_ms: 500 },
  { method: "PUT", path: "/api/v1/gpio/CON_MAS", body: '{"direction":"input"}', wait_ms: 0 },
];

const BUILT_IN_SNAPSHOT: TaskRunSnapshot = {
  id: "builtin/maskrom/5v_out",
  name: "MASKROM via 5v_out",
  source: "builtin",
  requests: MASKROM_5V_REQUESTS,
  cleanup: { method: "PUT", path: "/api/v1/gpio/CON_MAS", body: '{"direction":"input"}' },
};

const BUILT_IN_CATALOG: readonly BuiltInTask[] = [
  {
    id: "builtin/maskrom/5v_out",
    name: "Rockchip MASKROM via 5v_out",
    requests: MASKROM_5V_REQUESTS,
    cleanup: { method: "PUT", path: "/api/v1/gpio/CON_MAS", body: '{"direction":"input"}' },
  },
  {
    id: "builtin/edl/12v_out",
    name: "Qualcomm EDL via 12v_out",
    requests: MASKROM_5V_REQUESTS,
    cleanup: { method: "PUT", path: "/api/v1/gpio/CON_MAS", body: '{"direction":"input"}' },
  },
];

const STORED_SNAPSHOT: TaskRunSnapshot = {
  id: "t1",
  name: "t1",
  source: "stored",
  requests: [
    { method: "PUT", path: "/api/v1/power/5v_out", body: '{"state":"off"}', wait_ms: 1000 },
    { method: "PUT", path: "/api/v1/gpio/CON_MAS", body: '{"direction":"input"}' },
  ],
  cleanup: null,
};

const instantSleep = () => Promise.resolve();

function listData(tasks: readonly { id: string; name: string; request_count: number }[], blob: string): TaskListData {
  return { tasks, blob };
}

describe("resolveTaskRunSnapshot", () => {
  it("resolves a built-in locally without touching task storage", async () => {
    // Given: a fetch that must never run for built-ins
    let fetched = false;
    const fetchTasks = () => {
      fetched = true;
      return Promise.reject(new Error("must not fetch"));
    };

    // When: resolving a built-in id
    const resolved = await resolveTaskRunSnapshot("builtin/maskrom/5v_out", BUILT_IN_CATALOG, fetchTasks);

    // Then: the snapshot is the catalog built-in with cleanup metadata
    expect(fetched).toBe(false);
    expect(resolved.snapshot.source).toBe("builtin");
    expect(resolved.snapshot.id).toBe("builtin/maskrom/5v_out");
    expect(resolved.snapshot.requests).toHaveLength(5);
    expect(resolved.snapshot.cleanup?.path).toBe("/api/v1/gpio/CON_MAS");
    expect(resolved.storedTasks).toBeUndefined();
  });

  it("resolves a stored task through GET + strict parse and reports the summaries", async () => {
    // Given: stored data with one two-record task
    const blob = [
      "# linkr-task.v1",
      "# task t1",
      '{"method":"PUT","path":"/api/v1/power/5v_out","body":"{\\"state\\":\\"off\\"}"}',
      '{"method":"PUT","path":"/api/v1/gpio/CON_MAS","body":"{\\"direction\\":\\"input\\"}"}',
      "",
    ].join("\n");
    const data = listData([{ id: "t1", name: "t1", request_count: 2 }], blob);

    // When: resolving the stored id
    const resolved = await resolveTaskRunSnapshot("t1", BUILT_IN_CATALOG, () => Promise.resolve(data));

    // Then: the snapshot uses the parsed stored records and never infers cleanup
    expect(resolved.snapshot.source).toBe("stored");
    expect(resolved.snapshot.requests.map((request) => request.path)).toEqual([
      "/api/v1/power/5v_out",
      "/api/v1/gpio/CON_MAS",
    ]);
    expect(resolved.snapshot.cleanup).toBeNull();
    expect(resolved.storedTasks).toEqual(data.tasks);
  });

  it("lets the built-in win an exact ID collision without fetching storage", async () => {
    let fetched = false;
    const resolved = await resolveTaskRunSnapshot("builtin/edl/12v_out", BUILT_IN_CATALOG, () => {
      fetched = true;
      return Promise.reject(new Error("must not fetch"));
    });
    expect(fetched).toBe(false);
    expect(resolved.snapshot.source).toBe("builtin");
  });

  it("rejects an unknown stored id before any execution", async () => {
    const data = listData([], "# linkr-task.v1\n");
    await expect(resolveTaskRunSnapshot("ghost", BUILT_IN_CATALOG, () => Promise.resolve(data))).rejects.toBeInstanceOf(TaskDataError);
  });

  it("rejects a builtin/ id with catalog_unavailable when the catalog fetch failed, without touching storage", async () => {
    // Given: an unavailable catalog and a fetch that must never run
    let fetched = false;
    const fetchTasks = () => {
      fetched = true;
      return Promise.reject(new Error("must not fetch"));
    };

    // When/Then: resolving a builtin/ id fails closed with catalog_unavailable
    await expect(resolveTaskRunSnapshot("builtin/maskrom/5v_out", null, fetchTasks)).rejects.toMatchObject({
      code: "catalog_unavailable",
    });
    expect(fetched).toBe(false);
  });

  it("rejects a builtin/ id absent from a successfully parsed catalog before fetching storage", async () => {
    // Given: a valid catalog that does not carry the requested builtin/ id
    let fetched = false;
    const fetchTasks = () => {
      fetched = true;
      return Promise.reject(new Error("must not fetch"));
    };

    // When/Then: the stored-blob impersonation path is never reached
    await expect(resolveTaskRunSnapshot("builtin/rogue", BUILT_IN_CATALOG, fetchTasks)).rejects.toMatchObject({
      code: "catalog_unavailable",
    });
    expect(fetched).toBe(false);
  });

  it("still resolves non-builtin stored ids when the catalog is unavailable", async () => {
    // Given: an unavailable catalog but reachable stored task data
    const blob = [
      "# linkr-task.v1",
      "# task t1",
      '{"method":"PUT","path":"/api/v1/power/5v_out","body":"{\\"state\\":\\"off\\"}"}',
      "",
    ].join("\n");
    const data = listData([{ id: "t1", name: "t1", request_count: 1 }], blob);

    // When: resolving a stored id
    const resolved = await resolveTaskRunSnapshot("t1", null, () => Promise.resolve(data));

    // Then: stored resolution is independent of catalog availability
    expect(resolved.snapshot.source).toBe("stored");
    expect(resolved.snapshot.requests).toHaveLength(1);
  });
});

describe("executeTaskRun", () => {
  function scriptedDispatch(failAt?: number, failCleanup = false) {
    const records: DispatchRecord[] = [];
    const dispatch = (request: TaskRequest, signal?: AbortSignal): Promise<unknown> => {
      records.push({ path: request.path, body: request.body, signal });
      const normalIndex = records.length;
      const isCleanup = failCleanup && normalIndex > (failAt ?? 0);
      const shouldFail = failAt !== undefined && normalIndex === failAt;
      if (shouldFail || isCleanup) return Promise.reject(new Error(`rejected ${request.path}`));
      return Promise.resolve({});
    };
    return { records, dispatch };
  }

  it("runs a successful built-in as exactly the five normal requests with no cleanup", async () => {
    // Given: a built-in snapshot and an always-succeeding dispatch
    const { records, dispatch } = scriptedDispatch();

    // When: executing to completion
    const outcome = await executeTaskRun(BUILT_IN_SNAPSHOT, { dispatch, sleep: instantSleep });

    // Then: exactly the five normal requests ran, in order, and no cleanup followed
    expect(outcome).toEqual({ kind: "success", completed: 5 });
    expect(records.map((record) => `${record.path} ${record.body}`)).toEqual([
      '/api/v1/gpio/CON_MAS {"direction":"input"}',
      '/api/v1/power/5v_out {"state":"off"}',
      '/api/v1/gpio/CON_MAS {"direction":"output","value":0}',
      '/api/v1/power/5v_out {"state":"on"}',
      '/api/v1/gpio/CON_MAS {"direction":"input"}',
    ]);
  });

  for (const failAt of [1, 2, 3, 4, 5] as const) {
    it(`failure at built-in request ${failAt} preserves the failure and runs exactly one CON_MAS cleanup`, async () => {
      // Given: a dispatch that rejects normal request `failAt`
      const { records, dispatch } = scriptedDispatch(failAt);

      // When: executing
      const outcome = await executeTaskRun(BUILT_IN_SNAPSHOT, { dispatch, sleep: instantSleep });

      // Then: the primary failure keeps its index and path
      expect(outcome.kind).toBe("failed");
      if (outcome.kind !== "failed") throw new Error("unreachable");
      expect(outcome.failedIndex).toBe(failAt);
      expect(outcome.failedPath).toBe(MASKROM_5V_REQUESTS[failAt - 1]?.path);
      expect(outcome.completed).toBe(failAt - 1);
      expect(outcome.error).toContain("rejected");
      // And: exactly one cleanup PUT releases CON_MAS; no rail compensation follows
      expect(outcome.cleanup).toEqual({ attempted: true, ok: true });
      const normal = records.slice(0, failAt);
      const cleanup = records.slice(failAt);
      expect(cleanup).toHaveLength(1);
      expect(cleanup[0]?.path).toBe("/api/v1/gpio/CON_MAS");
      expect(cleanup[0]?.body).toBe('{"direction":"input"}');
      expect(normal.some((record) => record.path.includes("/power/") && records.indexOf(record) >= failAt)).toBe(false);
    });
  }

  it("keeps the primary failure authoritative when cleanup also fails", async () => {
    // Given: request 2 fails and the cleanup PUT fails too
    const { records, dispatch } = scriptedDispatch(2, true);

    // When: executing
    const outcome = await executeTaskRun(BUILT_IN_SNAPSHOT, { dispatch, sleep: instantSleep });

    // Then: the primary failure is preserved and the cleanup diagnostic is appended
    expect(outcome.kind).toBe("failed");
    if (outcome.kind !== "failed") throw new Error("unreachable");
    expect(outcome.failedIndex).toBe(2);
    expect(outcome.failedPath).toBe("/api/v1/power/5v_out");
    expect(outcome.cleanup?.attempted).toBe(true);
    expect(outcome.cleanup?.ok).toBe(false);
    expect(outcome.cleanup?.error).toContain("rejected");
    expect(records).toHaveLength(3);
  });

  it("never infers cleanup for stored tasks on failure", async () => {
    // Given: a stored snapshot whose first request fails
    const { records, dispatch } = scriptedDispatch(1);

    // When: executing
    const outcome = await executeTaskRun(STORED_SNAPSHOT, { dispatch, sleep: instantSleep });

    // Then: no cleanup is attempted
    expect(outcome.kind).toBe("failed");
    if (outcome.kind !== "failed") throw new Error("unreachable");
    expect(outcome.cleanup).toBeNull();
    expect(records).toHaveLength(1);
  });

  it("aborts before the first dispatch with zero requests and no cleanup", async () => {
    // Given: an already-aborted run signal
    const controller = new AbortController();
    controller.abort();
    const { records, dispatch } = scriptedDispatch();

    // When: executing
    const outcome = await executeTaskRun(BUILT_IN_SNAPSHOT, {
      dispatch,
      sleep: instantSleep,
      signal: controller.signal,
    });

    // Then: nothing was dispatched and no cleanup ran
    expect(outcome).toEqual({ kind: "cancelled", completed: 0, cleanup: null });
    expect(records).toHaveLength(0);
  });

  it("aborts during a wait, stops later records, and cleans up on a fresh context", async () => {
    // Given: a run that aborts while waiting after the second request
    const controller = new AbortController();
    const { records, dispatch } = scriptedDispatch();
    const sleep = () => {
      controller.abort();
      return new Promise<void>(() => {});
    };

    // When: executing
    const outcome = await executeTaskRun(BUILT_IN_SNAPSHOT, {
      dispatch,
      sleep,
      signal: controller.signal,
    });

    // Then: the run reports partial cancellation and one cleanup on a non-aborted context
    expect(outcome.kind).toBe("cancelled");
    if (outcome.kind !== "cancelled") throw new Error("unreachable");
    expect(outcome.completed).toBe(2);
    expect(outcome.cleanup).toEqual({ attempted: true, ok: true });
    expect(records).toHaveLength(3);
    expect(records[2]?.path).toBe("/api/v1/gpio/CON_MAS");
    expect(records[2]?.body).toBe('{"direction":"input"}');
    expect(records[2]?.signal?.aborted).toBe(false);
    expect(records[2]?.signal).not.toBe(controller.signal);
  });

  it("treats an aborted in-flight dispatch as partial cancellation, not rollback", async () => {
    // Given: the second request rejects because the run signal aborted mid-flight
    const controller = new AbortController();
    let count = 0;
    const records: DispatchRecord[] = [];
    const dispatch = (request: TaskRequest, signal?: AbortSignal): Promise<unknown> => {
      records.push({ path: request.path, body: request.body, signal });
      count += 1;
      if (count === 2) {
        controller.abort();
        return Promise.reject(new DOMException("The operation was aborted.", "AbortError"));
      }
      return Promise.resolve({});
    };

    // When: executing
    const outcome = await executeTaskRun(BUILT_IN_SNAPSHOT, {
      dispatch,
      sleep: instantSleep,
      signal: controller.signal,
    });

    // Then: cancellation is reported with the in-flight request uncounted and cleanup attempted
    expect(outcome.kind).toBe("cancelled");
    if (outcome.kind !== "cancelled") throw new Error("unreachable");
    expect(outcome.completed).toBe(1);
    expect(outcome.cleanup).toEqual({ attempted: true, ok: true });
    expect(records.map((record) => record.path)).toEqual([
      "/api/v1/gpio/CON_MAS",
      "/api/v1/power/5v_out",
      "/api/v1/gpio/CON_MAS",
    ]);
  });

  it("never cleans up a stored task cancelled during a wait", async () => {
    // Given: a stored run aborted while waiting after the first request
    const controller = new AbortController();
    const { records, dispatch } = scriptedDispatch();
    const sleep = () => {
      controller.abort();
      return new Promise<void>(() => {});
    };

    // When: executing
    const outcome = await executeTaskRun(STORED_SNAPSHOT, {
      dispatch,
      sleep,
      signal: controller.signal,
    });

    // Then: cancellation is reported without any cleanup
    expect(outcome).toEqual({ kind: "cancelled", completed: 1, cleanup: null });
    expect(records).toHaveLength(1);
  });
});
