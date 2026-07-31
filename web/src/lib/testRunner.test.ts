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
    onPowerCapture() {},
    onComplete,
    onError(error) {
      throw new Error(error);
    },
  };
}

function baseBoard() {
  return {
    snapshot: { mcu: "RP2350", gpios: [] },
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
    assert.equal(summary.results[0].status, "fail");
    assert.match(summary.results[0].assertionResult?.detail ?? "", /does not match/);
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

  it("compares gpio_assert parameters with the observed GPIO state", async () => {
    const board = baseBoard() as any;
    board.snapshot.gpios = [{
      name: "GP13",
      pin: 13,
      note: "",
      direction: "input",
      value: 0,
    }];
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "gpio assertion",
      steps: [{
        id: "gpio",
        type: "gpio_assert",
        params: { pin: "GP13", direction: "output", value: 1 },
      }],
    };

    const summary = await run(script, board, serialWithCommandOutput(""));
    assert.equal(summary.results[0].status, "fail");
    assert.match(summary.results[0].assertionResult?.detail ?? "", /direction|value/);
  });

  it("records serial output from power_on before a following serial_wait starts", async () => {
    const listeners = new Set<(text: string, receivedAtMs: number) => void>();
    const serial = {
      isConnected: () => true,
      connectedChannels: () => ["uart0"],
      clear() {},
      setAutomationActive() {},
      subscribe(listener: (text: string, receivedAtMs: number) => void, channel = "uart0") {
        if (channel === "uart0") listeners.add(listener);
        return () => listeners.delete(listener);
      },
      async write() {},
    };
    const board = baseBoard() as any;
    board.setPower = async (_rail: string, on: boolean) => {
      if (on) {
        for (const listener of listeners) listener("BOOT0 ready\n", Date.now());
      }
    };
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "early boot",
      steps: [
        { id: "power", type: "power_on", params: { rail: "5v_out" } },
        { id: "boot", type: "serial_wait", params: { channel: "uart0", pattern: "BOOT0", timeout_ms: 10 } },
      ],
    };

    const summary = await run(script, board, serial);
    assert.equal(summary.results[1].status, "pass");
  });

  it("records the UART channel and receive timestamp for serial evidence", async () => {
    const listeners = new Map<string, Set<(text: string, receivedAtMs: number) => void>>();
    const serial = {
      isConnected: (channel: string) => channel === "uart0",
      connectedChannels: () => ["uart0"],
      clear() {},
      setAutomationActive() {},
      subscribe(listener: (text: string, receivedAtMs: number) => void, channel = "uart0") {
        const channelListeners = listeners.get(channel) ?? new Set();
        channelListeners.add(listener);
        listeners.set(channel, channelListeners);
        return () => channelListeners.delete(listener);
      },
      async write() {},
    };
    const board = baseBoard() as any;
    board.setPower = async (_rail: string, on: boolean) => {
      if (on) {
        for (const listener of listeners.get("uart0") ?? []) listener("BOOT0 ready\n", 1234);
      }
    };
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "serial evidence",
      steps: [
        { id: "power", type: "power_on", params: { rail: "5v_out" } },
        { id: "boot", type: "serial_wait", params: { channel: "uart0", pattern: "BOOT0", timeout_ms: 10 } },
      ],
    };
    const logs: Array<{ channel: string; timestampMs: number }> = [];
    let summary: RunSummary | null = null;
    const runnerCallbacks = callbacks((value) => { summary = value; });
    runnerCallbacks.onSerialLog = (_stepId, channel, _text, _direction, timestampMs) => {
      logs.push({ channel, timestampMs });
    };
    const runner = createTestRunner(
      script,
      { current: board } as never,
      { current: serial } as never,
      runnerCallbacks,
    );
    await runner.start();

    assert.ok(summary);
    assert.deepEqual(logs, [{ channel: "uart0", timestampMs: 1234 }]);
  });

  it("preserves later milestones that arrived in the same early serial chunk", async () => {
    const listeners = new Set<(text: string, receivedAtMs: number) => void>();
    const serial = {
      isConnected: () => true,
      connectedChannels: () => ["uart0"],
      clear() {},
      setAutomationActive() {},
      subscribe(listener: (text: string, receivedAtMs: number) => void, channel = "uart0") {
        if (channel === "uart0") listeners.add(listener);
        return () => listeners.delete(listener);
      },
      async write() {},
    };
    const board = baseBoard() as any;
    board.setPower = async (_rail: string, on: boolean) => {
      if (on) {
        for (const listener of listeners) {
          listener("\x1b[32mBOOT0 ready\x1b[0m\nUEFI ready\nKernel ready\n", Date.now());
        }
      }
    };
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "early milestones",
      steps: [
        { id: "power", type: "power_on", params: { rail: "5v_out" } },
        { id: "boot", type: "serial_wait", params: { channel: "uart0", pattern: "BOOT0", timeout_ms: 10 } },
        { id: "uefi", type: "serial_wait", params: { channel: "uart0", pattern: "UEFI", timeout_ms: 10 } },
        { id: "kernel", type: "serial_wait", params: { channel: "uart0", pattern: "Kernel", timeout_ms: 10 } },
      ],
    };

    const summary = await run(script, board, serial);
    assert.deepEqual(summary.results.map((result) => result.status), ["pass", "pass", "pass", "pass"]);
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

  it("cancels a capture that times out", async () => {
    const board = baseBoard() as any;
    let cancelCalls = 0;
    board.cancelCapture = () => { cancelCalls += 1; };
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "capture timeout",
      steps: [{
        id: "capture",
        type: "capture",
        params: { rail: "5v_out", trigger: "current", duration_ms: 5000, threshold_a: 1 },
      }],
    };
    const realNow = Date.now;
    let fakeNow = 0;
    Date.now = () => (fakeNow += 20_000);
    try {
      const summary = await run(script, board, serialWithCommandOutput(""));
      assert.equal(summary.results[0].status, "error");
      assert.match(summary.results[0].error ?? "", /timed out/);
      assert.equal(cancelCalls, 1);
    } finally {
      Date.now = realNow;
    }
  });

  it("streams long captures on the host instead of applying a firmware sample cap", async () => {
    const board = baseBoard() as any;
    let armedConfig: Record<string, unknown> | null = null;
    board.armCapture = async (config: Record<string, unknown>) => {
      armedConfig = config;
      setTimeout(() => {
        board.captures = [{
          id: 2,
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
        }];
      }, 10);
    };
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "capture capacity",
      steps: [{
        id: "capture",
        type: "capture",
        params: { rail: "5v_out", trigger: "manual", duration_ms: 30_000 },
      }],
    };

    const summary = await run(script, board, serialWithCommandOutput(""));
    assert.equal(summary.results[0].status, "pass");
    assert.equal(summary.completed, true);
    assert.equal(armedConfig?.streaming, true);
    assert.equal(armedConfig?.stopAfterMs, 30_000);
  });

  it("evaluates capture assertions from the complete host summary instead of the preview", async () => {
    const board = baseBoard() as any;
    board.armCapture = async () => {
      setTimeout(() => {
        board.captures = [{
          id: 3,
          trigger: "manual",
          source: "5v_out",
          edge: "rising",
          thresholdUa: 0,
          rateHz: 100,
          preSamples: 0,
          postSamples: 10_000,
          triggerOffset: 0,
          capturedAt: Date.now(),
          samples: [],
          summaries: {
            "5v_out": {
              nominalVoltageV: 5,
              durationMs: 10_000,
              averageCurrentA: 0.4,
              peakCurrentA: 0.8,
              averagePowerW: 2,
              peakPowerW: 4,
              milliampHours: 1.111,
              wattHours: 0.005556,
            },
          },
        }];
      }, 10);
    };
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "capture summary",
      steps: [{
        id: "capture",
        type: "capture",
        params: { rail: "5v_out", trigger: "manual", duration_ms: 10_000 },
        assert: { peak_current_max_a: 0.5 },
      }],
    };

    const summary = await run(script, board, serialWithCommandOutput(""));
    assert.equal(summary.results[0].status, "fail");
    assert.match(
      summary.results[0].assertionResult?.detail ?? "",
      /peak 0\.800A exceeds max 0\.5A/,
    );
  });

  it("fails incomplete captures as infrastructure errors and preserves their evidence", async () => {
    const board = baseBoard() as any;
    const incompleteCapture = {
      id: 4,
      trigger: "manual",
      source: "5v_out",
      edge: "rising",
      thresholdUa: 0,
      rateHz: 100,
      preSamples: 0,
      postSamples: 1,
      triggerOffset: 0,
      capturedAt: 0,
      samples: [],
      sampleCount: 500,
      droppedSamples: 7,
      incomplete: true,
      interruptionReason: "host storage fell behind",
      summaries: {
        "5v_out": {
          nominalVoltageV: 5,
          durationMs: 5000,
          averageCurrentA: 0.2,
          peakCurrentA: 0.4,
          averagePowerW: 1,
          peakPowerW: 2,
          milliampHours: 0.278,
          wattHours: 0.001389,
        },
      },
    };
    board.armCapture = async () => {
      setTimeout(() => {
        board.captures = [{ ...incompleteCapture, capturedAt: Date.now() }];
      }, 10);
    };
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "incomplete capture",
      steps: [{
        id: "capture",
        type: "capture",
        params: { rail: "5v_out", trigger: "manual", duration_ms: 100 },
      }],
    };
    const evidence: unknown[] = [];
    let summary: RunSummary | null = null;
    const runnerCallbacks = callbacks((value) => { summary = value; });
    runnerCallbacks.onPowerCapture = (value) => evidence.push(value);
    const runner = createTestRunner(
      script,
      { current: board } as never,
      { current: serialWithCommandOutput("") } as never,
      runnerCallbacks,
    );
    await runner.start();

    assert.ok(summary);
    assert.equal((summary as RunSummary).results[0].status, "error");
    assert.equal((summary as RunSummary).completed, false);
    assert.match((summary as RunSummary).infrastructureError ?? "", /host storage fell behind/);
    assert.equal(evidence.length, 1);
  });

  it("executes multi-step scripts and accumulates results", async () => {
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "multi-step",
      steps: [
        { id: "s1", type: "power_on", params: { rail: "5v_out" } },
        { id: "s2", type: "delay", params: { ms: 10 } },
        { id: "s3", type: "power_off", params: { rail: "5v_out" } },
      ],
    };
    const summary = await run(script, baseBoard(), serialWithCommandOutput(""));
    assert.equal(summary.totalSteps, 3);
    assert.equal(summary.results.length, 3);
    assert.equal(summary.results[0].status, "pass");
    assert.equal(summary.results[1].status, "pass");
    assert.equal(summary.results[2].status, "pass");
  });

  it("stops on error unless continue_on_error is set", async () => {
    const board = baseBoard() as any;
    board.setPower = async () => { throw new Error("power failed"); };
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "coe",
      steps: [
        { id: "s1", type: "power_on", params: { rail: "5v_out" }, continue_on_error: true },
        { id: "s2", type: "delay", params: { ms: 10 } },
      ],
    };
    const summary = await run(script, board, serialWithCommandOutput(""));
    assert.equal(summary.results[0].status, "error");
    assert.equal(summary.results[1].status, "pass");
    assert.equal(summary.totalSteps, 2);
  });

  it("runs safe cleanup after a step failure", async () => {
    const powerCalls: boolean[] = [];
    const board = baseBoard() as any;
    board.setPower = async (_rail: string, on: boolean) => { powerCalls.push(on); };
    board.readPower = async () => { throw new Error("ADC unavailable"); };
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "cleanup after failure",
      steps: [
        { id: "power", type: "power_on", params: { rail: "5v_out" } },
        { id: "adc", type: "adc_read", params: { channel: "5v_out" } },
      ],
    };

    const summary = await run(script, board, serialWithCommandOutput(""));
    assert.deepEqual(powerCalls, [true, false]);
    assert.equal(summary.results[1].status, "error");
    assert.equal(summary.cleanup?.passed, true);
  });

  it("reports cleanup failures as infrastructure errors", async () => {
    const board = baseBoard() as any;
    board.setPower = async (_rail: string, on: boolean) => {
      if (!on) throw new Error("power off failed");
    };
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "cleanup failure",
      steps: [{ id: "power", type: "power_on", params: { rail: "5v_out" } }],
    };

    const summary = await run(script, board, serialWithCommandOutput(""));
    assert.equal(summary.cleanup?.passed, false);
    assert.match(summary.infrastructureError ?? "", /cleanup failed.*power off failed/);
    assert.equal(summary.completed, false);
  });

  it("executes power_on and power_off steps", async () => {
    const calls: string[] = [];
    const board = baseBoard() as any;
    board.setPower = async (name: string, on: boolean) => { calls.push(`${name}:${on}`); };
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "power",
      steps: [
        { id: "s1", type: "power_on", params: { rail: "12v_out" } },
        { id: "s2", type: "power_off", params: { rail: "12v_out" } },
      ],
    };
    const summary = await run(script, board, serialWithCommandOutput(""));
    assert.deepEqual(calls, ["12v_out:true", "12v_out:false", "12v_out:false"]);
    assert.equal(summary.results[0].status, "pass");
    assert.equal(summary.results[1].status, "pass");
    assert.equal(summary.cleanup?.passed, true);
  });

  it("executes adc_read step and reports current", async () => {
    const board = baseBoard() as any;
    board.readPower = async () => ({ state: "on", currentUa: 500_000 });
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "adc",
      steps: [{ id: "s1", type: "adc_read", params: { channel: "5v_out" } }],
    };
    const summary = await run(script, board, serialWithCommandOutput(""));
    assert.equal(summary.results[0].status, "pass");
    assert.equal(summary.results[0].adcValueUa, 500_000);
  });

  it("executes gpio_set step", async () => {
    const calls: Array<{ pin: string; dir: string; val?: number }> = [];
    const board = baseBoard() as any;
    board.setGpio = async (pin: string, dir: string, val?: number) => { calls.push({ pin, dir, val }); };
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "gpio",
      steps: [{ id: "s1", type: "gpio_set", params: { pin: "GP13", value: 1 } }],
    };
    const summary = await run(script, board, serialWithCommandOutput(""));
    assert.deepEqual(calls, [
      { pin: "GP13", dir: "output", val: 1 },
      { pin: "GP13", dir: "input", val: undefined },
    ]);
    assert.equal(summary.results[0].status, "pass");
    assert.equal(summary.cleanup?.passed, true);
  });

  it("executes loop steps for every round with unique result identities", async () => {
    const calls: Array<number | undefined> = [];
    const board = baseBoard() as any;
    board.setGpio = async (_pin: string, _direction: string, value?: number) => {
      calls.push(value);
    };
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "loop",
      steps: [{
        id: "loop1",
        type: "loop",
        params: {
          count: 3,
          steps: [{ id: "gpio", type: "gpio_set", params: { pin: "GP13", value: 1 } }],
        },
      }],
    };

    const summary = await run(script, board, serialWithCommandOutput(""));
    assert.deepEqual(calls, [1, 1, 1, undefined]);
    assert.equal(summary.totalSteps, 3);
    assert.deepEqual(
      summary.results.map((result) => result.stepId),
      ["gpio@loop1:1", "gpio@loop1:2", "gpio@loop1:3"],
    );
    assert.deepEqual(summary.results.map((result) => result.loopIteration), [1, 2, 3]);
    assert.deepEqual(summary.results.map((result) => result.loopCount), [3, 3, 3]);
  });

  it("preserves Unit identity in step results", async () => {
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "unit",
      steps: [{
        id: "unit1",
        type: "loop",
        params: {
          count: 1,
          unit: { name: "Boot and login" },
          steps: [{ id: "wait", type: "delay", params: { ms: 1 } }],
        },
      }],
    };

    const summary = await run(script, baseBoard() as any, serialWithCommandOutput(""));
    assert.equal(summary.results[0].unitId, "unit1");
    assert.equal(summary.results[0].unitName, "Boot and login");
  });

  it("runs only the true branch when a condition assertion passes", async () => {
    const values: Array<number | undefined> = [];
    const board = baseBoard() as any;
    board.setGpio = async (_pin: string, _direction: string, value?: number) => values.push(value);
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "condition true",
      steps: [{
        id: "condition1",
        type: "condition",
        params: {
          check: {
            id: "check",
            type: "serial_expect",
            params: { channel: "uart0", command: "uname -s", pattern: "Linux", timeout_ms: 100 },
          },
          then_steps: [{ id: "then", type: "gpio_set", params: { pin: "GP13", value: 1 } }],
          else_steps: [{ id: "else", type: "gpio_set", params: { pin: "GP13", value: 0 } }],
        },
      }],
    };

    const summary = await run(script, board, serialWithCommandOutput("Linux"));
    assert.deepEqual(values, [1, undefined]);
    assert.equal(summary.results[0].conditionOutcome, true);
    assert.equal(summary.results[2].conditionalSkip, true);
    assert.equal(summary.completed, true);
  });

  it("runs only the false branch without treating a false condition as test failure", async () => {
    const values: Array<number | undefined> = [];
    const board = baseBoard() as any;
    board.setGpio = async (_pin: string, _direction: string, value?: number) => values.push(value);
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "condition false",
      steps: [{
        id: "condition1",
        type: "condition",
        params: {
          check: {
            id: "check",
            type: "serial_expect",
            params: { channel: "uart0", command: "uname -s", pattern: "Linux", timeout_ms: 100 },
          },
          then_steps: [{ id: "then", type: "gpio_set", params: { pin: "GP13", value: 1 } }],
          else_steps: [{ id: "else", type: "gpio_set", params: { pin: "GP13", value: 0 } }],
        },
      }],
    };

    const summary = await run(script, board, serialWithCommandOutput("Darwin"));
    assert.deepEqual(values, [0, undefined]);
    assert.equal(summary.results[0].status, "pass");
    assert.equal(summary.results[0].conditionOutcome, false);
    assert.equal(summary.results[1].conditionalSkip, true);
    assert.equal(summary.completed, true);
  });

  it("executes switch_route step", async () => {
    const calls: Array<{ name: string; route: string }> = [];
    const board = baseBoard() as any;
    board.setSwitch = async (name: string, route: string) => { calls.push({ name, route }); };
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "switch",
      steps: [{ id: "s1", type: "switch_route", params: { switch: "sd", route: "usb-reader" } }],
    };
    const summary = await run(script, board, serialWithCommandOutput(""));
    assert.deepEqual(calls, [{ name: "sd", route: "usb-reader" }]);
    assert.equal(summary.results[0].status, "pass");
  });

  it("fails preflight before hardware actions when required serial is not connected", async () => {
    const serial = {
      isConnected: () => false,
      connectedChannels: () => [] as string[],
      subscribe: () => () => {},
      async write() {},
    };
    const board = baseBoard() as any;
    let powerCalls = 0;
    board.setPower = async () => { powerCalls += 1; };
    const script: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "skip",
      steps: [
        { id: "power", type: "power_on", params: { rail: "5v_out" } },
        { id: "serial", type: "serial_send", params: { channel: "uart0", text: "hello" } },
      ],
    };
    const summary = await run(script, board, serial);
    assert.equal(summary.results.length, 0);
    assert.match(summary.infrastructureError ?? "", /UART0.*not connected/);
    assert.equal(summary.completed, false);
    assert.equal(powerCalls, 0);
  });
});
