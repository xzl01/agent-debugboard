// @vitest-environment jsdom

import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import { buildExecutionPlan, type RunSummary, type StepResult, type TestScript } from "@/lib/testScript";
import type { PowerCapture } from "@/lib/types";
import { TestReport } from "./TestReport";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const downloadBlob = vi.hoisted(() => vi.fn());

vi.mock("@/lib/utils", async (importOriginal) => ({
  ...(await importOriginal<typeof import("@/lib/utils")>()),
  downloadBlob,
}));

vi.mock("@/lib/i18n", () => ({
  useI18n: () => ({
    t: (key: string, values?: Record<string, unknown>) => {
      if (key === "test.report.partial") return "PARTIAL";
      if (key === "test.report.resultCount") return `${values?.actual}/${values?.expected} results`;
      if (key === "test.report.completedCount") return `${values?.n}/${values?.total} completed`;
      return values ? `${key}:${JSON.stringify(values)}` : key;
    },
  }),
}));

const script: TestScript = {
  schema: "linkr-test.v1",
  version: "1.0",
  name: "report hierarchy",
  steps: [
    { id: "power", type: "power_on", params: { rail: "5v_out" } },
    {
      id: "loop",
      type: "loop",
      params: {
        count: 2,
        steps: [
          { id: "adc", type: "adc_read", params: { channel: "5v_out" } },
          { id: "expect", type: "serial_expect", params: { channel: "uart0", command: "uname -a", pattern: "Linux", timeout_ms: 5000 } },
        ],
      },
    },
    { id: "capture", type: "capture", params: { rail: "5v_out", trigger: "manual", duration_ms: 1000 } },
    { id: "off", type: "power_off", params: { rail: "5v_out" } },
  ],
};

const plan = buildExecutionPlan(script);
const startedAtMs = Date.now() - 10_000;

function makeResults(captureStatus: StepResult["status"] = "error"): StepResult[] {
  return plan.map((step, index) => ({
    stepId: step.executionId,
    sourceStepId: step.sourceStepId,
    loopId: step.loopId,
    loopIteration: step.loopIteration,
    loopCount: step.loopCount,
    unitId: step.unitId,
    unitName: step.unitName,
    conditionId: step.conditionId,
    conditionRole: step.conditionRole,
    stepType: step.type,
    status: step.executionId === "capture" ? captureStatus : "pass",
    startedAtMs: startedAtMs + index * 100,
    finishedAtMs: startedAtMs + index * 100 + 80,
    durationMs: 80,
    adcValueUa: step.type === "adc_read" ? 182_000 : undefined,
  }));
}

function summary(
  results: StepResult[],
  options: { totalSteps?: number; aborted?: boolean; infrastructureError?: string } = {},
): RunSummary {
  const totalSteps = options.totalSteps ?? plan.length;
  return {
    totalSteps,
    passed: results.filter((result) => result.status === "pass").length,
    failed: results.filter((result) => result.status === "fail").length,
    skipped: results.filter((result) => result.status === "skip").length,
    errored: results.filter((result) => result.status === "error").length,
    aborted: options.aborted ?? false,
    completed: !(options.aborted ?? false)
      && results.length === totalSteps
      && results.every((result) => result.status === "pass" || (result.status === "skip" && result.conditionalSkip)),
    durationMs: 900,
    startedAtMs,
    finishedAtMs: startedAtMs + 900,
    results,
    cleanup: { attempted: true, passed: true, actions: [] },
    infrastructureError: options.infrastructureError
      ?? (results.some((result) => result.status === "error") ? "host_backpressure" : undefined),
  };
}

const incompleteCapture: PowerCapture = {
  id: 5,
  trigger: "manual",
  source: "5v_out",
  edge: "rising",
  thresholdUa: 0,
  rateHz: 500,
  preSamples: 0,
  postSamples: 6000,
  triggerOffset: 0,
  samples: [],
  capturedAt: startedAtMs,
  sampleCount: 6000,
  droppedSamples: 23,
  incomplete: true,
  interruptionReason: "host_backpressure",
  summaries: {
    "5v_out": {
      nominalVoltageV: 5,
      durationMs: 1000,
      averageCurrentA: 0.244,
      peakCurrentA: 0.412,
      averagePowerW: 1.22,
      peakPowerW: 2.06,
      milliampHours: 0.067,
      wattHours: 0.000339,
    },
  },
};

type View = { host: HTMLDivElement; root: Root };
let view: View | null = null;

function mountReport(
  results = makeResults(),
  options: {
    testScript?: TestScript;
    runSummary?: RunSummary;
    captures?: { stepId: string; rail: string; capture: PowerCapture }[];
  } = {},
): HTMLDivElement {
  const host = document.createElement("div");
  document.body.append(host);
  const root = createRoot(host);
  view = { host, root };
  act(() => root.render(
    <TestReport
      script={options.testScript ?? script}
      summary={options.runSummary ?? summary(results)}
      serialLogs={[{ stepId: "capture", channel: "uart0", direction: "rx", text: "capture done", timestampMs: startedAtMs + 700 }]}
      adcSamples={Array.from({ length: 12 }, (_, index) => ({ stepId: "capture", channel: "5v_out", currentUa: 100_000 + index * 10_000, timestampMs: startedAtMs + index * 50 }))}
      powerCaptures={options.captures ?? [{ stepId: "capture", rail: "5v_out", capture: incompleteCapture }]}
      onReRun={vi.fn()}
    />,
  ));
  return host;
}

afterEach(() => {
  if (view) {
    act(() => view?.root.unmount());
    view.host.remove();
  }
  view = null;
  downloadBlob.mockReset();
  document.body.replaceChildren();
});

