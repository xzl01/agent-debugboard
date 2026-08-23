import { act } from "react";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import {
  automationLockOwner,
  cancelButton,
  deferNextPutResponse,
  flush,
  flushUntil,
  mountTaskCard,
  putCalls,
  runButton,
  setupTaskCardHarness,
  stubTaskList,
  storedListPayload,
  teardownTaskCardHarness,
  unmountTaskCard,
} from "./TaskCard.testUtils";

beforeEach(setupTaskCardHarness);
afterEach(teardownTaskCardHarness);

const STORED_WITH_WAIT_BLOB = [
  "# linkr-task.v1",
  "# task t1",
  '{"method":"PUT","path":"/api/v1/power/5v_out","body":"{\\"state\\":\\"off\\"}","wait_ms":5000}',
  '{"method":"PUT","path":"/api/v1/gpio/CON_MAS","body":"{\\"direction\\":\\"input\\"}"}',
  "",
].join("\n");

describe("TaskCard run cancellation lifecycle", () => {
  it("shows the cancel control only while a task is executing and disables other runs", async () => {
    // Given: the mounted card with no run in progress
    const view = await mountTaskCard();
    const hasCancel = () =>
      [...view.querySelectorAll("button")].some((button) => button.textContent?.trim() === "Cancel");
    expect(hasCancel()).toBe(false);

    // When: a built-in run is in progress
    act(() => runButton(view, "MASKROM via 5v_out").click());
    await flushUntil(() => putCalls().length === 2);

    // Then: only the running row offers Cancel and other runs are disabled
    expect(hasCancel()).toBe(true);
    expect(runButton(view, "EDL via 12v_out").disabled).toBe(true);

    // When: cancelling
    act(() => cancelButton(view, "MASKROM via 5v_out").click());
    await flushUntil(() => view.textContent?.includes("cancelled") === true);

    // Then: the cancel control is gone again
    expect(hasCancel()).toBe(false);
  });

  it("stops later built-in records on cancel and runs exactly one CON_MAS cleanup", async () => {
    // Given: a MASKROM run waiting after the rail-off request
    const view = await mountTaskCard();
    act(() => runButton(view, "MASKROM via 5v_out").click());
    await flushUntil(() => putCalls().length === 2);

    // When: cancelling during the wait
    act(() => cancelButton(view, "MASKROM via 5v_out").click());
    await flushUntil(() => view.textContent?.includes("cancelled") === true);

    // Then: the assert/power-on records never ran and one cleanup PUT released CON_MAS
    const puts = putCalls();
    expect(puts.map((call) => `${call.url} ${call.body ?? ""}`)).toEqual([
      '/api/v1/gpio/CON_MAS {"direction":"input"}',
      '/api/v1/power/5v_out {"state":"off"}',
      '/api/v1/gpio/CON_MAS {"direction":"input"}',
    ]);
    // And: the cancellation is reported as partial, never as a rollback
    expect(view.textContent).toContain("cancelled");
    expect(view.textContent?.toLowerCase()).toContain("partial");
    // And: the automation lock is released after cleanup finished
    expect(automationLockOwner()).toBeNull();
  });

  it("never runs cleanup for a cancelled stored task", async () => {
    // Given: a stored task waiting after its first request
    stubTaskList(storedListPayload([{ id: "t1", name: "t1", request_count: 2 }], STORED_WITH_WAIT_BLOB));
    const view = await mountTaskCard();
    act(() => runButton(view, "t1").click());
    await flushUntil(() => putCalls().length === 1);

    // When: cancelling during the wait
    act(() => cancelButton(view, "t1").click());
    await flushUntil(() => view.textContent?.includes("cancelled") === true);

    // Then: only the first record went out; stored tasks never infer cleanup
    expect(putCalls().map((call) => `${call.url} ${call.body ?? ""}`)).toEqual([
      '/api/v1/power/5v_out {"state":"off"}',
    ]);
    expect(automationLockOwner()).toBeNull();
  });

  it("aborts on unmount, finishes cleanup, and releases the lock without further renders", async () => {
    // Given: a built-in run waiting after the rail-off request
    const view = await mountTaskCard();
    act(() => runButton(view, "MASKROM via 5v_out").click());
    await flushUntil(() => putCalls().length === 2);
    expect(automationLockOwner()).toBe("task");

    // When: the component unmounts mid-run
    unmountTaskCard();
    await flushUntil(() => automationLockOwner() === null);

    // Then: the abort stopped later records, cleanup still ran, and the lock was released last
    const puts = putCalls();
    expect(puts.map((call) => `${call.url} ${call.body ?? ""}`)).toEqual([
      '/api/v1/gpio/CON_MAS {"direction":"input"}',
      '/api/v1/power/5v_out {"state":"off"}',
      '/api/v1/gpio/CON_MAS {"direction":"input"}',
    ]);
    await flush();
  });

  it("keeps task ownership until deferred unmount cleanup settles", async () => {
    // Given: a built-in run waiting after rail-off and a cleanup response held open
    const view = await mountTaskCard();
    act(() => runButton(view, "MASKROM via 5v_out").click());
    await flushUntil(() => putCalls().length === 2);
    const releaseCleanup = deferNextPutResponse();

    // When: unmount aborts the run and starts cleanup
    unmountTaskCard();
    await flushUntil(() => putCalls().length === 3);

    // Then: shared ownership remains held until cleanup has actually settled
    expect(automationLockOwner()).toBe("task");
    releaseCleanup();
    await flushUntil(() => automationLockOwner() === null);
  });
});
