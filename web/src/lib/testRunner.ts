import type { RefObject } from "react";
import type { UseBoard } from "../hooks/useBoard";
import type { SerialAutomationHandle, SerialChannelId } from "../components/SerialCard";
import type { PowerCapture } from "./types";
import {
  commandEnvelope,
  commandMarker,
  parseCommandCompletion,
  stripTerminalControl,
} from "./serialTask.ts";
import { evaluateAssertion, type AssertionContext } from "./testAssertions.ts";
import type {
  TestScript,
  StepAssertion,
  StepResult,
  RunSummary,
  AdcSampleEntry,
  SerialLogEntry,
  ExecutionStep,
} from "./testScript";
import { buildExecutionPlan } from "./testScript.ts";
import { POWER_CAPTURE_SAMPLE_CAPACITY } from "./power.ts";

export interface RunnerCallbacks {
  onStepStart(stepId: string): void;
  onStepResult(result: StepResult): void;
  onSerialLog(stepId: string, text: string, direction: "rx" | "tx"): void;
  onAdcSample(stepId: string, channel: string, currentUa: number, timestampMs: number): void;
  onComplete(summary: RunSummary): void;
  onError(error: string): void;
}

export interface RunnerHandle {
  start(): Promise<void>;
  abort(): void;
}

class RunnerAbortedError extends Error {
  constructor() {
    super("aborted");
    this.name = "AbortError";
  }
}

const SERIAL_BUFFER_LIMIT = 65_536;

function appendBoundedText(current: string, text: string): string {
  const combined = current + text;
  return combined.length > SERIAL_BUFFER_LIMIT
    ? combined.slice(-SERIAL_BUFFER_LIMIT)
    : combined;
}

function rawPatternMatchEnd(text: string, regex: RegExp): number | null {
  let clean = "";
  const rawEnds: number[] = [];
  for (let index = 0; index < text.length;) {
    const ansi = /^\x1b\[[0-?]*[ -/]*[@-~]/.exec(text.slice(index));
    if (ansi) {
      index += ansi[0].length;
      continue;
    }
    const code = text.charCodeAt(index);
    if (text[index] === "\uFFFD" || code <= 0x08 || code === 0x0b || code === 0x0c
      || (code >= 0x0e && code <= 0x1f) || code === 0x7f) {
      index += 1;
      continue;
    }
    clean += text[index];
    rawEnds.push(index + 1);
    index += 1;
  }
  const match = regex.exec(clean);
  if (!match || match.index == null) return null;
  const cleanEnd = match.index + match[0].length;
  return cleanEnd === 0 ? 0 : (rawEnds[cleanEnd - 1] ?? text.length);
}

function sleep(ms: number, signal: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    if (signal.aborted) {
      reject(new RunnerAbortedError());
      return;
    }
    const timer = globalThis.setTimeout(() => {
      signal.removeEventListener("abort", onAbort);
      resolve();
    }, ms);
    const onAbort = () => {
      globalThis.clearTimeout(timer);
      reject(new RunnerAbortedError());
    };
    signal.addEventListener("abort", onAbort, { once: true });
  });
}

function waitForPattern(
  serialRef: RefObject<SerialAutomationHandle>,
  channel: SerialChannelId,
  pattern: string,
  timeoutMs: number,
  getInitialText: () => string,
  signal: AbortSignal,
): Promise<{ matched: boolean; text: string; consumedLength: number }> {
  return new Promise((resolve, reject) => {
    const handle = serialRef.current;
    if (!handle?.isConnected(channel)) {
      resolve({ matched: false, text: "", consumedLength: 0 });
      return;
    }

    let buffer = "";
    let resolved = false;
    let timer: ReturnType<typeof globalThis.setTimeout> | undefined;
    let regex: RegExp;
    try {
      if (pattern.length > 500) throw new Error("pattern too long");
      regex = new RegExp(pattern, "im");
    } catch {
      reject(new Error(`invalid serial wait pattern: ${pattern}`));
      return;
    }

    let unsub = () => {};
    const finish = (result: { matched: boolean; text: string; consumedLength: number }) => {
      if (resolved) return;
      resolved = true;
      unsub();
      if (timer != null) globalThis.clearTimeout(timer);
      signal.removeEventListener("abort", onAbort);
      resolve(result);
    };
    const onAbort = () => {
      if (resolved) return;
      resolved = true;
      unsub();
      if (timer != null) globalThis.clearTimeout(timer);
      reject(new RunnerAbortedError());
    };

    unsub = handle.subscribe((text: string) => {
      buffer = appendBoundedText(buffer, text);
      const matchEnd = rawPatternMatchEnd(buffer, regex);
      if (!resolved && matchEnd != null) {
        finish({ matched: true, text: buffer, consumedLength: matchEnd });
      }
    }, channel);

    buffer = getInitialText();
    const initialMatchEnd = rawPatternMatchEnd(buffer, regex);
    if (initialMatchEnd != null) {
      finish({ matched: true, text: buffer, consumedLength: initialMatchEnd });
    }

    if (!resolved) {
      timer = globalThis.setTimeout(() => {
        finish({ matched: false, text: buffer, consumedLength: buffer.length });
      }, timeoutMs);
      signal.addEventListener("abort", onAbort, { once: true });
    }
  });
}

