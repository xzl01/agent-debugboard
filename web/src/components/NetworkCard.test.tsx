// @vitest-environment jsdom

import { act } from "react";
import { createRoot } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { getNetSettings, setNetIdentity } from "@/lib/api";
import { LanguageProvider } from "@/lib/i18n";
import { NetworkCard } from "./NetworkCard";

vi.mock("@/lib/api", async (importOriginal) => ({
  ...(await importOriginal<typeof import("@/lib/api")>()),
  getNetSettings: vi.fn(),
  setNetIdentity: vi.fn(),
}));

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const getNetSettingsMock = vi.mocked(getNetSettings);
const setNetIdentityMock = vi.mocked(setNetIdentity);

const baseIdentity = {
  mac: "02:11:22:33:44:55",
  origin: "derived" as const,
  network: "10.0.0.0",
  prefix: 30 as const,
  ip: "10.0.0.1",
  dhcp_base: "10.0.0.2",
  url: "http://10.0.0.1/",
};

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

async function applyNetwork(host: HTMLElement, network: string) {
  act(() => setInputValue(host.querySelector<HTMLInputElement>('[data-testid="network-input"]')!, network));
  await act(async () => host.querySelector<HTMLButtonElement>('[data-testid="network-apply"]')!.click());
}

describe("NetworkCard", () => {
  beforeEach(() => {
    getNetSettingsMock.mockResolvedValue(baseIdentity);
    setNetIdentityMock.mockResolvedValue({ ...baseIdentity, network: "10.0.0.4", ip: "10.0.0.5", dhcp_base: "10.0.0.6", url: "http://10.0.0.5/", origin: "manual" });
  });

  afterEach(() => {
    document.body.replaceChildren();
    localStorage.clear();
    vi.clearAllMocks();
  });

  it("shows the persisted identity, addresses, and board URL hint", async () => {
    const view = await render();

    expect(view.host.querySelector<HTMLInputElement>('[data-testid="network-mac-input"]')?.value).toBe(baseIdentity.mac);
    expect(view.host.querySelector<HTMLInputElement>('[data-testid="network-input"]')?.value).toBe("10.0.0.0/30");
    expect(view.host.textContent).toContain("10.0.0.1");
    expect(view.host.textContent).toContain("derived from chip ID");
    expect(view.host.querySelector('[data-testid="network-hint"]')?.textContent).toContain("http://10.0.0.1/");
    view.close();
  });

  it("applies only the changed network and reports the resulting URL", async () => {
    const view = await render();
    const input = view.host.querySelector<HTMLInputElement>('[data-testid="network-input"]');
    if (!input) throw new TypeError("network input not found");

    act(() => setInputValue(input, "10.0.0.4/30"));
    const apply = view.host.querySelector<HTMLButtonElement>('[data-testid="network-apply"]');
    if (!apply) throw new TypeError("apply button not found");
    await act(async () => apply.click());

    expect(setNetIdentityMock).toHaveBeenCalledWith({ network: "10.0.0.4" });
    expect(view.host.querySelector('[data-testid="network-current"]')?.textContent).toContain("10.0.0.4/30");
    expect(view.host.textContent).toContain("10.0.0.5");
    expect(view.host.querySelector('[role="status"]')?.textContent).toContain("http://10.0.0.5/");
    expect(view.host.querySelector('[role="status"]')?.textContent).toContain(
      "Disconnect and reconnect the USB NCM interface, or renew its DHCP lease.",
    );
    view.close();
  });

  it("applies only a changed MAC", async () => {
    const view = await render();
    const input = view.host.querySelector<HTMLInputElement>('[data-testid="network-mac-input"]');
    if (!input) throw new TypeError("MAC input not found");

    act(() => setInputValue(input, "02:AA:BB:CC:DD:EE"));
    const apply = view.host.querySelector<HTMLButtonElement>('[data-testid="network-apply"]');
    if (!apply) throw new TypeError("apply button not found");
    await act(async () => apply.click());

    expect(setNetIdentityMock).toHaveBeenCalledWith({ mac: "02:aa:bb:cc:dd:ee" });
    view.close();
  });

  it("does not apply an unchanged identity", async () => {
    const view = await render();
    const apply = view.host.querySelector<HTMLButtonElement>('[data-testid="network-apply"]');
    expect(apply?.disabled).toBe(true);
    expect(setNetIdentityMock).not.toHaveBeenCalled();
    view.close();
  });

  it("applies both fields when both change", async () => {
    const view = await render();
    const mac = view.host.querySelector<HTMLInputElement>('[data-testid="network-mac-input"]')!;
    const network = view.host.querySelector<HTMLInputElement>('[data-testid="network-input"]')!;
    act(() => setInputValue(mac, "02:aa:bb:cc:dd:ee"));
    act(() => setInputValue(network, "10.0.0.4/30"));
    await act(async () => view.host.querySelector<HTMLButtonElement>('[data-testid="network-apply"]')!.click());
    expect(setNetIdentityMock).toHaveBeenCalledWith({ mac: "02:aa:bb:cc:dd:ee", network: "10.0.0.4" });
    view.close();
  });

  it("rejects invalid identities without calling the firmware", async () => {
    const view = await render();
    const mac = view.host.querySelector<HTMLInputElement>('[data-testid="network-mac-input"]')!;
    const network = view.host.querySelector<HTMLInputElement>('[data-testid="network-input"]')!;
    const apply = view.host.querySelector<HTMLButtonElement>('[data-testid="network-apply"]')!;

    act(() => setInputValue(mac, "00:11:22:33:44:55"));
    expect(apply.disabled).toBe(true);
    act(() => setInputValue(mac, baseIdentity.mac));
    act(() => setInputValue(network, "10.0.0.1/30"));
    expect(apply.disabled).toBe(true);
    act(() => setInputValue(network, "172.15.0.0/30"));
    expect(apply.disabled).toBe(true);
    act(() => setInputValue(network, "172.29.0.0/30"));
    expect(apply.disabled).toBe(false);
    expect(setNetIdentityMock).not.toHaveBeenCalled();
    view.close();
  });

  it("surfaces apply failures", async () => {
    setNetIdentityMock.mockRejectedValue(new Error("failed to apply network identity"));
    const view = await render();
    const input = view.host.querySelector<HTMLInputElement>('[data-testid="network-input"]');
    if (!input) throw new TypeError("network input not found");

    act(() => setInputValue(input, "10.0.0.4/30"));
    const apply = view.host.querySelector<HTMLButtonElement>('[data-testid="network-apply"]');
    if (!apply) throw new TypeError("apply button not found");
    await act(async () => apply.click());

    expect(view.host.querySelector('[role="alert"]')?.textContent).toContain(
      "failed to apply network identity",
    );
    view.close();
  });

  it.each([
    ["en", "Disconnect and reconnect the USB NCM interface, or renew its DHCP lease."],
    ["zh", "请断开并重新连接 USB NCM 接口，或重新获取 DHCP 租约。"],
  ] as const)("localizes the fallback reconnect notice in %s", async (lang, expected) => {
    setNetIdentityMock.mockResolvedValue({ ...baseIdentity, network: "10.0.0.4", reconnect_required: true });
    const view = await render(lang);
    await applyNetwork(view.host, "10.0.0.4/30");
    expect(view.host.querySelector('[role="status"]')?.textContent).toContain(expected);
    if (lang === "zh") expect(view.host.textContent).not.toContain("Disconnect and reconnect");
    view.close();
  });

  it("honors firmware reconnect_required=false even when the network changes", async () => {
    setNetIdentityMock.mockResolvedValue({ ...baseIdentity, network: "10.0.0.4", reconnect_required: false });
    const view = await render("zh");
    await applyNetwork(view.host, "10.0.0.4/30");
    expect(view.host.querySelector('[role="status"]')?.textContent).toContain("网络身份已保存");
    expect(view.host.textContent).not.toContain("重新连接 USB NCM");
    view.close();
  });

  it("honors firmware reconnect_required=true for a MAC-only change", async () => {
    setNetIdentityMock.mockResolvedValue({ ...baseIdentity, mac: "02:aa:bb:cc:dd:ee", reconnect_required: true });
    const view = await render();
    const mac = view.host.querySelector<HTMLInputElement>('[data-testid="network-mac-input"]')!;
    act(() => setInputValue(mac, "02:aa:bb:cc:dd:ee"));
    await act(async () => view.host.querySelector<HTMLButtonElement>('[data-testid="network-apply"]')!.click());
    expect(view.host.querySelector('[role="status"]')?.textContent).toContain("Disconnect and reconnect the USB NCM interface");
    view.close();
  });

  it.each([true, false])("prefers the firmware message over generic reconnect advice when required=%s", async (reconnect_required) => {
    const message = "Renew DHCP without disconnecting this device.";
    setNetIdentityMock.mockResolvedValue({ ...baseIdentity, network: "10.0.0.4", reconnect_required, message });
    const view = await render("zh");
    await applyNetwork(view.host, "10.0.0.4/30");
    const notice = view.host.querySelector('[role="status"]')?.textContent;
    expect(notice).toContain(message);
    expect(notice).toContain("http://10.0.0.1/");
    expect(notice).not.toContain("请断开并重新连接");
    view.close();
  });
});
