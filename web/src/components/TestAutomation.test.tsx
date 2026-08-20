// @vitest-environment jsdom

import { act, createRef } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import type { UseBoard } from "@/hooks/useBoard";
import type { AutomationTaskControl, AutomationTaskOwner } from "@/lib/automationTask";
import type { RunnerCallbacks } from "@/lib/testRunner";
import { preflightTestRun } from "@/lib/testRunner";
import type { RunSummary, TestScript } from "@/lib/testScript";
import type { WorkflowComposerProps } from "./workflow/types";
import type { SerialAutomationHandle } from "./SerialCard";
import { TestAutomation } from "./TestAutomation";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const workflowCapture = vi.hoisted(() => ({
  props: null as WorkflowComposerProps | null,
}));

const runnerCapture = vi.hoisted(() => ({
  callbacks: null as RunnerCallbacks | null,
  scripts: [] as TestScript[],
  abort: vi.fn(),
  start: vi.fn(() => new Promise<void>(() => undefined)),
}));

const reportCapture = vi.hoisted(() => ({
  script: null as TestScript | null,
}));

vi.mock("./WorkflowComposer", () => ({
  WorkflowComposer: (props: WorkflowComposerProps) => {
    workflowCapture.props = props;
    return <div data-testid="workflow-composer" />;
  },
}));

vi.mock("./TestRunnerView", () => ({
  TestRunnerView: ({ onAbort }: { onAbort: () => void }) => (
    <button data-testid="runner-abort" type="button" onClick={onAbort}>Abort</button>
  ),
}));

vi.mock("./TestReport", () => ({
  TestReport: ({ script, onReRun }: { script: TestScript; onReRun: () => void }) => {
    reportCapture.script = script;
    return <button data-testid="report-rerun" type="button" onClick={onReRun}>Re-run</button>;
  },
}));

vi.mock("@/lib/testRunner", () => ({
  preflightTestRun: vi.fn(),
  createTestRunner: vi.fn((
    script: TestScript,
    _board: unknown,
    _serial: unknown,
    callbacks: RunnerCallbacks,
  ) => {
    runnerCapture.scripts.push(script);
    runnerCapture.callbacks = callbacks;
    return { abort: runnerCapture.abort, start: runnerCapture.start };
  }),
}));

vi.mock("@/lib/i18n", () => ({
  useI18n: () => ({
    t: (key: string, values?: Record<string, unknown>) => values
      ? `${key}:${JSON.stringify(values)}`
      : key,
  }),
}));

const VALID_WORKFLOW = [
  JSON.stringify({ schema: "linkr-test.v1", name: "Boot check", version: "1.0" }),
  JSON.stringify({ id: "step-1", type: "delay", params: { ms: 10 } }),
  "",
].join("\n");

const COMPLETED_SUMMARY: RunSummary = {
  totalSteps: 1,
  passed: 1,
  failed: 0,
  skipped: 0,
  errored: 0,
  aborted: false,
  completed: true,
  durationMs: 10,
  startedAtMs: 1,
  finishedAtMs: 11,
  results: [],
  cleanup: { attempted: true, passed: true, actions: [] },
};

type MountOptions = {
  connected?: boolean;
  focusMode?: boolean;
  owner?: AutomationTaskOwner | null;
  acquire?: boolean;
  validWorkflow?: boolean;
};

type View = {
  readonly host: HTMLDivElement;
  readonly focusChange: ReturnType<typeof vi.fn>;
  readonly taskControl: AutomationTaskControl;
  readonly close: () => void;
};

let currentView: View | null = null;

async function mountAutomation({
  connected = true,
  focusMode = false,
  owner = null,
  acquire = true,
  validWorkflow = false,
}: MountOptions = {}): Promise<View> {
  if (validWorkflow) localStorage.setItem("linkr-test-script", VALID_WORKFLOW);
  const host = document.createElement("div");
  document.body.append(host);
  const root: Root = createRoot(host);
  const focusChange = vi.fn();
  const taskControl: AutomationTaskControl = {
    owner,
    acquire: vi.fn(() => acquire),
    release: vi.fn(),
  };
  const board = { connected, snapshot: {} } as unknown as UseBoard;

  await act(async () => {
    root.render(
      <TestAutomation
        board={board}
        serialRef={createRef<SerialAutomationHandle>()}
        taskControl={taskControl}
        focusMode={focusMode}
        onFocusModeChange={focusChange}
      />,
    );
    await Promise.resolve();
  });

  currentView = {
    host,
    focusChange,
    taskControl,
    close: () => act(() => {
      root.unmount();
      host.remove();
    }),
  };
  return currentView;
}

function tab(host: HTMLElement, name: "editor" | "running" | "report") {
  const element = host.querySelector<HTMLButtonElement>(`#automation-tab-${name}`);
  if (!element) throw new TypeError(`Missing ${name} tab`);
  return element;
}

afterEach(() => {
  currentView?.close();
  currentView = null;
  workflowCapture.props = null;
  runnerCapture.callbacks = null;
  runnerCapture.scripts = [];
  runnerCapture.abort.mockReset();
  runnerCapture.start.mockReset();
  runnerCapture.start.mockImplementation(() => new Promise<void>(() => undefined));
  reportCapture.script = null;
  localStorage.clear();
  vi.restoreAllMocks();
  document.body.replaceChildren();
});

