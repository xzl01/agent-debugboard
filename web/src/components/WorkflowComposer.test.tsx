import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import { defaultScript, type TestScript } from "@/lib/testScript";
import { WorkflowComposer } from "./WorkflowComposer";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

vi.mock("@/lib/i18n", () => ({
  useI18n: () => ({
    t: (key: string, values?: Record<string, unknown>) => values ? `${key}:${JSON.stringify(values)}` : key,
  }),
}));

type View = {
  readonly host: HTMLDivElement;
  readonly close: () => void;
};

let currentView: View | null = null;

function practicalScript(): TestScript {
  return {
    schema: "linkr-test.v1",
    name: "Startup and interface validation",
    version: "1.0",
    steps: [
      { id: "power-on", type: "power_on", params: { rail: "5V_OUT" } },
      {
        id: "serial-wait",
        type: "serial_wait",
        params: { channel: "uart0", pattern: "login:", timeout_ms: 60_000 },
      },
      {
        id: "loop-1",
        type: "loop",
        params: {
          count: 2,
          steps: [
            { id: "loop-send", type: "serial_send", params: { channel: "uart0", text: "root\\n" } },
            {
              id: "loop-adc",
              type: "adc_read",
              params: { channel: "5V_OUT" },
              assert: { current_range: { min_a: 0.15, max_a: 0.25 } },
            },
          ],
        },
      },
      {
        id: "condition-1",
        type: "condition",
        params: {
          check: {
            id: "condition-check",
            type: "adc_read",
            params: { channel: "5V_OUT" },
            assert: { current_range: { min_a: 0, max_a: 0.25 } },
          },
          then_steps: [
            { id: "then-gpio", type: "gpio_assert", params: { pin: "GP10", direction: "output", value: 1 } },
          ],
          else_steps: [
            { id: "else-power-off", type: "power_off", params: { rail: "5V_OUT" } },
          ],
        },
      },
      {
        id: "capture-1",
        type: "capture",
        params: { rail: "5V_OUT", trigger: "manual", duration_ms: 12_000 },
      },
      { id: "power-off", type: "power_off", params: { rail: "5V_OUT" } },
    ],
  };
}

