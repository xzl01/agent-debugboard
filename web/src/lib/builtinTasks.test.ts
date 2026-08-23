import { describe, expect, it } from "vitest";
import {
  findBuiltInTask,
  mergeTaskCatalog,
  parseTaskCatalogResponse,
  TASK_CATALOG_MAX_TASKS,
} from "./builtinTasks.ts";
import { TASK_ID_MAX_BYTES } from "./taskRunner.ts";
import type { TaskSummary } from "./taskRunner.ts";
import { TaskDataError } from "./taskBlob.ts";

const textEncoder = new TextEncoder();

interface CatalogRequestSpec {
  readonly path: string;
  readonly body: Record<string, unknown>;
  readonly wait_ms?: number;
}

function catalogRequest(spec: CatalogRequestSpec): Record<string, unknown> {
  return { method: "PUT", path: spec.path, body: spec.body, wait_ms: spec.wait_ms ?? 0 };
}

function catalogTask(id: string, overrides?: Record<string, unknown>): Record<string, unknown> {
  const gpioInput = { direction: "input" };
  return {
    id,
    name: `catalog ${id}`,
    requests: [
      catalogRequest({ path: "/api/v1/gpio/CON_MAS", body: gpioInput }),
      catalogRequest({ path: "/api/v1/power/5v_out", body: { state: "off" }, wait_ms: 1000 }),
    ],
    cleanup: catalogRequest({ path: "/api/v1/gpio/CON_MAS", body: gpioInput }),
    ...overrides,
  };
}

function catalogEnvelope(tasks: readonly Record<string, unknown>[], overrides?: Record<string, unknown>) {
  return {
    schema: "radxa-linkr-debugger.v1",
    ok: true,
    command: "task",
    action: "catalog",
    version: 1,
    tasks: [...tasks],
    ...overrides,
  };
}

function expectInvalid(data: unknown, fragment: string): void {
  try {
    parseTaskCatalogResponse(data);
  } catch (cause) {
    expect(cause).toBeInstanceOf(TaskDataError);
    expect((cause as TaskDataError).code).toBe("invalid_response");
    expect((cause as TaskDataError).detail).toContain(fragment);
    return;
  }
  throw new Error(`expected catalog parse to reject: ${fragment}`);
}

