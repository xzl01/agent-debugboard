import { act } from "react";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import {
  armStreamingCapture,
  emitCaptureTriggered,
  sentCommands,
} from "./useBoard.fixtures";
import {
  flushMicrotasks,
  powerCaptureStoreMocks,
  renderLiveBoard,
  setupUseBoardHarness,
  teardownUseBoardHarness,
} from "./useBoard.testUtils";

describe("useBoard capture cleanup", () => {
  beforeEach(setupUseBoardHarness);
  afterEach(teardownUseBoardHarness);

  it("deletes the active archive after queued writes settle on cancel", async () => {
    // Given a recording capture
    const { view, socket } = await renderLiveBoard();
    await armStreamingCapture(view, socket);
    emitCaptureTriggered(socket);

    // When the capture is cancelled
    act(() => {
      view.current().cancelCapture();
    });

    // Then the cancel command is sent and the state resets
    expect(sentCommands(socket).some((cmd) =>
      cmd.command === "capture_cancel" && cmd.id === "web-cancel"
    )).toBe(true);
    expect(view.current().captureState).toBe("idle");

    // And the active archive is deleted once queued writes settle
    await flushMicrotasks();
    expect(powerCaptureStoreMocks.deletePowerCaptureArchive).toHaveBeenCalledTimes(1);
    view.close();
  });

  it("keeps the active archive when clearing capture history", async () => {
    // Given a recording capture
    const { view, socket } = await renderLiveBoard();
    await armStreamingCapture(view, socket);
    emitCaptureTriggered(socket);
    powerCaptureStoreMocks.listRecentPowerCaptures.mockResolvedValue([]);

    // When the history is cleared
    act(() => {
      view.current().clearCaptures();
    });
    await flushMicrotasks();

    // Then the active archive is excluded and the capture keeps recording
    expect(powerCaptureStoreMocks.clearPowerCaptureArchives).toHaveBeenCalledWith({
      activeArchiveId: expect.any(String),
    });
    expect(view.current().captures).toEqual([]);
    expect(view.current().captureState).toBe("recording");
    view.close();
  });
});
