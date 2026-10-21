// @vitest-environment jsdom

import { act } from "react";
import { createRoot } from "react-dom/client";
import { afterEach, describe, expect, it } from "vitest";
import { LanguageProvider } from "@/lib/i18n";
import type { BoardSnapshot } from "@/lib/types";
import { WorkbenchStatusBar } from "./WorkbenchStatusBar";
import { x12ControlStatusFixture } from "@/lib/switches.testFixtures";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const controls = x12ControlStatusFixture();
const snapshot: BoardSnapshot = {
  powerOutputs: [
    { name: "5v_out", controllable: true, state: "on", value: 1 },
    { name: "12v_out", controllable: true, state: "off", value: 0 },
    { name: "20v_out", controllable: true, state: "off", value: 0 },
    { name: "vdd_5v", controllable: true, state: "on", value: 1 },
  ],
  switches: controls.switches,
  gpios: controls.gpios,
  gpioLayoutGroups: controls.gpio_layout_groups,
  watchdog: {
    supported: true,
    automatic: true,
    healthy: true,
    armed: true,
    timeout_ms: 5000,
    bootloader_on_timeout: false,
    failing_service: "",
  },
  monitoring: {
    temperature: { available: false },
    heap: { available: false },
    runtime: { available: false },
    cpu: { available: false },
  },
  adc: [],
};

function render(options?: {
  readonly snapshot?: BoardSnapshot;
  readonly connected?: boolean;
  readonly j4?: boolean;
  readonly serialState?: import("@/lib/j4SerialSession").J4SerialConnectionState;
  readonly taskOwner?: "power" | null;
}) {
  localStorage.setItem("lang", "en");
  const host = document.createElement("div");
  document.body.append(host);
  const root = createRoot(host);
  act(() => {
    root.render(
      <LanguageProvider>
        <WorkbenchStatusBar
          snapshot={options?.snapshot ?? snapshot}
          connected={options?.connected ?? true}
          serialConnections={{
            j4: options?.j4 ?? false,
            state: options?.serialState ?? (options?.j4 ? "ready" : "disconnected"),
          }}
          taskOwner={options?.taskOwner ?? null}
        />
      </LanguageProvider>,
    );
  });
  return {
    host,
    close: () => act(() => {
      root.unmount();
      host.remove();
    }),
  };
}

describe("WorkbenchStatusBar", () => {
  afterEach(() => {
    document.body.replaceChildren();
    localStorage.clear();
  });

  it("shows every power rail with a restrained on or off lamp and readable text", () => {
    const view = render();

    for (const rail of ["5v_out", "vdd_5v"]) {
      expect(view.host.querySelector(`[data-testid="status-rail-lamp-${rail}"]`)?.classList.contains("bg-ok")).toBe(true);
    }
    for (const rail of ["12v_out", "20v_out"]) {
      expect(view.host.querySelector(`[data-testid="status-rail-lamp-${rail}"]`)?.classList.contains("bg-line")).toBe(true);
    }
    expect(view.host.textContent).toContain("5V on");
    expect(view.host.textContent).toContain("12V off");
    expect(view.host.textContent).toContain("20V off");
    expect(view.host.textContent).toContain("VDD_5V on");
    view.close();
  });

  it("uses live routing, UART and task state instead of static status copy", () => {
    const view = render({ j4: true, taskOwner: "power" });

    expect(view.host.textContent).toContain("SD→SBC");
    expect(view.host.textContent).toContain("TF_WP→Writable");
    expect(view.host.textContent).not.toContain("USB→");
    expect(view.host.textContent).not.toContain("VIN");
    expect([...view.host.querySelectorAll('[data-testid^="status-route-"]')].map((item) => item.getAttribute("data-testid"))).toEqual(["status-route-sd", "status-route-tf_wp"]);
    expect(snapshot.gpios).toHaveLength(11);
    expect(view.host.textContent).toContain("J4 connected");
    expect(view.host.textContent).toContain("power capture");
    view.close();
  });

  it("renders an arbitrary advertised aux route and never invents unreported entries", () => {
    const view = render({ snapshot: { ...snapshot, switches: { aux: { route: "lane-b", routes: ["lane-a", "lane-b"], requires_confirm: true } } } });
    expect(view.host.textContent).toContain("AUX→lane-b");
    expect(view.host.querySelector('[data-testid="status-route-aux"]')?.getAttribute("title")).toBe("aux → lane-b");
    expect(view.host.querySelector('[data-testid="status-route-sd"]')).toBeNull();
    expect(view.host.querySelector('[data-testid="status-route-usb"]')).toBeNull();
    view.close();
    const empty = render({ snapshot: { ...snapshot, switches: {} } });
    expect(empty.host.querySelector('[data-testid^="status-route-"]')).toBeNull();
    empty.close();
  });

  it("marks an unreachable board as offline without changing the rail facts", () => {
    const view = render({ connected: false });
    const bar = view.host.querySelector('[data-testid="workbench-status-bar"]');

    expect(bar?.textContent).toContain("debugger offline");
    expect(bar?.querySelector(".bg-danger")).not.toBeNull();
    expect(bar?.textContent).toContain("5V on");
    view.close();
  });

  it("distinguishes a connected J4 socket from a UART that is not ready", () => {
    for (const [state, label] of [["paused", "J4 suspended"], ["faulted", "J4 fault"], ["read_only", "J4 read-only"], ["reconfig", "J4 applying settings"]] as const) {
      const view = render({ j4: true, serialState: state });
      expect(view.host.textContent).toContain(label);
      expect(view.host.textContent).not.toContain("J4 connected");
      view.close();
    }
  });

  it("keeps non-binary firmware rail states unknown", () => {
    const view = render({
      snapshot: {
        ...snapshot,
        powerOutputs: [
          { name: "5v_out", controllable: true, state: "locked", value: null },
        ],
      },
    });

    expect(view.host.textContent).toContain("5V —");
    expect(
      view.host
        .querySelector('[data-testid="status-rail-lamp-5v_out"]')
        ?.classList.contains("bg-transparent"),
    ).toBe(true);
    view.close();
  });

  it("shows new advertised outputs without inventing unreported rail lamps", () => {
    const view = render({ snapshot: { ...snapshot, powerOutputs: [{ name: "future_aux", controllable: true, state: "on", value: 1 }] } });
    expect(view.host.querySelector('[data-testid="status-rail-future_aux"]')?.textContent).toContain("FUTURE_AUX on");
    expect(view.host.querySelector('[data-testid="status-rail-5v_out"]')).toBeNull();
    view.close();
  });
});
