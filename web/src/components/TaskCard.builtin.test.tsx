import { act } from "react";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import {
  catalogRowIds,
  flushUntil,
  mountTaskCard,
  putCalls,
  rowFor,
  runButton,
  setupTaskCardHarness,
  stubTaskCatalog,
  stubTaskList,
  storedListPayload,
  taskListGets,
  teardownTaskCardHarness,
  TWO_RECORD_BLOB,
} from "./TaskCard.testUtils";

beforeEach(setupTaskCardHarness);
afterEach(teardownTaskCardHarness);

const BUILT_IN_ORDER = [
  "builtin/maskrom/5v_out",
  "builtin/maskrom/12v_out",
  "builtin/maskrom/20v_out",
  "builtin/edl/5v_out",
  "builtin/edl/12v_out",
  "builtin/edl/20v_out",
] as const;

describe("TaskCard built-in execution", () => {
  it("runs a MASKROM built-in through the generic request path without touching task storage", async () => {
    // Given: empty storage and the mounted card
    const view = await mountTaskCard();
    const getsBefore = taskListGets().length;

    // When: running the 5v_out MASKROM built-in
    act(() => runButton(view, "MASKROM via 5v_out").click());
    await flushUntil(() => putCalls().length === 5);

    // Then: exactly the five frozen requests hit the plain board APIs in order
    const puts = putCalls();
    expect(puts.map((call) => `${call.url} ${call.body ?? ""}`)).toEqual([
      '/api/v1/gpio/CON_MAS {"direction":"input"}',
      '/api/v1/power/5v_out {"state":"off"}',
      '/api/v1/gpio/CON_MAS {"direction":"output","value":0}',
      '/api/v1/power/5v_out {"state":"on"}',
      '/api/v1/gpio/CON_MAS {"direction":"input"}',
    ]);
    // And: running the built-in never reads, writes, or clears task storage
    expect(taskListGets()).toHaveLength(getsBefore);
    // And: the run reports all five requests completed
    expect(view.textContent).toContain("5");
  });

  it("runs an EDL built-in with CON_MAS driven high against the selected rail", async () => {
    // Given: the mounted card
    const view = await mountTaskCard();

    // When: running the 12v_out EDL built-in
    act(() => runButton(view, "EDL via 12v_out").click());
    await flushUntil(() => putCalls().length === 5);

    // Then: the assert request drives CON_MAS high and the rail requests target 12v_out
    const puts = putCalls();
    expect(puts.map((call) => call.url)).toEqual([
      "/api/v1/gpio/CON_MAS",
      "/api/v1/power/12v_out",
      "/api/v1/gpio/CON_MAS",
      "/api/v1/power/12v_out",
      "/api/v1/gpio/CON_MAS",
    ]);
    expect(puts[2]?.body).toBe('{"direction":"output","value":1}');
  });

  it("gives the built-in precedence over a stored task with the same ID", async () => {
    // Given: stored flash data whose ID collides with a built-in
    stubTaskList(storedListPayload(
      [{ id: "builtin/maskrom/5v_out", name: "evil-twin", request_count: 1 }],
      [
        "# linkr-task.v1",
        "# task builtin/maskrom/5v_out",
        '{"method":"PUT","path":"/api/v1/switch/sd","body":"{\\"route\\":\\"target\\"}"}',
        "",
      ].join("\n"),
    ));
    const view = await mountTaskCard();

    // Then: exactly one row carries the colliding ID and it flags the shadowed stored task
    const rows = [...view.querySelectorAll<HTMLElement>("div.rounded-lg")].filter((candidate) =>
      candidate.textContent?.includes("builtin/maskrom/5v_out"),
    );
    expect(rows).toHaveLength(1);
    expect(rows[0]?.textContent).toContain("Built-in");
    expect(rows[0]?.textContent).not.toContain("evil-twin");
    expect(rows[0]?.textContent?.toLowerCase()).toContain("shadow");

    // When: running that row
    act(() => runButton(view, "MASKROM via 5v_out").click());
    await flushUntil(() => putCalls().length === 5);

    // Then: the built-in sequence ran; the shadowed stored record was never dispatched
    const puts = putCalls();
    expect(puts.some((call) => call.url === "/api/v1/switch/sd")).toBe(false);
    expect(puts).toHaveLength(5);
  });

  it("lists built-ins before stored tasks", async () => {
    // Given: one stored task
    stubTaskList(storedListPayload([{ id: "t1", name: "t1", request_count: 2 }], TWO_RECORD_BLOB));
    const view = await mountTaskCard();

    // Then: the merged order is six built-ins followed by the stored task
    expect(catalogRowIds(view)).toEqual([...BUILT_IN_ORDER, "t1"]);
  });

  it("shows the colliding stored task as shadowed only on the matching built-in row", async () => {
    // Given: a stored task colliding with the EDL 20v_out built-in
    stubTaskList(storedListPayload(
      [{ id: "builtin/edl/20v_out", name: "x", request_count: 1 }],
      TWO_RECORD_BLOB,
    ));
    const view = await mountTaskCard();

    // Then: the matching row flags the shadow and the other built-ins do not
    expect(rowFor(view, "EDL via 20v_out").textContent?.toLowerCase()).toContain("shadow");
    expect(rowFor(view, "MASKROM via 5v_out").textContent?.toLowerCase()).not.toContain("shadow");
    expect(catalogRowIds(view)).toEqual([...BUILT_IN_ORDER]);
  });
});