describe("TestReport result and evidence workspace", () => {
  it("maps the workflow hierarchy and reports incomplete capture evidence as PARTIAL", () => {
    const host = mountReport();
    const workspace = host.querySelector('[data-testid="automation-report-workspace"]');

    expect(workspace?.getAttribute("data-overall-status")).toBe("partial");
    expect(host.querySelector('[data-testid="report-hierarchical-timeline"] [data-group-kind="loop"]')).not.toBeNull();
    expect(host.querySelector('[data-testid="report-incomplete-warning"]')?.textContent).toContain("dropped_samples 23");
    expect(host.textContent).toContain("PARTIAL");
    expect(host.querySelector('[data-testid="report-power-chart"]')).not.toBeNull();
  });

  it("counts a condition's unselected branch as complete without degrading PASS to SKIP", () => {
    const conditionScript: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "condition report",
      steps: [{
        id: "if-ready",
        type: "condition",
        params: {
          check: { id: "check", type: "adc_read", params: { channel: "5v_out" }, assert: { current_range: { min_a: 0.1, max_a: 0.3 } } },
          then_steps: [{ id: "then", type: "delay", params: { ms: 10 } }],
          else_steps: [{ id: "else", type: "delay", params: { ms: 10 } }],
        },
      }],
    };
    const conditionPlan = buildExecutionPlan(conditionScript);
    const results: StepResult[] = conditionPlan.map((step, index) => ({
      stepId: step.executionId,
      sourceStepId: step.sourceStepId,
      conditionId: step.conditionId,
      conditionRole: step.conditionRole,
      stepType: step.type,
      status: step.conditionRole === "else" ? "skip" : "pass",
      conditionalSkip: step.conditionRole === "else",
      startedAtMs: startedAtMs + index,
      finishedAtMs: startedAtMs + index + 1,
      durationMs: 1,
    }));
    const host = mountReport(results, {
      testScript: conditionScript,
      runSummary: summary(results, { totalSteps: conditionPlan.length }),
      captures: [],
    });
    const condition = host.querySelector('[data-group-kind="condition"]');

    expect(condition?.textContent).toContain("test.status.pass");
    expect(condition?.textContent).not.toContain("test.status.skip");
    expect(host.textContent).toContain("3/3 results");
    expect(host.textContent).toContain("3/3 completed");
  });

  it("keeps duplicate child IDs separated by Unit path inside an outer loop", () => {
    const unitScript: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "unit paths",
      steps: [{
        id: "outer",
        type: "loop",
        params: {
          count: 2,
          steps: [
            { id: "unit-a", type: "loop", params: { count: 1, unit: { name: "Unit A" }, steps: [{ id: "shared", type: "delay", params: { ms: 1 } }] } },
            { id: "unit-b", type: "loop", params: { count: 1, unit: { name: "Unit B" }, steps: [{ id: "shared", type: "delay", params: { ms: 1 } }] } },
          ],
        },
      }],
    };
    const unitPlan = buildExecutionPlan(unitScript);
    const results: StepResult[] = unitPlan.map((step, index) => ({
      stepId: step.executionId,
      sourceStepId: step.sourceStepId,
      loopId: step.loopId,
      loopIteration: step.loopIteration,
      loopCount: step.loopCount,
      unitId: step.unitId,
      unitName: step.unitName,
      stepType: step.type,
      status: step.unitId === "unit-b" ? "fail" : "pass",
      startedAtMs: startedAtMs + index,
      finishedAtMs: startedAtMs + index + 1,
      durationMs: 1,
    }));
    const host = mountReport(results, {
      testScript: unitScript,
      runSummary: summary(results, { totalSteps: unitPlan.length }),
      captures: [],
    });
    const unitA = host.querySelector('[data-unit-id="unit-a"]');
    const unitB = host.querySelector('[data-unit-id="unit-b"]');

    expect(unitA?.textContent).toContain("Unit A");
    expect(unitA?.textContent).toContain("2/2 results");
    expect(unitA?.textContent).toContain("test.status.pass");
    expect(unitB?.textContent).toContain("Unit B");
    expect(unitB?.textContent).toContain("2/2 results");
    expect(unitB?.textContent).toContain("test.status.fail");
  });

  it("shows actual versus expected counts and ABORTED for missing results", () => {
    const results = makeResults("pass").slice(0, 1);
    const host = mountReport(results, {
      runSummary: summary(results, { totalSteps: plan.length, aborted: true }),
      captures: [],
    });
    const missingLoop = host.querySelector('[data-testid="report-nav-loop:loop"]');

    expect(host.textContent).toContain(`1/${plan.length} results`);
    expect(host.textContent).toContain(`1/${plan.length} completed`);
    expect(host.textContent).toContain("test.status.aborted");
    expect(missingLoop?.textContent).toContain("0/4 results");
    expect(missingLoop?.textContent).toContain("test.status.aborted");
  });

  it("keeps JSON, CSV, and NDJSON report export entry points functional", () => {
    const host = mountReport();
    for (const testId of ["report-export-json", "report-export-csv", "report-export-ndjson"]) {
      const button = host.querySelector<HTMLButtonElement>(`[data-testid="${testId}"]`);
      if (!button) throw new TypeError(`Missing export button ${testId}`);
      act(() => button.click());
    }

    expect(downloadBlob).toHaveBeenCalledTimes(3);
    expect(downloadBlob.mock.calls.map((call) => call[0])).toEqual(expect.arrayContaining([
      "report-report-hierarchy.json",
      "report-report-hierarchy.csv",
      "report-report-hierarchy.ndjson",
    ]));
  });
});
