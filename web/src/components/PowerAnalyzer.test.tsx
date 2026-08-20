import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import type { CaptureConfig, PowerCapture } from "@/lib/types";
import type { AutomationTaskControl } from "@/lib/automationTask";
import { PowerAnalyzer } from "./PowerAnalyzer";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

vi.mock("@/lib/i18n", () => ({
  useI18n: () => ({ t: (key: string) => key }),
}));

vi.mock("@/lib/powerCaptureStore", () => ({
  getPowerCaptureStoragePlan: () => new Promise(() => {}),
}));

const exportMocks = vi.hoisted(() => ({
  exportPowerCaptureToFile: vi.fn(() => Promise.resolve()),
}));

vi.mock("@/lib/powerCaptureExport", () => ({
  exportPowerCaptureToFile: exportMocks.exportPowerCaptureToFile,
}));

const capture: PowerCapture = {
  id: 7,
  trigger: "manual",
  source: "5v_out",
  edge: "either",
  thresholdUa: 0,
  rateHz: 50,
  preSamples: 0,
  postSamples: 1,
  triggerOffset: 0,
  capturedAt: 1,
  samples: [
    {
      offset: 0,
      triggered: true,
      sampleSequence: 0,
      deviceTimeUs: 0,
      readings: [{
        name: "5v_out",
        signal: "current",
        value: 500_000,
        kind: "current",
        power_enabled: true,
        raw: null,
        mv: 0,
        sensor_channel: "current",
        unit: "uA",
        current_ua: 500_000,
      }],
    },
    {
      offset: 1,
      triggered: false,
      sampleSequence: 1,
      deviceTimeUs: 20_000,
      readings: [{
        name: "5v_out",
        signal: "current",
        value: 500_000,
        kind: "current",
        power_enabled: true,
        raw: null,
        mv: 0,
        sensor_channel: "current",
        unit: "uA",
        current_ua: 500_000,
      }],
    },
  ],
};

type CaptureState = "idle" | "connecting" | "armed" | "recording" | "receiving";

type View = {
  readonly host: HTMLDivElement;
  readonly root: Root;
  readonly render: (state: CaptureState, captures: PowerCapture[]) => void;
  readonly close: () => void;
};

type MountOptions = {
  taskControl?: AutomationTaskControl;
  onArm?: (config: CaptureConfig) => Promise<void>;
  onCancel?: () => void;
  onClear?: () => void;
};

let currentView: View | null = null;

function mount(state: CaptureState = "idle", captures: PowerCapture[] = [], options: MountOptions = {}): View {
  const host = document.createElement("div");
  document.body.append(host);
  const root = createRoot(host);
  const taskControl = options.taskControl ?? { owner: null, acquire: vi.fn(() => true), release: vi.fn() };
  const onArm = options.onArm ?? vi.fn(() => Promise.resolve());
  const onCancel = options.onCancel ?? vi.fn();
  const onClear = options.onClear ?? vi.fn();
  const render = (nextState: CaptureState, nextCaptures: PowerCapture[]) => act(() => {
    root.render(
      <PowerAnalyzer
        gpios={[]}
        state={nextState}
        progress={null}
        captures={nextCaptures}
        onArm={onArm}
        onTrigger={vi.fn()}
        onStop={vi.fn()}
        onCancel={onCancel}
        onClear={onClear}
        taskControl={taskControl}
        showHeader={false}
      />
    );
  });
  render(state, captures);
  currentView = {
    host,
    root,
    render,
    close: () => act(() => {
      root.unmount();
      host.remove();
    }),
  };
  return currentView;
}

function button(host: HTMLElement, text: string): HTMLButtonElement {
  const match = [...host.querySelectorAll("button")].find((item) => item.textContent?.includes(text));
  if (!(match instanceof HTMLButtonElement)) throw new TypeError(`Button not found: ${text}`);
  return match;
}

function click(element: HTMLElement): void {
  act(() => element.click());
}

afterEach(() => {
  currentView?.close();
  currentView = null;
  document.body.replaceChildren();
  vi.restoreAllMocks();
  vi.clearAllMocks();
});

