import { act } from "react";
import { createRoot } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import { LanguageProvider } from "@/lib/i18n";
import {
  computeOtaSha256Hex,
  confirmOtaImage,
  getOtaStatus,
  startOtaTest,
  uploadOtaImage,
  type OtaStatus,
} from "@/lib/ota";
import {
  createAutomationTaskLock,
  type AutomationTaskControl,
} from "@/lib/automationTask";
import { BootCard } from "./BootCard";
import { OtaCard } from "./OtaCard";

vi.mock("@/lib/ota", async (importOriginal) => {
  const original = await importOriginal<typeof import("@/lib/ota")>();
  return {
    ...original,
    computeOtaSha256Hex: vi.fn().mockResolvedValue("a".repeat(64)),
    confirmOtaImage: vi.fn().mockResolvedValue(undefined),
    startOtaTest: vi.fn().mockResolvedValue(undefined),
    uploadOtaImage: vi.fn().mockResolvedValue({
      state: "verified",
      expectedSize: 1,
      writtenSize: 1,
      maxSize: 2048,
      currentImageConfirmed: false,
      lastError: null,
    }),
    getOtaStatus: vi.fn().mockResolvedValue({
      state: "pending_test",
      expectedSize: 1024,
      writtenSize: 1024,
      maxSize: 2048,
      currentImageConfirmed: false,
      lastError: null,
    }),
  };
});

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

function render(node: React.ReactNode) {
  localStorage.setItem("lang", "en");
  const host = document.createElement("div");
  document.body.append(host);
  const root = createRoot(host);
  act(() => root.render(<LanguageProvider>{node}</LanguageProvider>));
  return {
    host,
    close: () => act(() => {
      root.unmount();
      host.remove();
    }),
  };
}

function deferred<T>() {
  let resolve: (value: T) => void = () => undefined;
  const promise = new Promise<T>((done) => {
    resolve = done;
  });
  return { promise, resolve };
}

function taskControl(lock = createAutomationTaskLock()): AutomationTaskControl {
  return {
    get owner() {
      return lock.owner();
    },
    acquire: lock.acquire,
    release: lock.release,
  };
}

async function clickAndFlush(button: HTMLButtonElement): Promise<void> {
  await act(async () => {
    button.click();
    await Promise.resolve();
    await Promise.resolve();
  });
}

function button(host: HTMLElement, text: string): HTMLButtonElement {
  const result = [...host.querySelectorAll<HTMLButtonElement>("button")]
    .find((candidate) => candidate.textContent?.includes(text));
  if (!result) throw new TypeError(`Button not found: ${text}`);
  return result;
}