describe("TaskCard catalog failure isolation", () => {
  it("disables built-ins with a visible error but keeps stored tasks usable when the catalog fetch fails", async () => {
    // Given: a board whose catalog endpoint fails but whose task list works
    stubTaskCatalog({ ok: false, error: { code: "rejected", message: "boom" } }, 500);
    stubTaskList(storedListPayload([{ id: "t1", name: "t1", request_count: 2 }], TWO_RECORD_BLOB));
    const view = await mountTaskCard();

    // Then: only the stored row is listed and a structured catalog error is visible
    expect(catalogRowIds(view)).toEqual(["t1"]);
    expect(view.textContent).toContain("Built-in task catalog unavailable");

    // When: running the stored task
    act(() => runButton(view, "t1").click());
    await flushUntil(() => putCalls().length === 2);

    // Then: the stored task executed normally
    expect(putCalls().map((call) => call.url)).toEqual([
      "/api/v1/power/5v_out",
      "/api/v1/gpio/CON_MAS",
    ]);
  });

  it("treats a malformed catalog as unavailable without affecting stored tasks", async () => {
    // Given: a catalog response with the wrong contract version
    stubTaskCatalog({
      schema: "radxa-linkr-debugger.v1",
      ok: true,
      command: "task",
      action: "catalog",
      version: 2,
      tasks: [],
    });
    stubTaskList(storedListPayload([{ id: "t1", name: "t1", request_count: 2 }], TWO_RECORD_BLOB));
    const view = await mountTaskCard();

    // Then: built-ins are absent and the catalog error names the version problem
    expect(catalogRowIds(view)).toEqual(["t1"]);
    expect(view.textContent).toContain("Built-in task catalog unavailable");
    expect(view.textContent).toContain("version");
  });

  it("hides stored builtin/ ids and never runs them when the catalog fetch fails", async () => {
    // Given: a failed catalog and a stored blob impersonating the builtin/ namespace
    stubTaskCatalog({ ok: false, error: { code: "rejected", message: "boom" } }, 500);
    stubTaskList(storedListPayload(
      [
        { id: "builtin/maskrom/5v_out", name: "evil-twin", request_count: 1 },
        { id: "t1", name: "t1", request_count: 2 },
      ],
      [
        "# linkr-task.v1",
        "# task builtin/maskrom/5v_out",
        '# task t1',
        '{"method":"PUT","path":"/api/v1/switch/sd","body":"{\\"route\\":\\"target\\"}"}',
        "",
      ].join("\n"),
    ));
    const view = await mountTaskCard();

    // Then: the impersonating row is hidden and only the plain stored task remains
    expect(catalogRowIds(view)).toEqual(["t1"]);
    expect(view.textContent).not.toContain("evil-twin");
    expect(view.textContent).toContain("Built-in task catalog unavailable");
  });
});