describe("parseTaskCatalogResponse", () => {
  it("parses a valid catalog and canonicalizes object bodies to JSON strings", () => {
    // Given: a catalog whose wire bodies are JSON objects
    const parsed = parseTaskCatalogResponse(catalogEnvelope([
      catalogTask("builtin/maskrom/5v_out"),
      catalogTask("builtin/edl/20v_out"),
    ]));

    // Then: each task carries typed requests with canonical string bodies
    expect(parsed).toHaveLength(2);
    const first = parsed[0];
    if (!first) throw new Error("missing first task");
    expect(first.id).toBe("builtin/maskrom/5v_out");
    expect(first.requests[0]?.body).toBe('{"direction":"input"}');
    expect(first.requests[1]?.body).toBe('{"state":"off"}');
    expect(first.requests[1]?.wait_ms).toBe(1000);
    expect(first.cleanup.body).toBe('{"direction":"input"}');
  });

  it("rejects a non-object envelope", () => {
    expectInvalid(null, "JSON object");
    expectInvalid([1], "JSON object");
    expectInvalid("catalog", "JSON object");
  });

  it("rejects a wrong schema, command, action, or version", () => {
    expectInvalid(catalogEnvelope([catalogTask("a")], { schema: "other.v1" }), "schema");
    expectInvalid(catalogEnvelope([catalogTask("a")], { ok: false }), "ok");
    expectInvalid(catalogEnvelope([catalogTask("a")], { command: "power" }), "command");
    expectInvalid(catalogEnvelope([catalogTask("a")], { action: "list" }), "action");
    expectInvalid(catalogEnvelope([catalogTask("a")], { version: 2 }), "version");
    expectInvalid(catalogEnvelope([catalogTask("a")], { version: "1" }), "version");
  });

  it("rejects a missing, empty, or oversized tasks array", () => {
    expectInvalid(catalogEnvelope([], { tasks: undefined as unknown as never }), "tasks");
    expectInvalid(catalogEnvelope([]), "tasks");
    const tooMany = Array.from({ length: TASK_CATALOG_MAX_TASKS + 1 }, (_, index) =>
      catalogTask(`t${index}`),
    );
    expectInvalid(catalogEnvelope(tooMany), `${TASK_CATALOG_MAX_TASKS}`);
  });

  it("rejects duplicate task IDs", () => {
    expectInvalid(
      catalogEnvelope([catalogTask("dup"), catalogTask("dup")]),
      "duplicate",
    );
  });

  it("rejects invalid task IDs before any dispatch", () => {
    expectInvalid(catalogEnvelope([catalogTask("")]), "id");
    expectInvalid(catalogEnvelope([catalogTask("has space")]), "id");
    expectInvalid(catalogEnvelope([catalogTask("x".repeat(TASK_ID_MAX_BYTES + 1))]), "id");
  });

  it("rejects unknown top-level envelope fields", () => {
    expectInvalid(catalogEnvelope([catalogTask("a")], { extra: true }), "unsupported");
    expectInvalid(catalogEnvelope([catalogTask("a")], { blob: "" }), "unsupported");
  });

  it("rejects unknown fields on tasks and requests", () => {
    expectInvalid(
      catalogEnvelope([catalogTask("a", { extra: true })]),
      "unsupported",
    );
    expectInvalid(
      catalogEnvelope([
        catalogTask("a", {
          requests: [{ ...catalogRequest({ path: "/api/v1/gpio/CON_MAS", body: {} }), hack: 1 }],
        }),
      ]),
      "unsupported",
    );
  });

  it("rejects non-PUT methods and disallowed paths", () => {
    expectInvalid(
      catalogEnvelope([
        catalogTask("a", {
          requests: [{ method: "POST", path: "/api/v1/gpio/CON_MAS", body: {}, wait_ms: 0 }],
        }),
      ]),
      "PUT",
    );
    expectInvalid(
      catalogEnvelope([
        catalogTask("a", {
          requests: [catalogRequest({ path: "/api/v1/ota", body: {} })],
        }),
      ]),
      "control path",
    );
  });

  it("rejects malformed or non-object bodies", () => {
    expectInvalid(
      catalogEnvelope([
        catalogTask("a", {
          requests: [{ method: "PUT", path: "/api/v1/gpio/CON_MAS", body: "not-an-object", wait_ms: 0 }],
        }),
      ]),
      "body",
    );
    expectInvalid(
      catalogEnvelope([
        catalogTask("a", {
          requests: [{ method: "PUT", path: "/api/v1/gpio/CON_MAS", body: [1, 2], wait_ms: 0 }],
        }),
      ]),
      "body",
    );
  });

  it("rejects out-of-range and non-integer waits", () => {
    expectInvalid(
      catalogEnvelope([
        catalogTask("a", {
          requests: [catalogRequest({ path: "/api/v1/gpio/CON_MAS", body: {}, wait_ms: -1 })],
        }),
      ]),
      "wait_ms",
    );
    expectInvalid(
      catalogEnvelope([
        catalogTask("a", {
          requests: [catalogRequest({ path: "/api/v1/gpio/CON_MAS", body: {}, wait_ms: 60_001 })],
        }),
      ]),
      "wait_ms",
    );
    expectInvalid(
      catalogEnvelope([
        catalogTask("a", {
          requests: [catalogRequest({ path: "/api/v1/gpio/CON_MAS", body: {}, wait_ms: 0.5 })],
        }),
      ]),
      "wait_ms",
    );
  });

  it("rejects empty and oversized request lists", () => {
    expectInvalid(catalogEnvelope([catalogTask("a", { requests: [] })]), "requests");
    const oversized = Array.from({ length: 33 }, () =>
      catalogRequest({ path: "/api/v1/gpio/CON_MAS", body: {} }),
    );
    expectInvalid(catalogEnvelope([catalogTask("a", { requests: oversized })]), "32");
  });

  it("rejects an absent cleanup", () => {
    expectInvalid(
      catalogEnvelope([catalogTask("a", { cleanup: undefined as unknown as never })]),
      "cleanup",
    );
  });

  it("rejects request and cleanup bodies deeper than the shared JSON depth limit", () => {
    // Given: bodies nested one level past the shared TASK_MAX_JSON_DEPTH limit
    const wrap = (levels: number): Record<string, unknown> => {
      let body: Record<string, unknown> = {};
      for (let index = 0; index < levels; index += 1) body = { nest: body };
      return body;
    };
    // When/Then: 17 levels are rejected for both requests and cleanup
    expectInvalid(
      catalogEnvelope([
        catalogTask("a", {
          requests: [catalogRequest({ path: "/api/v1/gpio/CON_MAS", body: wrap(16) })],
        }),
      ]),
      "depth",
    );
    expectInvalid(
      catalogEnvelope([catalogTask("a", { cleanup: catalogRequest({ path: "/api/v1/gpio/CON_MAS", body: wrap(16) }) })]),
      "depth",
    );
    // And: the boundary body at exactly 16 levels is still accepted
    const parsed = parseTaskCatalogResponse(catalogEnvelope([
      catalogTask("a", {
        requests: [catalogRequest({ path: "/api/v1/gpio/CON_MAS", body: wrap(15) })],
      }),
    ]));
    expect(parsed).toHaveLength(1);
  });
});

