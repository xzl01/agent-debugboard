import { afterEach, describe, expect, it } from "vitest";
import type { WatchdogStatus } from "@/lib/types";
import { WatchdogCard } from "./WatchdogCard";
import { mount } from "./GpioPinoutSvg.testUtils";

afterEach(() => { document.body.replaceChildren(); localStorage.clear(); });

const watchdog: WatchdogStatus = {
  supported: true, healthy: true, armed: true, automatic: true,
  timeout_ms: 5000, bootloader_on_timeout: true, failing_service: "",
};

describe("WatchdogCard status layout", () => {
  it("lets all four complete status badges wrap rather than overflow the card", () => {
    const view = mount(<WatchdogCard watchdog={watchdog} />);
    const statuses = view.host.querySelector('[data-testid="watchdog-statuses"]')!;
    expect(statuses.classList.contains("flex-wrap")).toBe(true);
    expect(statuses.classList.contains("min-w-0")).toBe(true);
    expect(statuses.classList.contains("[&>span]:max-w-full")).toBe(true);
    expect(Array.from(statuses.children).map((badge) => badge.textContent))
      .toEqual(["healthy", "armed", "automatic", "bootloader on timeout"]);
    expect(view.host.querySelector('[class*="overflow-hidden"]')).toBeNull();
    view.close();
  });

  it("keeps firmware-owned health and optional status flags unchanged", () => {
    const view = mount(<WatchdogCard watchdog={{ ...watchdog, healthy: false, armed: false,
      automatic: false, bootloader_on_timeout: false, failing_service: "worker" }} />);
    const statuses = view.host.querySelector('[data-testid="watchdog-statuses"]')!;
    expect(Array.from(statuses.children).map((badge) => badge.textContent)).toEqual(["unhealthy"]);
    expect(view.host.textContent).toContain("5000 ms");
    expect(view.host.textContent).toContain("worker");
    expect(view.host.querySelector("button")).toBeNull();
    view.close();
  });
});
