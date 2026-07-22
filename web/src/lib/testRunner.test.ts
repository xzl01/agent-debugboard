import { describe, it } from "node:test";
import assert from "node:assert/strict";
import { createTestRunner, type RunnerCallbacks } from "./testRunner.ts";
import type { RunSummary, TestScript } from "./testScript.ts";

function callbacks(onComplete: (summary: RunSummary) => void): RunnerCallbacks {
  return {
    onStepStart() {},
    onStepResult() {},
    onSerialLog() {},
    onAdcSample() {},
    onComplete,
    onError(error) {
      throw new Error(error);
    },
  };
}

function baseBoard() {
  return {
    snapshot: { gpios: [] },
    captures: [],
    async setPower() {},
    async readPower() { return { state: "on", currentUa: 100_000 }; },
    async setSwitch() {},
    async setGpio() {},
    async armCapture() {},
    triggerCapture() {},
    cancelCapture() {},
  };
}

function serialWithCommandOutput(output: string) {
  const listeners = new Set<(text: string, receivedAtMs: number) => void>();
  return {
    isConnected: () => true,
    connectedChannels: () => ["uart0"],
    clear() {},
    setAutomationActive() {},
    subscribe(listener: (text: string, receivedAtMs: number) => void) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    async write(data: string) {
      const marker = /__LINKR_TASK_\d+__/.exec(data)?.[0];
      assert.ok(marker);
      queueMicrotask(() => {
        for (const listener of listeners) listener(`${output}\n${marker}:0\n`, Date.now());
      });
    },
  };
}

async function run(script: TestScript, board: object, serial: object) {
  let summary: RunSummary | null = null;
  const runner = createTestRunner(
    script,
    { current: board } as never,
    { current: serial } as never,
    callbacks((value) => { summary = value; }),
  );
  await runner.start();
  assert.ok(summary);
  return summary as RunSummary;
}

describe("createTestRunner", () => {
  it("requires serial_expect output to match the configured pattern", async () => {
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "expect",
      steps: [{
        id: "expect",
        type: "serial_expect",
        params: { channel: "uart0", command: "uname -a", pattern: "Linux", timeout_ms: 100 },
      }],
    };
    const summary = await run(script, baseBoard(), serialWithCommandOutput("Darwin"));
    assert.equal(summary.results[0].status, "error");
    assert.match(summary.results[0].error ?? "", /did not match/);
  });

  it("passes serial_expect only after command completion and output match", async () => {
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "expect",
      steps: [{
        id: "expect",
        type: "serial_expect",
        params: { channel: "uart0", command: "uname -a", pattern: "Linux", timeout_ms: 100 },
      }],
    };
    const summary = await run(script, baseBoard(), serialWithCommandOutput("Linux 6.1"));
    assert.equal(summary.results[0].status, "pass");
    assert.equal(summary.completed, true);
  });

  it("aborts a delay immediately and marks the run incomplete", async () => {
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "abort",
      steps: [{ id: "delay", type: "delay", params: { ms: 10_000 } }],
    };
    let summary: RunSummary | null = null;
    const runner = createTestRunner(
      script,
      { current: baseBoard() } as never,
      { current: serialWithCommandOutput("") } as never,
      callbacks((value) => { summary = value; }),
    );
    const startedAt = Date.now();
    const pending = runner.start();
    setTimeout(() => runner.abort(), 10);
    await pending;
    assert.ok(summary);
    assert.equal((summary as RunSummary).aborted, true);
    assert.equal((summary as RunSummary).completed, false);
    assert.equal((summary as RunSummary).results[0].status, "aborted");
    assert.ok(Date.now() - startedAt < 500);
  });

  it("observes a capture published through a fresh board reference", async () => {
    const boardRef: { current: ReturnType<typeof baseBoard> & { captures: any[] } } = {
      current: { ...baseBoard(), captures: [] },
    };
    boardRef.current.triggerCapture = () => {
      setTimeout(() => {
        boardRef.current = {
          ...boardRef.current,
          captures: [{
            id: 1,
            trigger: "manual",
            source: "5v_out",
            edge: "rising",
            thresholdUa: 0,
            rateHz: 100,
            preSamples: 0,
            postSamples: 1,
            triggerOffset: 0,
            capturedAt: Date.now(),
            samples: [{
              offset: 0,
              triggered: true,
              sampleSequence: 1,
              deviceTimeUs: 1_000,
              readings: [{ name: "5v_out", current_ua: 200_000 }],
            }],
          }],
        };
      }, 10);
    };
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "capture",
      steps: [{
        id: "capture",
        type: "capture",
        params: { rail: "5v_out", trigger: "manual", duration_ms: 100 },
        assert: { peak_current_max_a: 1, energy_max_j: 1 },
      }],
    };
    let summary: RunSummary | null = null;
    const runner = createTestRunner(
      script,
      boardRef as never,
      { current: serialWithCommandOutput("") } as never,
      callbacks((value) => { summary = value; }),
    );
    await runner.start();
    assert.ok(summary);
    assert.equal((summary as RunSummary).results[0].status, "pass");
  });
});