describe("TestAutomation workspace shell", () => {
  it("exposes only available views and toggles focus mode", async () => {
    const { host, focusChange } = await mountAutomation();
    const focusButton = host.querySelector<HTMLButtonElement>("#automation-focus-toggle");
    if (!focusButton || !workflowCapture.props) throw new TypeError("Automation editor did not render");

    expect(tab(host, "editor").getAttribute("aria-selected")).toBe("true");
    expect(tab(host, "editor").disabled).toBe(false);
    expect(tab(host, "running").disabled).toBe(true);
    expect(tab(host, "report").disabled).toBe(true);
    expect(focusButton.getAttribute("aria-expanded")).toBe("true");
    expect(focusButton.getAttribute("aria-pressed")).toBe("false");

    act(() => focusButton.click());
    expect(focusChange).toHaveBeenCalledWith(true);
  });

  it("locks editor and report while a workflow is running, then enables the report", async () => {
    const { host } = await mountAutomation({ validWorkflow: true });
    if (!workflowCapture.props) throw new TypeError("Workflow actions were not exposed");

    act(() => workflowCapture.props?.onRun());

    expect(tab(host, "running").getAttribute("aria-selected")).toBe("true");
    expect(tab(host, "running").disabled).toBe(false);
    expect(tab(host, "editor").disabled).toBe(true);
    expect(tab(host, "report").disabled).toBe(true);
    expect(host.querySelector("[data-automation-view='running']")?.getAttribute("aria-busy")).toBe("true");
    expect(host.querySelector("[data-testid='runner-abort']")).not.toBeNull();

    if (!runnerCapture.callbacks) throw new TypeError("Runner callbacks were not captured");
    act(() => runnerCapture.callbacks?.onComplete(COMPLETED_SUMMARY));

    expect(tab(host, "report").getAttribute("aria-selected")).toBe("true");
    expect(tab(host, "report").disabled).toBe(false);
    expect(tab(host, "editor").disabled).toBe(false);
    expect(tab(host, "running").disabled).toBe(true);
    expect(host.querySelector("[data-testid='report-rerun']")).not.toBeNull();
  });

  it("keeps the completed report and re-run bound to the executed workflow snapshot", async () => {
    const { host } = await mountAutomation({ validWorkflow: true });
    if (!workflowCapture.props) throw new TypeError("Workflow actions were not exposed");

    act(() => workflowCapture.props?.onRun());
    if (!runnerCapture.callbacks) throw new TypeError("Runner callbacks were not captured");
    act(() => runnerCapture.callbacks?.onComplete(COMPLETED_SUMMARY));
    expect(reportCapture.script?.name).toBe("Boot check");

    act(() => tab(host, "editor").click());
    if (!workflowCapture.props) throw new TypeError("Workflow editor did not return");
    act(() => workflowCapture.props?.onChange({ ...workflowCapture.props.script, name: "Edited draft" }));
    expect(workflowCapture.props?.script.name).toBe("Edited draft");

    act(() => tab(host, "report").click());
    expect(reportCapture.script?.name).toBe("Boot check");

    const reRun = host.querySelector<HTMLButtonElement>("[data-testid='report-rerun']");
    if (!reRun) throw new TypeError("Report re-run action is missing");
    act(() => reRun.click());
    expect(runnerCapture.scripts).toHaveLength(2);
    expect(runnerCapture.scripts[1].name).toBe("Boot check");
  });

  it("keeps disconnected and occupied-task failures visible without losing the draft", async () => {
    vi.mocked(preflightTestRun).mockImplementationOnce(() => {
      throw new Error("debug board is not connected");
    });
    const { host } = await mountAutomation({ connected: false, validWorkflow: true });
    if (!workflowCapture.props) throw new TypeError("Workflow actions were not exposed");

    expect(workflowCapture.props.runDisabled).toBe(true);
    expect(workflowCapture.props.runDisabledReason).toBe("test.error.boardDisconnected");
    act(() => workflowCapture.props?.onRun());
    expect(host.querySelector("[data-testid='automation-error']")?.textContent)
      .toContain("test.error.boardDisconnected");
    expect(workflowCapture.props.script.name).toBe("Boot check");
  });

  it("reports a competing automation owner as a locked run", async () => {
    const { host } = await mountAutomation({ owner: "startup", acquire: false, validWorkflow: true });
    if (!workflowCapture.props) throw new TypeError("Workflow actions were not exposed");

    expect(workflowCapture.props.runDisabled).toBe(true);
    expect(workflowCapture.props.runDisabledReason).toBe("test.error.taskBusy");
    act(() => workflowCapture.props?.onRun());
    expect(host.querySelector("[data-testid='automation-error']")?.textContent)
      .toContain("test.error.taskBusy");
  });
});

describe("TestAutomation editor state", () => {
  it("starts a confirmed localized draft and exposes its save state", async () => {
    await mountAutomation({ validWorkflow: true });
    vi.spyOn(window, "confirm").mockReturnValue(true);
    if (!workflowCapture.props?.onNew) throw new TypeError("New workflow action not available");

    act(() => workflowCapture.props?.onNew?.());

    expect(window.confirm).toHaveBeenCalledWith("test.new.confirm");
    expect(workflowCapture.props?.script.name).toBe("test.defaultName");
    expect(workflowCapture.props?.draftState).toBe("saved");
    expect(localStorage.getItem("linkr-test-script")).toContain("test.defaultName");
  });
});
