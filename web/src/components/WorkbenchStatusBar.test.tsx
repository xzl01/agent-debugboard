// @vitest-environment jsdom

import { act } from "react";
import { createRoot } from "react-dom/client";
import { afterEach, describe, expect, it } from "vitest";
import { LanguageProvider } from "@/lib/i18n";
import type { BoardSnapshot } from "@/lib/types";
import { WorkbenchStatusBar } from "./WorkbenchStatusBar";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const snapshot: BoardSnapshot = {
  powerOutputs: [
    { name: "5v_out", controllable: true, state: "on", value: 1 },
    { name: "12v_out", controllable: true, state: "off", value: 0 },
    { name: "20v_out", controllable: true, state: "off", value: 0 },
    { name: "vdd_5v", controllable: true, state: "on", value: 1 },
  ],
  switches: {
    sd: { route: "target" },
    usb: { route: "target" },
    tf_wp: { route: "writable" },
    vin: { route: "3.3v" },
  },
  gpios: [],
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
  readonly uart0?: boolean;
  readonly uart1?: boolean;
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
            uart0: options?.uart0 ?? false,
            uart1: options?.uart1 ?? false,
            bridgeActive: false,
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
    const view = render({ uart0: true, uart1: true, taskOwner: "power" });

    expect(view.host.textContent).toContain("SD→SBC");
    expect(view.host.textContent).toContain("USB→SBC");
    expect(view.host.textContent).toContain("TF Writable");
    expect(view.host.textContent).toContain("VIN 3.3V");
    expect(view.host.textContent).toContain("UART0/1 connected");
    expect(view.host.textContent).toContain("power capture");
    view.close();
  });

  it("marks an unreachable board as offline without changing the rail facts", () => {
    const view = render({ connected: false });
    const bar = view.host.querySelector('[data-testid="workbench-status-bar"]');

    expect(bar?.textContent).toContain("debugger offline");
    expect(bar?.querySelector(".bg-danger")).not.toBeNull();
    expect(bar?.textContent).toContain("5V on");
    view.close();
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
});
