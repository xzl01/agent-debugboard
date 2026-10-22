import { act } from "react";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import {
  automationLockOwner,
  confirmCalls,
  fetchCalls,
  flush,
  flushUntil,
  holdAutomationLock,
  mountTaskCard,
  putCalls,
  runButton,
  setupTaskCardHarness,
  stubConfirmResult,
  stubPutFailure,
  stubTaskList,
  storedListPayload,
  storeButton,
  taskListGets,
  teardownTaskCardHarness,
  TWO_RECORD_BLOB,
} from "./TaskCard.testUtils";

beforeEach(setupTaskCardHarness);
afterEach(teardownTaskCardHarness);

describe("TaskCard stored-task execution", () => {
  it("executes the selected task records in order and shows a localized success", async () => {
    // Given: a stored task with two request records
    stubTaskList(storedListPayload([{ id: "t1", name: "t1", request_count: 2 }], TWO_RECORD_BLOB));
    const view = await mountTaskCard();

    // When: running the task
    act(() => runButton(view, "t1").click());
    await flushUntil(() => putCalls().length === 2);

    // Then: both records went to the normal board APIs in blob order
    const puts = putCalls();
    expect(puts.map((call) => `${call.url} ${call.body ?? ""}`)).toEqual([
      '/api/v1/power/5v_out {"state":"off"}',
      '/api/v1/gpio/CON_MAS {"direction":"input"}',
    ]);
    // And: no wait metadata ever reaches a control endpoint
    for (const put of puts) expect(put.body).not.toContain("wait_ms");
    // And: the card reports completion
    expect(view.textContent).toContain("2");
  });

  it("stops at the first failed record and renders the failed path", async () => {
    // Given: a three-record task whose second request is rejected by the board
    stubTaskList(storedListPayload(
      [{ id: "t1", name: "t1", request_count: 3 }],
      [
        "# linkr-task.v1",
        "# task t1",
        '{"method":"PUT","path":"/api/v1/power/5v_out","body":"{\\"state\\":\\"off\\"}"}',
        '{"method":"PUT","path":"/api/v1/gpio/CON_REST","body":"{\\"direction\\":\\"output\\",\\"value\\":1}"}',
        '{"method":"PUT","path":"/api/v1/switch/sd","body":"{\\"route\\":\\"target\\"}"}',
        "",
      ].join("\n"),
    ));
    stubPutFailure("/api/v1/gpio/CON_REST", 400, "gpio rejected");
    const view = await mountTaskCard();

    // When: running the task
    act(() => runButton(view, "t1").click());
    await flushUntil(() => view.textContent?.includes("gpio rejected") === true);

    // Then: the third record is never dispatched
    expect(putCalls().map((call) => call.url)).toEqual([
      "/api/v1/power/5v_out",
      "/api/v1/gpio/CON_REST",
    ]);
    // And: the failure names the failed request path and the board error
    expect(view.textContent).toContain("/api/v1/gpio/CON_REST");
    expect(view.textContent).toContain("gpio rejected");
  });

  it("shows NVS occupancy and disables task saves below the reserve", async () => {
    stubTaskList({
      ...storedListPayload([], ""),
      storage: {
        total_bytes: 131072,
        used_bytes: 8192,
        free_bytes: 122880,
        task_bytes: 1024,
        reserve_bytes: 8192,
        measured: true,
        save_allowed: false,
      },
    });
    const view = await mountTaskCard();

    expect(view.textContent).toContain("NVS 8.0 KiB used");
    expect(view.textContent).toContain("120.0 KiB free");
    expect(view.textContent).toContain("task data 1.0 KiB");
    expect(storeButton(view).disabled).toBe(true);
    expect(view.textContent).toContain("save reserve");
  });

  it("shows a visible error when the stored blob is malformed", async () => {
    // Given: a summary that lists a task but a blob that is not valid task data
    stubTaskList(storedListPayload(
      [{ id: "t1", name: "t1", request_count: 1 }],
      "# linkr-orch.v1\n# task t1\n",
    ));
    const view = await mountTaskCard();

    // When: running the task
    act(() => runButton(view, "t1").click());
    await flushUntil(() => view.textContent?.includes("linkr-task.v1") === true);

    // Then: no control request is dispatched and the error is visible
    expect(putCalls()).toEqual([]);
    expect(view.textContent).toContain("linkr-task.v1");
  });

  it("renders no boot controls or boot badges for stored tasks", async () => {
    // Given: a stored task
    stubTaskList(storedListPayload([{ id: "t1", name: "t1", request_count: 2 }], TWO_RECORD_BLOB));
    const view = await mountTaskCard();

    // Then: only the run control is offered, nothing boot-related
    expect(view.textContent).toContain("Run task");
    expect(view.textContent?.toLowerCase()).not.toContain("boot");
  });

  it("offers no set-default, clear-default, or automatic-execution controls", async () => {
    // Given: a stored task
    stubTaskList(storedListPayload([{ id: "t1", name: "t1", request_count: 2 }], TWO_RECORD_BLOB));
    const view = await mountTaskCard();

    // Then: every control is on-demand; nothing stores defaults or schedules execution
    expect(view.textContent?.toLowerCase()).not.toMatch(
      /set[- ]default|clear[- ]default|auto[- ]?run|automatic|run on boot/,
    );
  });

  it("fetches tasks from the task API and shows the empty state", async () => {
    // Given: empty storage
    const view = await mountTaskCard();

    // Then: the task API was queried and the empty state is visible
    expect(taskListGets()).toHaveLength(1);
    expect(view.textContent).toContain("No tasks stored in flash.");
  });

  it("stores current workflow source and IR through an atomic task upsert", async () => {
    // Given: the mounted card with a task id entered
    const view = await mountTaskCard();
    const input = view.querySelector("input");
    if (!input) throw new TypeError("task id input not found");
    act(() => {
      const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")?.set;
      setter?.call(input, "my-task");
      input.dispatchEvent(new Event("input", { bubbles: true }));
    });

    // When: storing the current workflow
    const store = [...view.querySelectorAll("button")].find(
      (candidate) => candidate.textContent?.trim() === "Store current workflow",
    );
    if (!store) throw new TypeError("store button not found");
    act(() => store.click());
    await flushUntil(() => fetchCalls().some((call) => call.method === "POST" && call.url === "/api/v1/tasks/mutations"));

    // Then: the task API received a linkr-task.v1 blob for that id
    const put = fetchCalls().find((call) => call.method === "POST" && call.url === "/api/v1/tasks/mutations");
    const stored = JSON.parse(put!.body!) as Record<string, string>;
    expect(stored.action).toBe("upsert");
    expect(Buffer.from(stored.source_base64!, "base64").toString("utf8")).toContain('"schema":"linkr-test.v1"');
    expect(Buffer.from(stored.ir_base64!, "base64").toString("utf8")).toContain("# linkr-task.v1\n# task my-task\n");
  });
});

