// @vitest-environment jsdom

import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import { buildExecutionPlan, type StepResult, type StepStatus, type TestScript } from "@/lib/testScript";
import { buildExecutionGroups, executionGroupStatus, TestRunnerView, visibleGroupSteps } from "./TestRunnerView";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

vi.mock("@/lib/i18n", () => ({
  useI18n: () => ({
    t: (key: string, values?: Record<string, unknown>) => values ? `${key}:${JSON.stringify(values)}` : key,
  }),
}));

const script: TestScript = {
  schema: "linkr-test.v1",
  version: "1.0",
  name: "hierarchy",
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
    { id: "off", type: "power_off", params: { rail: "5v_out" } },
  ],
};

type View = { host: HTMLDivElement; root: Root };
let view: View | null = null;

function result(stepId: string, status: StepStatus, startedAtMs: number): StepResult {
  const step = buildExecutionPlan(script).find((candidate) => candidate.executionId === stepId);
  if (!step) throw new TypeError(`Missing execution step ${stepId}`);
  return {
    stepId,
    sourceStepId: step.sourceStepId,
    loopId: step.loopId,
    loopIteration: step.loopIteration,
    loopCount: step.loopCount,
    stepType: step.type,
    status,
    startedAtMs,
    finishedAtMs: startedAtMs + 100,
    durationMs: 100,
    adcValueUa: step.type === "adc_read" ? 182_000 : undefined,
  };
}

afterEach(() => {
  if (view) {
    act(() => view?.root.unmount());
    view.host.remove();
  }
  view = null;
  document.body.replaceChildren();
});

describe("TestRunnerView practical hierarchy", () => {
  it("maps expanded loop executions back into one loop with the active iteration", () => {
    const plan = buildExecutionPlan(script);
    const groups = buildExecutionGroups(plan);
    expect(groups.map((group) => group.kind)).toEqual(["step", "loop", "step"]);
    expect(groups[1].steps).toHaveLength(4);

    const states = new Map<string, StepStatus>(plan.map((step) => [step.executionId, "pending"]));
    states.set("adc@loop:1", "pass");
    states.set("expect@loop:1", "pass");
    states.set("adc@loop:2", "running");

    expect(visibleGroupSteps(groups[1], states).map((step) => step.executionId)).toEqual([
      "adc@loop:2",
      "expect@loop:2",
    ]);
  });

  it("renders the current step, nested timeline, live evidence, and abort action", () => {
    const plan = buildExecutionPlan(script);
    const startedAtMs = Date.now() - 1500;
    const states = new Map<string, StepStatus>(plan.map((step) => [step.executionId, "pending"]));
    states.set("power", "pass");
    states.set("adc@loop:1", "pass");
    states.set("expect@loop:1", "running");
    const results = [
      result("power", "pass", startedAtMs),
      result("adc@loop:1", "pass", startedAtMs + 100),
    ];
    const onAbort = vi.fn();
    const host = document.createElement("div");
    document.body.append(host);
    const root = createRoot(host);
    view = { host, root };

    act(() => root.render(
      <TestRunnerView
        steps={plan}
        stepStates={states}
        stepResults={results}
        serialLogs={[{ stepId: "expect@loop:1", channel: "uart0", direction: "rx", text: "Linux radxa", timestampMs: startedAtMs + 500 }]}
        adcSamples={[{ stepId: "adc@loop:1", channel: "5v_out", currentUa: 182_000, timestampMs: startedAtMs + 300 }]}
        startedAtMs={startedAtMs}
        onAbort={onAbort}
      />,
    ));

    expect(host.querySelector('[data-testid="automation-running-workspace"]')).not.toBeNull();
    expect(host.querySelector('[data-group-kind="loop"]')).not.toBeNull();
    expect(host.querySelector('[data-testid="runner-step-expect@loop:1"]')?.getAttribute("data-status")).toBe("running");
    expect(host.textContent).toContain("Linux radxa");
    const evidence = host.querySelector('[data-testid="runner-evidence-inspector"]');
    expect(evidence?.textContent).not.toContain("182.0 mA");
    expect(evidence?.textContent).toContain("test.report.samples0");
    expect(host.textContent).toContain("test.running.cleanupPending");
    expect(host.textContent).not.toContain("test.report.cleanupPassed");

    const abortButton = [...host.querySelectorAll("button")].find((button) => button.textContent?.includes("test.abort"));
    if (!abortButton) throw new TypeError("Abort button missing");
    act(() => abortButton.click());
    expect(onAbort).toHaveBeenCalledOnce();
  });

  it("treats the unselected conditional branch as a completed neutral result", () => {
    const conditionalScript: TestScript = {
      schema: "linkr-test.v1",
      version: "1.0",
      name: "condition",
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
    const conditionalPlan = buildExecutionPlan(conditionalScript);
    const group = buildExecutionGroups(conditionalPlan)[0];
    const states = new Map<string, StepStatus>();
    const results = new Map<string, StepResult>(conditionalPlan.map((step) => [step.executionId, {
      stepId: step.executionId,
      sourceStepId: step.sourceStepId,
      conditionId: step.conditionId,
      conditionRole: step.conditionRole,
      stepType: step.type,
      status: step.conditionRole === "else" ? "skip" : "pass",
      conditionalSkip: step.conditionRole === "else",
      startedAtMs: 1,
      finishedAtMs: 2,
      durationMs: 1,
    }]));

    expect(executionGroupStatus(group, states, results)).toBe("pass");
  });
});
