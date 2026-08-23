import { act } from "react";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { defaultLiveSubscribeMessage } from "@/lib/liveSubscribe";
import {
  flushNextFrame,
  renderLiveBoard,
  scheduledFrameCount,
  setupUseBoardHarness,
  teardownUseBoardHarness,
  telemetryMessage,
} from "./useBoard.testUtils";

describe("useBoard animation-frame telemetry preview", () => {
  beforeEach(setupUseBoardHarness);
  afterEach(teardownUseBoardHarness);

  it("subscribes at 60 Hz with batch_size 1 on the wire when live telemetry opens", async () => {
    // Given a live board session
    const { view, socket } = await renderLiveBoard();

    // When the socket opens
    // Then the wire subscription keeps the 60 Hz single-sample contract
    expect(JSON.parse(socket.sent[0] ?? "")).toEqual(defaultLiveSubscribeMessage());
    expect(defaultLiveSubscribeMessage().rate_hz).toBe(60);
    expect(defaultLiveSubscribeMessage().batch_size).toBe(1);
    view.close();
  });

  it("publishes only the latest readings on one animation frame when a foreground burst arrives", async () => {
    // Given an open live session
    const { view, socket } = await renderLiveBoard();

    // When several telemetry frames arrive before the next animation frame
    act(() => {
      socket.emitMessage(telemetryMessage(100, 1));
      socket.emitMessage(telemetryMessage(200, 2));
      socket.emitMessage(telemetryMessage(300, 3));
    });

    // Then nothing is published until the frame fires, and a single frame is pending
    expect(view.current().snapshot.adc).toEqual([]);
    expect(scheduledFrameCount()).toBe(1);

    // When the animation frame fires
    flushNextFrame();

    // Then exactly the latest readings reach the snapshot
    const adc = view.current().snapshot.adc;
    expect(adc).toHaveLength(1);
    expect(adc[0]?.value).toBe(300);
    expect(scheduledFrameCount()).toBe(0);
    view.close();
  });

  it("drops the pending preview frame without publishing when the live socket closes", async () => {
    // Given a scheduled preview frame on an open live session
    const { view, socket } = await renderLiveBoard();
    act(() => {
      socket.emitMessage(telemetryMessage(100, 1));
    });
    expect(scheduledFrameCount()).toBe(1);

    // When the socket closes before the frame fires
    act(() => {
      socket.emitClose();
    });

    // Then the frame is cancelled and the stale reading is never published
    expect(scheduledFrameCount()).toBe(0);
    expect(view.current().snapshot.adc).toEqual([]);
    view.close();
  });

  it("drops the pending preview frame without publishing when live telemetry is disabled", async () => {
    // Given a scheduled preview frame on an open live session
    const { view, socket } = await renderLiveBoard();
    act(() => {
      socket.emitMessage(telemetryMessage(100, 1));
    });
    expect(scheduledFrameCount()).toBe(1);

    // When live telemetry is disabled before the frame fires
    act(() => {
      view.current().setLive(false);
    });

    // Then the frame is cancelled and the stale reading is never published
    expect(scheduledFrameCount()).toBe(0);
    expect(view.current().snapshot.adc).toEqual([]);
    view.close();
  });

  it("drops the pending preview frame when the hook unmounts", async () => {
    // Given a scheduled preview frame on an open live session
    const { view, socket } = await renderLiveBoard();
    act(() => {
      socket.emitMessage(telemetryMessage(100, 1));
    });
    expect(scheduledFrameCount()).toBe(1);

    // When the hook unmounts before the frame fires
    view.close();

    // Then the frame is cancelled
    expect(scheduledFrameCount()).toBe(0);
  });
});
