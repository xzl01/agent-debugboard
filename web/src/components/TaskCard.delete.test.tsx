import { act } from "react";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import {
  confirmCalls,
  deleteButton,
  fetchCalls,
  flush,
  mountTaskCard,
  rowFor,
  setupTaskCardHarness,
  stubConfirmResult,
  stubTaskList,
  storedListPayload,
  teardownTaskCardHarness,
  TWO_RECORD_BLOB,
} from "./TaskCard.testUtils";

beforeEach(setupTaskCardHarness);
afterEach(teardownTaskCardHarness);

describe("TaskCard single-task delete", () => {
  it("deletes one stored task through a revision-bound atomic mutation after confirmation", async () => {
    // Given: a stored task
    stubTaskList(storedListPayload([{ id: "t1", name: "t1", request_count: 2 }], TWO_RECORD_BLOB));
    const view = await mountTaskCard();

    // When: deleting the task
    act(() => deleteButton(view, "t1").click());
    await flush();

    // Then: the confirmation named the id and the targeted DELETE went out
    expect(confirmCalls()).toHaveLength(1);
    expect(confirmCalls()[0]).toContain("t1");
    const deletes = fetchCalls().filter((call) => call.method === "POST");
    expect(deletes.map((call) => call.url)).toEqual(["/api/v1/tasks/mutations"]);
    const deleted = JSON.parse(deletes[0].body!) as Record<string, string>;
    expect(deleted.action).toBe("delete");
    expect(deleted.id).toBe("t1");
  });

  it("sends no DELETE when the user rejects the confirmation", async () => {
    // Given: a stored task and a rejecting user
    stubTaskList(storedListPayload([{ id: "t1", name: "t1", request_count: 2 }], TWO_RECORD_BLOB));
    const view = await mountTaskCard();
    stubConfirmResult(false);

    // When: the delete is attempted
    act(() => deleteButton(view, "t1").click());
    await flush();

    // Then: the confirmation was shown but nothing was sent
    expect(confirmCalls()).toHaveLength(1);
    expect(fetchCalls().filter((call) => call.method === "DELETE")).toEqual([]);
    expect(fetchCalls().filter((call) => call.method === "POST")).toEqual([]);
  });

  it("offers no delete control for immutable built-in tasks", async () => {
    // Given: the built-in firmware catalog (no stored overrides)
    const view = await mountTaskCard();

    // Then: a built-in row has a Run control but no Delete control
    const row = rowFor(view, "Rockchip MASKROM via 5v_out");
    const labels = [...row.querySelectorAll("button")].map((b) => b.textContent?.trim());
    expect(labels).toContain("Run task");
    expect(labels).not.toContain("Delete");
  });
});
