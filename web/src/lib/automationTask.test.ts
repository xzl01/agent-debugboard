import assert from "node:assert/strict";
import test from "node:test";
import { createAutomationTaskLock } from "./automationTask.ts";

test("allows only one automation owner at a time", () => {
  const lock = createAutomationTaskLock();

  assert.equal(lock.acquire("startup"), true);
  assert.equal(lock.owner(), "startup");
  assert.equal(lock.acquire("startup"), false);
  assert.equal(lock.acquire("test"), false);
  assert.equal(lock.owner(), "startup");

  lock.release("test");
  assert.equal(lock.owner(), "startup");
  lock.release("startup");
  assert.equal(lock.owner(), null);

  assert.equal(lock.acquire("test"), true);
  assert.equal(lock.owner(), "test");
});