describe("findBuiltInTask", () => {
  const catalog = parseTaskCatalogResponse(catalogEnvelope([
    catalogTask("builtin/maskrom/5v_out"),
    catalogTask("builtin/edl/12v_out"),
  ]));

  it("resolves catalog tasks by exact ID only", () => {
    expect(findBuiltInTask(catalog, "builtin/maskrom/5v_out")?.id).toBe("builtin/maskrom/5v_out");
    expect(findBuiltInTask(catalog, "builtin/maskrom/3v3_out")).toBeUndefined();
    expect(findBuiltInTask(catalog, "")).toBeUndefined();
  });

  it("keeps every parsed task ID within the firmware 31-byte limit", () => {
    for (const task of catalog) {
      expect(textEncoder.encode(task.id).length).toBeLessThanOrEqual(TASK_ID_MAX_BYTES);
    }
  });
});

describe("mergeTaskCatalog", () => {
  const stored = (id: string): TaskSummary => ({ id, name: id, request_count: 1 });
  const builtIns = parseTaskCatalogResponse(catalogEnvelope([
    catalogTask("builtin/maskrom/5v_out"),
    catalogTask("builtin/edl/20v_out", { name: "EDL 20V" }),
  ]));

  it("lists catalog built-ins before stored tasks", () => {
    // Given: two catalog built-ins and two stored tasks
    const catalog = mergeTaskCatalog(builtIns, [stored("alpha"), stored("beta")]);

    // Then: built-ins first in catalog order, then stored tasks in firmware order
    expect(catalog.map((entry) => entry.id)).toEqual([
      "builtin/maskrom/5v_out",
      "builtin/edl/20v_out",
      "alpha",
      "beta",
    ]);
    expect(catalog[1]?.name).toBe("EDL 20V");
    expect(catalog[0]?.requestCount).toBe(2);
  });

  it("drops colliding stored tasks and flags the shadowing built-in", () => {
    // Given: a stored task whose ID collides with a catalog built-in
    const catalog = mergeTaskCatalog(builtIns, [stored("builtin/edl/20v_out"), stored("alpha")]);

    // Then: the stored duplicate is hidden and the built-in carries the shadow flag
    expect(catalog.map((entry) => entry.id)).toEqual([
      "builtin/maskrom/5v_out",
      "builtin/edl/20v_out",
      "alpha",
    ]);
    expect(catalog[1]).toMatchObject({ source: "builtin", shadowedStored: true });
    expect(catalog[0]).toMatchObject({ shadowedStored: false });
  });

  it("lists only stored tasks when the catalog is empty", () => {
    const catalog = mergeTaskCatalog([], [stored("alpha")]);
    expect(catalog.map((entry) => entry.id)).toEqual(["alpha"]);
    expect(catalog[0]?.source).toBe("stored");
  });

  it("hides every stored builtin/ id when the catalog is unavailable", () => {
    // Given: a failed catalog fetch and stored tasks inside the reserved builtin/ namespace
    const catalog = mergeTaskCatalog(null, [stored("builtin/maskrom/5v_out"), stored("builtin/rogue"), stored("alpha")]);

    // Then: no stored builtin/ id is displayed, non-builtin stored tasks stay listed
    expect(catalog.map((entry) => entry.id)).toEqual(["alpha"]);
  });

  it("hides stored builtin/ ids absent from an available catalog", () => {
    // Given: an available catalog and a stored id impersonating the builtin/ namespace
    const catalog = mergeTaskCatalog(builtIns, [stored("builtin/rogue"), stored("alpha")]);

    // Then: the impersonating stored id is hidden; real catalog built-ins are untouched
    expect(catalog.map((entry) => entry.id)).toEqual([
      "builtin/maskrom/5v_out",
      "builtin/edl/20v_out",
      "alpha",
    ]);
  });
});
