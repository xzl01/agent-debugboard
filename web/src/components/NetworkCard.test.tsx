// @vitest-environment jsdom

import { act } from "react";
import { createRoot } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { getNetSettings, setNetSubnet } from "@/lib/api";
import { LanguageProvider } from "@/lib/i18n";
import { NetworkCard } from "./NetworkCard";

vi.mock("@/lib/api", () => ({
  getNetSettings: vi.fn(),
  setNetSubnet: vi.fn(),
}));

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const getNetSettingsMock = vi.mocked(getNetSettings);
const setNetSubnetMock = vi.mocked(setNetSubnet);

async function render(lang: "en" | "zh" = "en") {
  localStorage.setItem("lang", lang);
  const host = document.createElement("div");
  document.body.append(host);
  const root = createRoot(host);
  await act(async () => {
    root.render(
      <LanguageProvider>
        <NetworkCard connected />
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

function setInputValue(input: HTMLInputElement, value: string) {
  const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")?.set;
  setter?.call(input, value);
  input.dispatchEvent(new Event("input", { bubbles: true }));
}

async function applySubnet(host: HTMLElement, subnet: string) {
  act(() => setInputValue(host.querySelector<HTMLInputElement>('[data-testid="network-subnet-input"]')!, subnet));
  await act(async () => host.querySelector<HTMLButtonElement>('[data-testid="network-apply"]')!.click());
}

describe("NetworkCard", () => {
  beforeEach(() => {
    getNetSettingsMock.mockResolvedValue({ subnet: 203, ip: "172.29.203.1" });
    setNetSubnetMock.mockResolvedValue({ subnet: 204, ip: "172.29.204.1" });
  });

  afterEach(() => {
    document.body.replaceChildren();
    localStorage.clear();
    vi.clearAllMocks();
  });

  it("shows the persisted subnet, current IP, and board URL hint", async () => {
    const view = await render();

    const input = view.host.querySelector<HTMLInputElement>('[data-testid="network-subnet-input"]');
    expect(input?.value).toBe("203");
    expect(view.host.textContent).toContain("172.29.203.1");
    expect(view.host.querySelector('[data-testid="network-hint"]')?.textContent).toContain(
      "http://172.29.203.1/",
    );
    view.close();
  });

  it("applies a new subnet and reports the resulting URL", async () => {
    const view = await render();
    const input = view.host.querySelector<HTMLInputElement>('[data-testid="network-subnet-input"]');
    if (!input) throw new TypeError("subnet input not found");

    act(() => setInputValue(input, "204"));
    expect(view.host.querySelector('[data-testid="network-hint"]')?.textContent).toContain(
      "http://172.29.204.1/",
    );

    const apply = view.host.querySelector<HTMLButtonElement>('[data-testid="network-apply"]');
    if (!apply) throw new TypeError("apply button not found");
    await act(async () => apply.click());

    expect(setNetSubnetMock).toHaveBeenCalledWith(204);
    expect(view.host.querySelector('[data-testid="network-current"]')?.textContent).toContain("204");
    expect(view.host.textContent).toContain("172.29.204.1");
    expect(view.host.querySelector('[role="status"]')?.textContent).toContain("http://172.29.204.1/");
    expect(view.host.querySelector('[role="status"]')?.textContent).toContain(
      "Disconnect and reconnect the USB NCM interface, or renew its DHCP lease.",
    );
    view.close();
  });

  it("rejects an out-of-range octet without calling the firmware", async () => {
    const view = await render();
    const input = view.host.querySelector<HTMLInputElement>('[data-testid="network-subnet-input"]');
    if (!input) throw new TypeError("subnet input not found");

    act(() => setInputValue(input, "255"));

    const apply = view.host.querySelector<HTMLButtonElement>('[data-testid="network-apply"]');
    expect(apply?.disabled).toBe(true);
    expect(setNetSubnetMock).not.toHaveBeenCalled();
    view.close();
  });

  it("surfaces apply failures", async () => {
    setNetSubnetMock.mockRejectedValue(new Error("failed to apply network subnet"));
    const view = await render();
    const input = view.host.querySelector<HTMLInputElement>('[data-testid="network-subnet-input"]');
    if (!input) throw new TypeError("subnet input not found");

    act(() => setInputValue(input, "204"));
    const apply = view.host.querySelector<HTMLButtonElement>('[data-testid="network-apply"]');
    if (!apply) throw new TypeError("apply button not found");
    await act(async () => apply.click());

    expect(view.host.querySelector('[role="alert"]')?.textContent).toContain(
      "failed to apply network subnet",
    );
    view.close();
  });

  it.each([
    ["en", "Disconnect and reconnect the USB NCM interface, or renew its DHCP lease."],
    ["zh", "请断开并重新连接 USB NCM 接口，或重新获取 DHCP 租约。"],
  ] as const)("localizes the fallback reconnect notice in %s", async (lang, expected) => {
    setNetSubnetMock.mockResolvedValue({ subnet: 204, ip: "172.29.204.1", reconnect_required: true });
    const view = await render(lang);
    await applySubnet(view.host, "204");
    expect(view.host.querySelector('[role="status"]')?.textContent).toContain(expected);
    if (lang === "zh") expect(view.host.textContent).not.toContain("Disconnect and reconnect");
    view.close();
  });

  it("honors firmware reconnect_required=false even when the subnet changes", async () => {
    setNetSubnetMock.mockResolvedValue({ subnet: 204, ip: "172.29.204.1", reconnect_required: false });
    const view = await render("zh");
    await applySubnet(view.host, "204");
    expect(view.host.querySelector('[role="status"]')?.textContent).toContain("子网已保存并应用");
    expect(view.host.textContent).not.toContain("重新连接 USB NCM");
    view.close();
  });

  it("honors firmware reconnect_required=true even when the subnet is unchanged", async () => {
    setNetSubnetMock.mockResolvedValue({ subnet: 203, ip: "172.29.203.1", reconnect_required: true });
    const view = await render();
    await applySubnet(view.host, "203");
    expect(view.host.querySelector('[role="status"]')?.textContent).toContain("Disconnect and reconnect the USB NCM interface");
    view.close();
  });

  it.each([true, false])("prefers the firmware message over generic reconnect advice when required=%s", async (reconnect_required) => {
    const message = "Renew DHCP without disconnecting this device.";
    setNetSubnetMock.mockResolvedValue({ subnet: 204, ip: "172.29.204.1", reconnect_required, message });
    const view = await render("zh");
    await applySubnet(view.host, "204");
    const notice = view.host.querySelector('[role="status"]')?.textContent;
    expect(notice).toContain(message);
    expect(notice).toContain("http://172.29.204.1/");
    expect(notice).not.toContain("请断开并重新连接");
    view.close();
  });
});