async function sendAndExpect(
  serialRef: RefObject<SerialAutomationHandle>,
  channel: SerialChannelId,
  command: string,
  pattern: string,
  timeoutMs: number,
  onTx: (text: string) => void,
  signal: AbortSignal,
): Promise<{ completed: boolean; matched: boolean; exitCode: number; output: string }> {
  const handle = serialRef.current;
  if (!handle?.isConnected(channel)) {
    return { completed: false, matched: false, exitCode: -1, output: "" };
  }

  let regex: RegExp;
  try {
    if (pattern.length > 500) throw new Error("pattern too long");
    regex = new RegExp(pattern, "im");
  } catch {
    throw new Error(`invalid expected pattern: ${pattern}`);
  }

  const result = await new Promise<{ completed: boolean; matched: boolean; exitCode: number; output: string }>((resolve, reject) => {
    if (signal.aborted) {
      reject(new RunnerAbortedError());
      return;
    }
    const runId = crypto.getRandomValues(new Uint32Array(1))[0];
    const marker = commandMarker(runId);
    const envelope = commandEnvelope(command, marker);
    let buffer = "";
    let resolved = false;
    let timer: ReturnType<typeof globalThis.setTimeout> | undefined;

    let unsub = () => {};
    const cleanup = () => {
      unsub();
      if (timer != null) globalThis.clearTimeout(timer);
      signal.removeEventListener("abort", onAbort);
    };
    const onAbort = () => {
      if (resolved) return;
      resolved = true;
      cleanup();
      reject(new RunnerAbortedError());
    };

    unsub = handle.subscribe((text: string) => {
      buffer = appendBoundedText(buffer, text);
      if (!resolved) {
        const completion = parseCommandCompletion(buffer, marker);
        if (completion) {
          resolved = true;
          cleanup();
          resolve({
            completed: true,
            matched: regex.test(stripTerminalControl(completion.output)),
            exitCode: completion.exitCode,
            output: completion.output,
          });
        }
      }
    }, channel);

    onTx(envelope);
    handle.write(envelope, channel).catch((err) => {
      if (!resolved) {
        resolved = true;
        cleanup();
        resolve({ completed: false, matched: false, exitCode: -1, output: `write failed: ${err instanceof Error ? err.message : String(err)}` });
      }
    });

    if (!resolved) {
      timer = globalThis.setTimeout(() => {
        if (!resolved) {
          resolved = true;
          cleanup();
          resolve({ completed: false, matched: false, exitCode: -1, output: buffer });
        }
      }, timeoutMs);
      signal.addEventListener("abort", onAbort, { once: true });
    }
  });

  return result;
}

async function waitForCapture(
  getBoard: () => UseBoard,
  startedAtMs: number,
  timeoutMs: number,
  signal: AbortSignal,
): Promise<PowerCapture | null> {
  while (Date.now() - startedAtMs <= timeoutMs) {
    const captures = getBoard().captures;
    const latest = captures[captures.length - 1];
    if (latest?.capturedAt >= startedAtMs) return latest;
    await sleep(100, signal);
  }
  return null;
}