function mountComposer(script: TestScript = defaultScript()): View {
  const host = document.createElement("div");
  document.body.append(host);
  const root: Root = createRoot(host);
  act(() => root.render(
    <WorkflowComposer
      script={script}
      onChange={vi.fn()}
      onRun={vi.fn()}
    />
  ));
  currentView = {
    host,
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

afterEach(() => {
  currentView?.close();
  currentView = null;
  localStorage.clear();
  document.body.replaceChildren();
});

describe("WorkflowComposer panel layout", () => {
  it("lets the Unit library and inspector be hidden independently without hiding the canvas", () => {
    const { host } = mountComposer();
    const layoutControls = host.querySelector<HTMLElement>('[aria-label="test.workflow.layoutControls"]');
    if (!layoutControls) throw new TypeError("Desktop layout controls not found");
    const libraryToggle = button(layoutControls, "test.workflow.library");
    const inspectorToggle = button(layoutControls, "test.workflow.inspector");
    const libraryPanel = host.querySelector<HTMLElement>('[data-testid="workflow-library-panel"]');
    const inspectorPanel = host.querySelector<HTMLElement>('[data-testid="workflow-inspector-panel"]');
    if (!libraryPanel || !inspectorPanel) throw new TypeError("Workflow side panels not found");

    expect(libraryToggle.getAttribute("aria-pressed")).toBe("true");
    expect(inspectorToggle.getAttribute("aria-pressed")).toBe("true");
    expect(libraryPanel.classList.contains("xl:contents")).toBe(true);
    expect(inspectorPanel.classList.contains("xl:contents")).toBe(true);

    act(() => libraryToggle.click());
    expect(libraryToggle.getAttribute("aria-pressed")).toBe("false");
    expect(libraryPanel.classList.contains("xl:hidden")).toBe(true);
    expect(host.querySelector('main[aria-label="test.workflow.canvas"]')).not.toBeNull();
    expect(inspectorPanel.classList.contains("xl:contents")).toBe(true);

    act(() => inspectorToggle.click());
    expect(inspectorToggle.getAttribute("aria-pressed")).toBe("false");
    expect(inspectorPanel.classList.contains("xl:hidden")).toBe(true);
    expect(host.querySelector('main[aria-label="test.workflow.canvas"]')).not.toBeNull();
  });

  it("shows one workflow panel at a time on compact layouts and defaults to the canvas", () => {
    const { host } = mountComposer();
    const mobileControls = host.querySelector<HTMLElement>('[aria-label="test.workflow.mobilePanels"]');
    const canvasPanel = host.querySelector<HTMLElement>('[data-testid="workflow-canvas-panel"]');
    const libraryPanel = host.querySelector<HTMLElement>('[data-testid="workflow-library-panel"]');
    if (!mobileControls || !canvasPanel || !libraryPanel) throw new TypeError("Mobile workflow panels not found");
    const canvasToggle = button(mobileControls, "test.workflow.canvasShort");
    const libraryToggle = button(mobileControls, "test.workflow.library");

    expect(canvasToggle.getAttribute("aria-pressed")).toBe("true");
    expect(canvasPanel.classList.contains("block")).toBe(true);
    expect(libraryPanel.classList.contains("hidden")).toBe(true);

    act(() => libraryToggle.click());

    expect(libraryToggle.getAttribute("aria-pressed")).toBe("true");
    expect(libraryPanel.classList.contains("block")).toBe(true);
    expect(canvasPanel.classList.contains("hidden")).toBe(true);
  });
});

describe("WorkflowComposer practical editor model", () => {
  it("keeps primitive Units directly available for click and drag", () => {
    const { host } = mountComposer(practicalScript());
    const library = host.querySelector<HTMLElement>('aside[aria-label="test.workflow.library"]');
    if (!library) throw new TypeError("Unit library not found");

    expect(button(library, "test.step.power_on").draggable).toBe(true);
    expect(button(library, "test.step.serial_send").draggable).toBe(true);
    expect(button(library, "test.step.adc_read").draggable).toBe(true);
    expect(button(library, "test.loop.title")).toBeInstanceOf(HTMLButtonElement);
    expect(button(library, "test.condition.title")).toBeInstanceOf(HTMLButtonElement);
  });

  it("renders the supplied script as top-level nodes with real nested loop and condition rows", () => {
    const { host } = mountComposer(practicalScript());
    expect(host.querySelectorAll("[data-workflow-item-id]")).toHaveLength(6);

    const loop = host.querySelector<HTMLElement>('[data-workflow-item-id="loop-1"]');
    const condition = host.querySelector<HTMLElement>('[data-workflow-item-id="condition-1"]');
    if (!loop || !condition) throw new TypeError("Nested workflow nodes not found");

    expect(loop.querySelectorAll("[data-workflow-nested-item-id]")).toHaveLength(2);
    expect(loop.textContent).toContain("test.step.serial_send");
    expect(loop.textContent).toContain("test.step.adc_read");
    expect(condition.querySelectorAll("[data-workflow-nested-item-id]")).toHaveLength(2);
    expect(condition.textContent).toContain("test.condition.then");
    expect(condition.textContent).toContain("test.condition.else");
  });

  it("shows configuration only for the selected canvas node", () => {
    const { host } = mountComposer(practicalScript());
    const loop = host.querySelector<HTMLElement>('[data-workflow-item-id="loop-1"]');
    const inspector = host.querySelector<HTMLElement>('aside[aria-label="test.workflow.inspector"]');
    if (!loop || !inspector) throw new TypeError("Workflow loop or inspector not found");

    expect(inspector.textContent).toContain("test.step.power_on");
    const loopSelect = [...loop.querySelectorAll("button")].find((item) => item.textContent?.includes("test.loop.title"));
    if (!(loopSelect instanceof HTMLButtonElement)) throw new TypeError("Loop select control not found");
    act(() => loopSelect.click());

    expect(inspector.textContent).toContain("test.loop.title");
    expect(inspector.textContent).toContain('test.workflow.containedSteps:{"n":2}');
    const rounds = inspector.querySelector<HTMLInputElement>('input[aria-label="test.loop.rounds"]');
    expect(rounds?.value).toBe("2");
  });
});