describe("PowerAnalyzer task workflow", () => {
  it("shows configuration first without adding a third navigation layer", () => {
    const { host } = mount();

    expect(host.querySelector('[aria-label="analyzer.stage.navigation"]')).toBeNull();
    expect(host.textContent).toContain("analyzer.stage.setup");
    expect(host.textContent).toContain("analyzer.guide.triggerTitle");
    expect(host.textContent).not.toContain("analyzer.guide.captureTitle");
    expect(host.querySelector('[data-testid="power-capture-sticky-action"]')).not.toBeNull();
    expect(host.querySelectorAll("details")).toHaveLength(2);

    click(button(host, "analyzer.stage.continue"));

    expect(host.textContent).toContain("analyzer.stage.capture");
    expect(host.textContent).not.toContain("analyzer.guide.triggerTitle");
    expect(host.textContent).toContain("analyzer.guide.captureTitle");
    expect(host.textContent).toContain("50 Hz");
  });

  it("opens saved captures as results and exposes archive evidence beside the chart", () => {
    const { host } = mount("idle", [capture]);

    expect(host.textContent).toContain("analyzer.stage.results");
    expect(host.textContent).toContain("analyzer.results.title");
    expect(host.textContent).toContain("#7");
    expect(host.textContent).toContain("analyzer.receivedSamples");
    expect(host.querySelector('[aria-label="power.chart.metric"]')).not.toBeNull();
    expect(host.textContent).not.toContain("analyzer.guide.triggerTitle");

    const newCaptureButton = button(host, "analyzer.stage.new");
    expect(newCaptureButton.className).toContain("bg-brand");
    expect(host.textContent).toContain("analyzer.stage.newHint");
    click(newCaptureButton);

    expect(host.textContent).toContain("analyzer.stage.setup");
    expect(host.textContent).toContain("analyzer.guide.triggerTitle");
  });

  it("follows an active capture and advances to results when a new record arrives", () => {
    const view = mount();

    view.render("armed", []);
    expect(view.host.textContent).toContain("analyzer.stage.capture");
    expect(view.host.textContent).toContain("analyzer.guide.captureTitle");

    view.render("idle", [capture]);
    expect(view.host.textContent).toContain("analyzer.stage.results");
    expect(view.host.textContent).toContain("analyzer.results.title");
  });

  it("binds metrics and export to the selected history record", async () => {
    const older = { ...capture, id: 6, capturedAt: 0 };
    const { host } = mount("idle", [older, capture]);

    const olderButton = button(host, "#6");
    click(olderButton);
    expect(olderButton.getAttribute("aria-pressed")).toBe("true");

    await act(async () => {
      button(host, "CSV").click();
      await Promise.resolve();
    });
    expect(exportMocks.exportPowerCaptureToFile).toHaveBeenCalledWith(
      older,
      "csv",
      expect.any(Function),
    );
  });

  it("confirms before clearing every saved capture", () => {
    const onClear = vi.fn();
    const confirm = vi.spyOn(window, "confirm").mockReturnValueOnce(false).mockReturnValueOnce(true);
    const { host } = mount("idle", [capture], { onClear });

    click(button(host, "analyzer.clearAll"));
    expect(onClear).not.toHaveBeenCalled();
    click(button(host, "analyzer.clearAll"));
    expect(confirm).toHaveBeenCalledWith("analyzer.clearConfirm");
    expect(onClear).toHaveBeenCalledTimes(1);
  });

  it("does not arm while another global hardware task owns the capture pipeline", () => {
    const onArm = vi.fn(() => Promise.resolve());
    const { host } = mount("idle", [], {
      onArm,
      taskControl: { owner: "test", acquire: vi.fn(() => false), release: vi.fn() },
    });
    click(button(host, "analyzer.stage.continue"));

    const armButton = button(host, "analyzer.arm");
    expect(armButton.disabled).toBe(true);
    click(armButton);
    expect(onArm).not.toHaveBeenCalled();
  });

  it("holds the manual capture lock through an idle parent render until capture actually completes", async () => {
    const release = vi.fn();
    const acquire = vi.fn(() => true);
    const view = mount("idle", [], {
      taskControl: { owner: null, acquire, release },
    });
    click(button(view.host, "analyzer.stage.continue"));
    await act(async () => {
      button(view.host, "analyzer.arm").click();
      await Promise.resolve();
    });
    expect(acquire).toHaveBeenCalledWith("power");

    view.render("idle", []);
    expect(release).not.toHaveBeenCalled();
    view.render("armed", []);
    expect(release).not.toHaveBeenCalled();
    view.render("idle", []);
    expect(release).toHaveBeenCalledWith("power");
  });

  it("cancels an active manual capture before releasing its lock on unmount", async () => {
    const release = vi.fn();
    const onCancel = vi.fn();
    const view = mount("idle", [], {
      onCancel,
      taskControl: { owner: null, acquire: () => true, release },
    });
    click(button(view.host, "analyzer.stage.continue"));
    await act(async () => {
      button(view.host, "analyzer.arm").click();
      await Promise.resolve();
    });
    view.render("recording", []);
    view.close();
    currentView = null;

    expect(onCancel).toHaveBeenCalledTimes(1);
    expect(release).toHaveBeenCalledWith("power");
  });
});