export function createTestRunner(
  script: TestScript,
  boardRef: RefObject<UseBoard>,
  serialRef: RefObject<SerialAutomationHandle>,
  callbacks: RunnerCallbacks,
): RunnerHandle {
  let aborted = false;
  let captureActive = false;
  const abortController = new AbortController();
  const { signal } = abortController;
  const results: StepResult[] = [];
  const executionPlan = buildExecutionPlan(script);
  const startedAtMs = Date.now();
  const serialBuffers = new Map<SerialChannelId, { text: string; cursor: number }>();
  let activeStepId = "run";
  const getBoard = () => {
    if (!boardRef.current) throw new Error("device state is unavailable");
    return boardRef.current;
  };
  const getSerialBuffer = (channel: SerialChannelId) => {
    let state = serialBuffers.get(channel);
    if (!state) {
      state = { text: "", cursor: 0 };
      serialBuffers.set(channel, state);
    }
    return state;
  };
  const appendSerialBuffer = (channel: SerialChannelId, text: string) => {
    const state = getSerialBuffer(channel);
    const combined = state.text + text;
    const trim = Math.max(0, combined.length - SERIAL_BUFFER_LIMIT);
    state.text = trim > 0 ? combined.slice(trim) : combined;
    state.cursor = Math.max(0, state.cursor - trim);
  };
  const unreadSerialBuffer = (channel: SerialChannelId) => {
    const state = getSerialBuffer(channel);
    return state.text.slice(state.cursor);
  };
  const consumeSerialBuffer = (channel: SerialChannelId, consumedLength?: number) => {
    const state = getSerialBuffer(channel);
    state.cursor = consumedLength == null
      ? state.text.length
      : Math.min(state.text.length, state.cursor + consumedLength);
  };

  async function executeStep(step: ExecutionStep): Promise<StepResult> {
    const stepStartedAt = Date.now();
    const resultIdentity = {
      stepId: step.executionId,
      sourceStepId: step.sourceStepId,
      loopId: step.loopId,
      loopIteration: step.loopIteration,
      loopCount: step.loopCount,
      unitId: step.unitId,
      unitName: step.unitName,
      conditionId: step.conditionId,
      conditionRole: step.conditionRole,
    };
    let ctx: AssertionContext = {};
    let error: string | undefined;
    let serialOutput: string | undefined;
    let adcValueUa: number | undefined;
    let implicitAssertion: StepAssertion | undefined;

    try {
      switch (step.type) {
        case "power_on": {
          const p = step.params as import("./testScript").PowerOnParams;
          await getBoard().setPower(p.rail, true);
          await sleep(500, signal);
          break;
        }
        case "power_off": {
          const p = step.params as import("./testScript").PowerOffParams;
          await getBoard().setPower(p.rail, false);
          break;
        }
        case "delay": {
          const p = step.params as import("./testScript").DelayParams;
          await sleep(p.ms, signal);
          break;
        }
        case "serial_wait": {
          const p = step.params as import("./testScript").SerialWaitParams;
          const channel = p.channel as SerialChannelId;
          if (!serialRef.current?.isConnected(channel)) {
            return makeSkip(step, stepStartedAt, "no serial connection");
          }
          serialRef.current?.setAutomationActive(true, channel);
          try {
            const result = await waitForPattern(
              serialRef,
              channel,
              p.pattern,
              p.timeout_ms,
              () => unreadSerialBuffer(channel),
              signal,
            );
            serialOutput = result.text;
            ctx.serialOutput = result.text;
            consumeSerialBuffer(channel, result.consumedLength);
            if (!result.matched) {
              error = `timeout waiting for pattern: ${p.pattern}`;
            }
          } finally {
            serialRef.current?.setAutomationActive(false, channel);
          }
          break;
        }
        case "serial_send": {
          const p = step.params as import("./testScript").SerialSendParams;
          const channel = p.channel as SerialChannelId;
          if (!serialRef.current?.isConnected(channel)) {
            return makeSkip(step, stepStartedAt, "no serial connection");
          }
          callbacks.onSerialLog(step.executionId, p.text, "tx");
          await serialRef.current.write(p.text, channel);
          break;
        }
        case "serial_expect": {
          const p = step.params as import("./testScript").SerialExpectParams;
          const channel = p.channel as SerialChannelId;
          if (!serialRef.current?.isConnected(channel)) {
            return makeSkip(step, stepStartedAt, "no serial connection");
          }
          serialRef.current?.setAutomationActive(true, channel);
          try {
            const result = await sendAndExpect(
              serialRef,
              channel,
              p.command,
              p.pattern,
              p.timeout_ms,
              (text) => callbacks.onSerialLog(step.executionId, text, "tx"),
              signal,
            );
            serialOutput = result.output;
            ctx.serialOutput = result.output;
            ctx.exitCode = result.exitCode;
            if (!result.completed) {
              error = "timeout waiting for command completion";
            } else {
              const pattern = p.pattern.trim();
              implicitAssertion = pattern
                ? { regex: pattern, exit_code: 0 }
                : { exit_code: 0 };
            }
          } finally {
            consumeSerialBuffer(channel);
            serialRef.current?.setAutomationActive(false, channel);
          }
          break;
        }
        case "adc_read": {
          const p = step.params as import("./testScript").AdcReadParams;
          const reading = await getBoard().readPower(p.channel);
          adcValueUa = reading.currentUa;
          ctx.adcValueUa = reading.currentUa;
          callbacks.onAdcSample(step.executionId, p.channel, reading.currentUa, Date.now());
          break;
        }
        case "gpio_set": {
          const p = step.params as import("./testScript").GpioSetParams;
          await getBoard().setGpio(p.pin, "output", p.value);
          break;
        }
        case "gpio_assert": {
          const p = step.params as import("./testScript").GpioAssertParams;
          const pin = p.pin;
          await sleep(100, signal);
          const gpio = getBoard().snapshot.gpios.find(
            (g) => g.name === pin || g.note === pin || String(g.pin) === pin,
          );
          if (!gpio) {
            error = `GPIO ${pin} not found`;
            break;
          }
          ctx.pinDirection = gpio.direction;
          ctx.pinValue = gpio.value;
          implicitAssertion = {
            pin_direction: p.direction,
            pin_value: p.value,
          };
          break;
        }
        case "switch_route": {
          const p = step.params as import("./testScript").SwitchRouteParams;
          await getBoard().setSwitch(p.switch, p.route);
          break;
        }
        case "capture": {
          const p = step.params as import("./testScript").CaptureParams;
          const { rail, trigger, duration_ms: durationMs } = p;
          const rateHz = 100;
          if (!Number.isFinite(durationMs) || durationMs <= 0) {
            throw new Error("capture duration must be greater than zero");
          }
          const samples = Math.max(2, Math.ceil((durationMs / 1000) * rateHz));
          if (samples > POWER_CAPTURE_SAMPLE_CAPACITY) {
            throw new Error(
              `capture requires ${samples} samples, but this firmware supports ${POWER_CAPTURE_SAMPLE_CAPACITY}`,
            );
          }
          const preSamples = Math.min(64, Math.floor(samples / 4));
          const captureStartedAt = Date.now();
          await getBoard().armCapture({
            trigger,
            source: rail,
            edge: "rising",
            thresholdUa: trigger === "current" ? Math.max(0, p.threshold_a ?? 0.1) * 1_000_000 : 0,
            rateHz,
            preSamples,
            postSamples: Math.max(1, samples - preSamples - 1),
          });
          captureActive = true;
          if (trigger === "manual") {
            getBoard().triggerCapture();
          } else if (trigger === "power_on") {
            await getBoard().setPower(rail, false);
            await sleep(500, signal);
            await getBoard().setPower(rail, true);
          } else if (trigger === "gpio") {
            throw new Error("GPIO-triggered capture is not supported by this task step");
          }
          const capture = await waitForCapture(getBoard, captureStartedAt, durationMs + 10000, signal);
          if (!capture) {
            error = "capture timed out";
            break;
          }
          captureActive = false;
          const railReadings = capture.samples.flatMap((s) => {
            const r = s.readings.find((rd) => rd.name === rail);
            return r ? [r.current_ua] : [];
          });
          ctx.peakCurrentUa = railReadings.length > 0 ? railReadings.reduce((max, v) => Math.max(max, v), 0) : 0;
          const nominalVoltage = rail.startsWith("12v") ? 12 : rail.startsWith("20v") ? 20 : 5;
          const samplePeriodSeconds = 1 / Math.max(1, capture.rateHz);
          ctx.energyUj = railReadings.reduce(
            (sum, currentUa) => sum + currentUa * nominalVoltage * samplePeriodSeconds,
            0,
          );
          const lastDeviceTimeUs = capture.samples.at(-1)?.deviceTimeUs ?? 0;
          for (const sample of capture.samples) {
            const reading = sample.readings.find((item) => item.name === rail);
            if (!reading) continue;
            const timestampMs = capture.capturedAt - (lastDeviceTimeUs - sample.deviceTimeUs) / 1000;
            callbacks.onAdcSample(step.executionId, rail, reading.current_ua, timestampMs);
          }
          break;
        }
      }
    } catch (e) {
      if (e instanceof Error && e.name === "AbortError") {
        const finishedAtMs = Date.now();
        return {
          ...resultIdentity,
          stepType: step.type,
          status: "aborted",
          startedAtMs: stepStartedAt,
          finishedAtMs,
          durationMs: finishedAtMs - stepStartedAt,
          error: "aborted",
        };
      }
      error = e instanceof Error ? e.message : String(e);
    } finally {
      if (captureActive) {
        boardRef.current?.cancelCapture?.();
        captureActive = false;
      }
    }

    const effectiveAssertion = implicitAssertion
      ? { ...step.assert, ...implicitAssertion }
      : step.assert;
    const assertResult = error ? undefined : evaluateAssertion(effectiveAssertion, ctx);
    const hasError = !!error;
    const assertFailed = assertResult ? !assertResult.passed : false;

    let status: StepResult["status"];
    if (hasError) status = "error";
    else if (assertFailed) status = "fail";
    else status = "pass";

    const finishedAtMs = Date.now();
    return {
      ...resultIdentity,
      stepType: step.type,
      status,
      startedAtMs: stepStartedAt,
      finishedAtMs,
      durationMs: finishedAtMs - stepStartedAt,
      error,
      assertionResult: assertResult,
      adcValueUa,
      serialOutput,
    };
  }

  function makeSkip(
    step: ExecutionStep,
    startMs: number,
    reason: string,
    conditionalSkip = false,
  ): StepResult {
    return {
      stepId: step.executionId,
      sourceStepId: step.sourceStepId,
      loopId: step.loopId,
      loopIteration: step.loopIteration,
      loopCount: step.loopCount,
      unitId: step.unitId,
      unitName: step.unitName,
      conditionId: step.conditionId,
      conditionRole: step.conditionRole,
      conditionalSkip: conditionalSkip || undefined,
      stepType: step.type,
      status: "skip",
      startedAtMs: startMs,
      finishedAtMs: Date.now(),
      durationMs: 0,
      error: reason,
    };
  }

  return {
    async start() {
      const serialUnsubscribers: Array<() => void> = [];
      const serial = serialRef.current;
      if (serial) {
        for (const channel of ["uart0", "uart1"] as const) {
          serialUnsubscribers.push(serial.subscribe((text) => {
            appendSerialBuffer(channel, text);
            callbacks.onSerialLog(activeStepId, text, "rx");
          }, channel));
        }
      }
      const conditionOutcomes = new Map<string, boolean>();
      try {
        for (const step of executionPlan) {
          if (aborted) break;
          activeStepId = step.executionId;
          callbacks.onStepStart(step.executionId);
          if (step.conditionId && (step.conditionRole === "then" || step.conditionRole === "else")) {
            const outcome = conditionOutcomes.get(step.conditionId);
            const branchSelected = outcome != null && (
              (step.conditionRole === "then" && outcome)
              || (step.conditionRole === "else" && !outcome)
            );
            if (!branchSelected) {
              const result = makeSkip(step, Date.now(), "condition branch not selected", true);
              results.push(result);
              callbacks.onStepResult(result);
              continue;
            }
          }

          let result = await executeStep(step);
          if (step.conditionId && step.conditionRole === "check") {
            if (result.status === "pass") {
              conditionOutcomes.set(step.conditionId, true);
              result = { ...result, conditionOutcome: true };
            } else if (result.status === "fail") {
              conditionOutcomes.set(step.conditionId, false);
              result = {
                ...result,
                status: "pass",
                conditionOutcome: false,
              };
            }
          }
          results.push(result);
          callbacks.onStepResult(result);
          if (result.status === "aborted") break;
          if (
            step.conditionRole === "check"
            && result.conditionOutcome == null
          ) {
            break;
          }
          if (
            (result.status === "fail" || result.status === "error") &&
            !step.continue_on_error &&
            !step.assert?.continue_on_error
          ) {
            break;
          }
        }
      } catch (e) {
        callbacks.onError(e instanceof Error ? e.message : String(e));
      } finally {
        for (const unsubscribe of serialUnsubscribers) unsubscribe();
      }

      const finishedAtMs = Date.now();
      const summary: RunSummary = {
        totalSteps: executionPlan.length,
        passed: results.filter((r) => r.status === "pass").length,
        failed: results.filter((r) => r.status === "fail").length,
        skipped: results.filter((r) => r.status === "skip").length,
        errored: results.filter((r) => r.status === "error").length,
        aborted,
        completed: !aborted && results.length === executionPlan.length,
        durationMs: finishedAtMs - startedAtMs,
        startedAtMs,
        finishedAtMs,
        results,
      };
      callbacks.onComplete(summary);
    },

    abort() {
      if (aborted) return;
      aborted = true;
      abortController.abort();
      if (captureActive) {
        boardRef.current?.cancelCapture?.();
        captureActive = false;
      }
    },
  };
}
