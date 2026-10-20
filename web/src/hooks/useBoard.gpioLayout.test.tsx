import { act } from "react";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { physicalLayoutFixture } from "@/lib/gpioPhysicalLayout.testFixtures";
import { apiMocks, flushMicrotasks, renderBoard, renderLiveBoard, setupUseBoardHarness, teardownUseBoardHarness } from "./useBoard.testUtils";

describe("useBoard physical layout lifecycle", () => {
  beforeEach(setupUseBoardHarness);
  afterEach(teardownUseBoardHarness);

  it("waits for the first successful status, then reads once across repeated polls", async () => {
    apiMocks.getStatus.mockRejectedValueOnce(new Error("offline"));
    apiMocks.getGpioLayout.mockResolvedValue(physicalLayoutFixture().layout);
    const view = renderBoard();
    await flushMicrotasks();
    expect(apiMocks.getGpioLayout).not.toHaveBeenCalled();
    await act(async () => { await view.current().refresh(); });
    await flushMicrotasks();
    expect(apiMocks.getGpioLayout).toHaveBeenCalledTimes(1);
    expect(view.current().gpioPhysicalLayout).not.toBeNull();
    await act(async () => { await view.current().refresh(); await view.current().refresh(); });
    expect(apiMocks.getGpioLayout).toHaveBeenCalledTimes(1);
    view.close();
  });

  it("does not mark the board offline or hot-retry when layout fails", async () => {
    apiMocks.getGpioLayout.mockRejectedValue(new Error("layout unsupported"));
    const view = renderBoard();
    await flushMicrotasks();
    expect(view.current().connected).toBe(true);
    expect(view.current().error).toBeNull();
    expect(view.current().gpioLayoutError).toBe("layout unsupported");
    await act(async () => { await view.current().refresh(); });
    expect(apiMocks.getGpioLayout).toHaveBeenCalledTimes(1);
    view.close();
  });

  it("aborts an old epoch and ignores its late result after reconnect", async () => {
    let resolveOld: (value: ReturnType<typeof physicalLayoutFixture>["layout"]) => void = () => {};
    apiMocks.getGpioLayout.mockImplementationOnce(() => new Promise((resolve) => { resolveOld = resolve; }));
    const view = renderBoard();
    await flushMicrotasks();
    const oldSignal = apiMocks.getGpioLayout.mock.calls[0][0]!;
    expect(view.current().gpioLayoutLoading).toBe(true);
    apiMocks.getStatus.mockRejectedValueOnce(new Error("disconnected"));
    await act(async () => { await view.current().refresh(); });
    expect(oldSignal.aborted).toBe(true);
    const next = { view: "component-top" as const, groups: [] };
    apiMocks.getGpioLayout.mockResolvedValueOnce(next);
    await act(async () => { await view.current().refresh(); });
    await flushMicrotasks();
    expect(apiMocks.getGpioLayout).toHaveBeenCalledTimes(2);
    await act(async () => resolveOld(physicalLayoutFixture().layout));
    expect(view.current().gpioPhysicalLayout).toBe(next);
    expect(view.current().gpioLayoutError).toBeNull();
    view.close();
  });

  it("aborts on unmount, and live-mode seed reads reuse the same epoch", async () => {
    const { view, socket } = await renderLiveBoard();
    expect(apiMocks.getGpioLayout).toHaveBeenCalledTimes(1);
    act(() => socket.emitClose());
    await flushMicrotasks();
    expect(apiMocks.getGpioLayout).toHaveBeenCalledTimes(2);
    const signal = apiMocks.getGpioLayout.mock.calls[1][0]!;
    view.close();
    expect(signal.aborted).toBe(true);
  });

  it("does not start a layout request when an old status completes after unmount", async () => {
    let finishStatus: (value: unknown) => void = () => {};
    apiMocks.getStatus.mockImplementationOnce(() => new Promise((resolve) => { finishStatus = resolve; }));
    const view = renderBoard();
    view.close();
    await act(async () => finishStatus({ gpios: [], power_outputs: [] }));
    await flushMicrotasks();
    expect(apiMocks.getGpioLayout).not.toHaveBeenCalled();
  });
});
