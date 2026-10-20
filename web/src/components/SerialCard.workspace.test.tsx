// @vitest-environment jsdom
import { act, createRef } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import { J4SerialSession } from "@/lib/j4SerialSession";
import { j4ReadySnapshot } from "@/lib/j4Serial.testFixtures";
import { SerialCard, type SerialAutomationHandle } from "./SerialCard";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
vi.mock("@/lib/i18n", () => ({ useI18n: () => ({ t: (key: string) => key }) }));
vi.mock("./SerialTerminalPane", () => ({ SerialTerminalPane: () => <div data-testid="serial-terminal-j4">J4</div> }));
let root: Root | null = null;
let host: HTMLDivElement;
function mount() {
  const session = new J4SerialSession();
  const ready = { ...j4ReadySnapshot(), automationActive: false };
  vi.spyOn(session, "getState").mockReturnValue(ready);
  vi.spyOn(session, "isConnected").mockReturnValue(true);
  vi.spyOn(session, "write").mockResolvedValue();
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
  const ref = createRef<SerialAutomationHandle>(); const onConnectionChange = vi.fn();
  act(() => root!.render(<SerialCard session={session} ref={ref} onConnectionChange={onConnectionChange} />));
  return { ref, session, onConnectionChange };
}
afterEach(() => { act(() => root?.unmount()); host?.remove(); root = null; vi.restoreAllMocks(); });

describe("single J4 workspace", () => {
  it("shows one terminal and reports J4 MCU state without channel/layout selectors", () => {
    const { onConnectionChange } = mount();
    expect(host.querySelectorAll('[data-testid="serial-terminal-j4"]')).toHaveLength(1);
    expect(host.querySelector('[role="tablist"]')).toBeNull();
    expect(host.textContent).toContain("J4 · MCU");
    expect(onConnectionChange).toHaveBeenLastCalledWith({ j4: true, state: "ready" });
  });
  it("exposes the same backend owner to automation, without a channel mapping or DOM adapter", async () => {
    const { ref, session } = mount();
    expect(ref.current).toBe(session);
    expect(ref.current?.connectedChannels()).toEqual(["j4"]);
    await ref.current?.write("J4", "j4");
    expect(session.write).toHaveBeenCalledWith("J4", "j4");
    expect(ref.current?.getState().serial?.state).toBe("ready");
  });
});