describe("TaskCard run confirmation", () => {
  it("sends zero control requests when a built-in run is rejected at confirmation", async () => {
    // Given: a mounted card and a user who rejects the confirmation
    const view = await mountTaskCard();
    stubConfirmResult(false);

    // When: running a built-in
    act(() => runButton(view, "MASKROM via 5v_out").click());
    await flush();

    // Then: the confirmation was shown and no control or storage request went out
    expect(confirmCalls()).toHaveLength(1);
    expect(putCalls()).toEqual([]);
    expect(taskListGets()).toHaveLength(1);
    expect(automationLockOwner()).toBeNull();
  });

  it("sends zero control requests when a stored run is rejected after its fetch and parse", async () => {
    // Given: a stored task and a rejecting user
    stubTaskList(storedListPayload([{ id: "t1", name: "t1", request_count: 2 }], TWO_RECORD_BLOB));
    const view = await mountTaskCard();
    stubConfirmResult(false);
    const getsBefore = taskListGets().length;

    // When: running the stored task
    act(() => runButton(view, "t1").click());
    await flushUntil(() => confirmCalls().length === 1);

    // Then: the snapshot was fetched and parsed, but no control PUT was sent
    expect(taskListGets().length).toBe(getsBefore + 1);
    expect(confirmCalls()).toHaveLength(1);
    expect(putCalls()).toEqual([]);
    expect(automationLockOwner()).toBeNull();
  });

  it("binds the confirmation text to the resolved snapshot with a non-transactional warning", async () => {
    // Given: a stored two-record task
    stubTaskList(storedListPayload([{ id: "t1", name: "t1", request_count: 2 }], TWO_RECORD_BLOB));
    const view = await mountTaskCard();

    // When: running the stored task
    act(() => runButton(view, "t1").click());
    await flushUntil(() => putCalls().length === 2);

    // Then: the confirmation names the id, source, request count, and hardware warning
    expect(confirmCalls()).toHaveLength(1);
    const message = confirmCalls()[0] ?? "";
    expect(message).toContain("t1");
    expect(message).toContain("Stored");
    expect(message).toContain("2");
    expect(message.toLowerCase()).toContain("not transactional");

    // And: execution used the same in-memory snapshot without refetching after confirmation
    expect(putCalls().map((call) => call.url)).toEqual([
      "/api/v1/power/5v_out",
      "/api/v1/gpio/CON_MAS",
    ]);
    expect(taskListGets()).toHaveLength(2);
  });

  it("shows the built-in source and frozen request count in the confirmation", async () => {
    // Given: the mounted card
    const view = await mountTaskCard();

    // When: running an EDL built-in
    act(() => runButton(view, "EDL via 12v_out").click());
    await flushUntil(() => putCalls().length === 5);

    // Then: the confirmation described the built-in snapshot
    const message = confirmCalls()[0] ?? "";
    expect(message).toContain("builtin/edl/12v_out");
    expect(message).toContain("Built-in");
    expect(message).toContain("5");
  });

  it("shows a localized busy error and sends nothing when another owner holds the automation lock", async () => {
    // Given: the startup automation owns the lock
    const view = await mountTaskCard();
    holdAutomationLock("startup");

    // When: running a built-in
    act(() => runButton(view, "MASKROM via 5v_out").click());
    await flush();

    // Then: confirmation happened, but the lock rejected execution before any PUT
    expect(confirmCalls()).toHaveLength(1);
    expect(putCalls()).toEqual([]);
    expect(view.textContent).toContain("Another automation task is controlling the debugger.");
    expect(automationLockOwner()).toBe("startup");
  });
});