describe("maintenance safety controls", () => {
  afterEach(() => {
    document.body.replaceChildren();
    localStorage.clear();
    vi.clearAllMocks();
  });

  it("keeps ROM BOOTSEL disabled offline and names the only safe full UF2", () => {
    const view = render(<BootCard onBoot={vi.fn()} disabled />);
    expect(view.host.textContent).toContain("radxa-linkr-debugger-rp2350.uf2");
    expect(view.host.textContent).toContain("Never flash application-only zephyr.uf2");
    expect(view.host.textContent).toContain("radxa-linkr-debugger-rp2350-ota.bin");
    expect((view.host.querySelector("button") as HTMLButtonElement).disabled).toBe(true);
    view.close();
  });

  it("clears stale BOOTSEL success and reports a later failure inline", async () => {
    const onBoot = vi.fn()
      .mockResolvedValueOnce(undefined)
      .mockRejectedValueOnce(new Error("USB request failed"));
    vi.spyOn(window, "confirm").mockReturnValue(true);
    const view = render(<BootCard onBoot={onBoot} />);

    await clickAndFlush(button(view.host, "Enter BOOTSEL"));
    expect(view.host.textContent).toContain("BOOTSEL requested");

    await clickAndFlush(button(view.host, "Enter BOOTSEL"));
    expect(view.host.textContent).not.toContain("BOOTSEL requested");
    expect(view.host.querySelector('[role="alert"]')?.textContent).toContain("USB request failed");
    view.close();
  });

  it("holds the global owner through BOOTSEL and releases it on completion", async () => {
    const request = deferred<void>();
    const lock = createAutomationTaskLock();
    vi.spyOn(window, "confirm").mockReturnValue(true);
    const view = render(
      <BootCard onBoot={() => request.promise} taskControl={taskControl(lock)} />,
    );

    act(() => button(view.host, "Enter BOOTSEL").click());
    expect(lock.acquire("test")).toBe(false);
    await act(async () => request.resolve());
    expect(lock.acquire("test")).toBe(true);
    view.close();
  });

  it("keeps recovery exposed only through tasks, with no dedicated recovery API or card", async () => {
    const { existsSync } = await import("node:fs");
    const api = await import("@/lib/api");
    expect("enterTargetRecovery" in api).toBe(false);
    expect(Object.values(api).every((value) => typeof value !== "function" ||
      String(value).indexOf("/target-recovery") === -1)).toBe(true);
    expect(existsSync(new URL("./TargetRecoveryCard.tsx", import.meta.url))).toBe(false);
  });

  it("shows the OTA lifecycle and locks firmware mutations while disconnected or task-owned", () => {
    const status: OtaStatus = {
      state: "pending_test",
      expectedSize: 1024,
      writtenSize: 1024,
      maxSize: 2048,
      currentImageConfirmed: false,
      lastError: null,
    };
    const view = render(<OtaCard status={status} setStatus={vi.fn()} disabled />);
    expect(view.host.textContent).toContain("Writes locked");
    expect(view.host.textContent).toContain("UploadVerifyTest bootConfirmComplete");
    expect(view.host.textContent).toContain("radxa-linkr-debugger-rp2350-ota.bin");
    expect(view.host.textContent).toContain("zephyr.uf2");
    const buttons = [...view.host.querySelectorAll("button")];
    expect(buttons.find((button) => button.textContent?.includes("Start test boot"))?.disabled).toBe(true);
    expect(buttons.find((button) => button.textContent?.includes("Confirm image"))?.disabled).toBe(true);
    view.close();
  });

  it("keeps automation from starting while an OTA test boot is in flight", async () => {
    const status: OtaStatus = {
      state: "verified",
      expectedSize: 1024,
      writtenSize: 1024,
      maxSize: 2048,
      currentImageConfirmed: false,
      lastError: null,
    };
    const request = deferred<void>();
    vi.mocked(getOtaStatus).mockResolvedValue(status);
    vi.mocked(startOtaTest).mockReturnValueOnce(request.promise);
    vi.spyOn(window, "confirm").mockReturnValue(true);
    const lock = createAutomationTaskLock();
    const view = render(
      <OtaCard
        status={status}
        setStatus={vi.fn()}
        taskControl={taskControl(lock)}
      />,
    );

    const testBoot = button(view.host, "Start test boot");
    expect(testBoot.disabled).toBe(false);
    await act(async () => {
      testBoot.click();
      await Promise.resolve();
    });
    expect(window.confirm).toHaveBeenCalled();
    expect(lock.acquire("test")).toBe(false);
    await act(async () => request.resolve());
    expect(lock.acquire("test")).toBe(true);
    view.close();
  });

  it("owns the task during OTA upload and confirm mutations", async () => {
    const verified: OtaStatus = {
      state: "verified",
      expectedSize: 1,
      writtenSize: 1,
      maxSize: 2048,
      currentImageConfirmed: false,
      lastError: null,
    };
    const pending: OtaStatus = { ...verified, state: "pending_test" };
    vi.mocked(getOtaStatus).mockResolvedValue(verified);
    vi.mocked(computeOtaSha256Hex).mockResolvedValue("a".repeat(64));
    const upload = deferred<OtaStatus>();
    vi.mocked(uploadOtaImage).mockReturnValueOnce(upload.promise);
    const uploadLock = createAutomationTaskLock();
    const uploadView = render(
      <OtaCard status={verified} setStatus={vi.fn()} taskControl={taskControl(uploadLock)} />,
    );
    const input = uploadView.host.querySelector<HTMLInputElement>('input[type="file"]');
    if (!input) throw new TypeError("OTA input not found");
    Object.defineProperty(input, "files", {
      configurable: true,
      value: [new File(["x"], "radxa-linkr-debugger-rp2350-ota.bin")],
    });
    await act(async () => {
      input.dispatchEvent(new Event("change", { bubbles: true }));
      await Promise.resolve();
      await Promise.resolve();
    });
    act(() => button(uploadView.host, "Upload image").click());
    expect(uploadLock.acquire("test")).toBe(false);
    await act(async () => upload.resolve(verified));
    expect(uploadLock.acquire("test")).toBe(true);
    uploadView.close();

    vi.mocked(getOtaStatus).mockResolvedValue(pending);
    const confirm = deferred<void>();
    vi.mocked(confirmOtaImage).mockReturnValueOnce(confirm.promise);
    const confirmLock = createAutomationTaskLock();
    const confirmView = render(
      <OtaCard status={pending} setStatus={vi.fn()} taskControl={taskControl(confirmLock)} />,
    );
    act(() => button(confirmView.host, "Confirm image").click());
    expect(confirmLock.acquire("test")).toBe(false);
    await act(async () => confirm.resolve());
    expect(confirmLock.acquire("test")).toBe(true);
    confirmView.close();
  });
});
