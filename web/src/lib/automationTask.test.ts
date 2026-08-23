import assert from "node:assert/strict";
import test from "node:test";
import { createAutomationTaskLock, type AutomationTaskOwner } from "./automationTask.ts";

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

test("accepts task as an automation owner", () => {
  const lock = createAutomationTaskLock();

  assert.equal(lock.acquire("task"), true);
  assert.equal(lock.owner(), "task");

  lock.release("task");
  assert.equal(lock.owner(), null);
});

const mutuallyExclusiveOwners = ["startup", "test", "task"] as const;

// Compile-time guard: if the removed "recovery" variant returns to the
// owner union, this assignment fails type-check.
type AssertOwnerExcludesRecovery = "recovery" extends AutomationTaskOwner
  ? never
  : true;
const ownerExcludesRecovery: AssertOwnerExcludesRecovery = true;

test("automation owner union excludes the removed recovery variant", () => {
  assert.equal(ownerExcludesRecovery, true);
});

for (const holder of mutuallyExclusiveOwners) {
  for (const contender of mutuallyExclusiveOwners) {
    test(`${contender} cannot acquire while ${holder} holds the automation lock`, () => {
      const lock = createAutomationTaskLock();
      assert.equal(lock.acquire(holder), true);
      assert.equal(lock.acquire(contender), false);
      assert.equal(lock.owner(), holder);
    });
  }
}

test("only the holding owner releases the lock", () => {
  const lock = createAutomationTaskLock();
  assert.equal(lock.acquire("task"), true);

  lock.release("startup");
  assert.equal(lock.owner(), "task");
  lock.release("test");
  assert.equal(lock.owner(), "task");

  lock.release("task");
  assert.equal(lock.owner(), null);

  assert.equal(lock.acquire("startup"), true);
  assert.equal(lock.owner(), "startup");
});
