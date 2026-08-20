// @vitest-environment jsdom

import { act } from "react";
import { createRoot } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import type { UsePersistentConfig } from "@/hooks/usePersistentConfig";
import { ConfigurationWorkspace } from "./ConfigurationWorkspace";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

vi.mock("@/lib/i18n", () => ({
  useI18n: () => ({ t: (key: string) => key }),
}));
vi.mock("./PersistentConfigCard", () => ({
  PersistentConfigCard: () => <div data-testid="persistent-config-card" />,
}));
vi.mock("./OtaCard", () => ({ OtaCard: () => <div data-testid="ota-card" /> }));
vi.mock("./BootCard", () => ({ BootCard: () => <div data-testid="boot-card" /> }));

const persistentConfig: UsePersistentConfig = {
  config: {
    backend: { available: true, reason: "" },
    snapshot: { present: true, version: 1 },
    pending: 1,
    items: [
      {
        id: "power/5v_out",
        kind: "power",
        selected: true,
        risk: "safe",
        applyState: "applied",
        current: { kind: "power", state: "on" },
        saved: { kind: "power", state: "on" },
      },
      {
        id: "power/12v_out",
        kind: "power",
        selected: false,
        risk: "safe",
        applyState: "not_saved",
        current: { kind: "power", state: "off" },
        saved: null,
      },
    ],
  },
  error: null,
  loading: false,
  busy: null,
  supported: true,
  refresh: vi.fn(async () => {}),
  save: vi.fn(async () => {}),
  clear: vi.fn(async () => {}),
};

function render() {
  const host = document.createElement("div");
  document.body.append(host);
  const root = createRoot(host);
  act(() => {
    root.render(
      <ConfigurationWorkspace
        connected
        persistentConfig={persistentConfig}
        onEnterBootloader={vi.fn(async () => {})}
        ota={null}
        setOta={vi.fn()}
        disabled={false}
        taskControl={{ owner: null, acquire: vi.fn(() => true), release: vi.fn() }}
      />,
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

describe("ConfigurationWorkspace", () => {
  afterEach(() => document.body.replaceChildren());

  it("starts with saved configuration and keeps target recovery out of this workspace", () => {
    const view = render();

    expect(view.host.querySelector('[data-testid="persistent-config-card"]')).not.toBeNull();
    expect(view.host.querySelector('[data-testid="configuration-summary-strip"]')).not.toBeNull();
    expect(view.host.querySelector('[data-testid="configuration-section-tab-recovery"]')).toBeNull();
    expect(view.host.querySelector('[data-testid="target-recovery-card"]')).toBeNull();
    expect(view.host.textContent).toContain("1");
    expect(view.host.querySelector('[data-testid="ota-card"]')).toBeNull();
    view.close();
  });

  it("switches to a focused firmware maintenance panel", () => {
    const view = render();
    const firmwareTab = view.host.querySelector<HTMLButtonElement>('[data-testid="configuration-section-tab-firmware"]');
    if (!firmwareTab) throw new TypeError("Firmware tab not found");

    act(() => firmwareTab.click());

    expect(firmwareTab.getAttribute("aria-selected")).toBe("true");
    expect(view.host.querySelector('[data-testid="ota-card"]')).not.toBeNull();
    expect(view.host.querySelector('[data-testid="boot-card"]')).not.toBeNull();
    expect(view.host.querySelector('[data-testid="persistent-config-card"]')).toBeNull();
    view.close();
  });
});
