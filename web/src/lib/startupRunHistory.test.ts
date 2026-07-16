import assert from "node:assert/strict";
import test from "node:test";
import { appendStartupRunHistory } from "./startupRunHistory.ts";

interface TestRun {
  id: string;
  rail: string;
}

function run(id: string, rail: string): TestRun {
  return { id, rail };
}

test("retains the latest two completed runs for the same rail", () => {
  const history = appendStartupRunHistory([
    run("a-1", "5v_out"),
    run("a-2", "5v_out"),
  ], run("a-3", "5v_out"));

  assert.deepEqual(history, [
    run("a-2", "5v_out"),
    run("a-3", "5v_out"),
  ]);
});

test("keeps interleaved history so A/B/A retains both A runs", () => {
  const history = appendStartupRunHistory([
    run("a-1", "5v_out"),
    run("b-1", "12v_out"),
  ], run("a-2", "5v_out"));

  assert.deepEqual(history, [
    run("a-1", "5v_out"),
    run("b-1", "12v_out"),
    run("a-2", "5v_out"),
  ]);
});

test("bounds each rail independently while preserving chronology", () => {
  const history = appendStartupRunHistory([
    run("a-1", "5v_out"),
    run("b-1", "12v_out"),
    run("a-2", "5v_out"),
    run("c-1", "20v_out"),
    run("b-2", "12v_out"),
  ], run("a-3", "5v_out"));

  assert.deepEqual(history, [
    run("b-1", "12v_out"),
    run("a-2", "5v_out"),
    run("c-1", "20v_out"),
    run("b-2", "12v_out"),
    run("a-3", "5v_out"),
  ]);
});
