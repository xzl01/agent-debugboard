import { describe, expect, it, vi } from "vitest";
import { createTelemetryFrameScheduler } from "./telemetryFrameScheduler";

interface FrameQueue {
  requestFrame: (callback: () => void) => number;
  cancelFrame: (handle: number) => void;
  flushNext: () => void;
  scheduledCount: () => number;
}

function createFrameQueue(): FrameQueue {
  const callbacks = new Map<number, () => void>();
  let nextHandle = 1;
  const requestFrame = vi.fn((callback: () => void): number => {
    const handle = nextHandle;
    nextHandle += 1;
    callbacks.set(handle, callback);
    return handle;
  });
  const cancelFrame = vi.fn((handle: number): void => {
    callbacks.delete(handle);
  });
  const flushNext = (): void => {
    const entry = callbacks.entries().next();
    if (entry.done) throw new Error("no scheduled frame to flush");
    const [handle, callback] = entry.value;
    callbacks.delete(handle);
    callback();
  };
  return { requestFrame, cancelFrame, flushNext, scheduledCount: () => callbacks.size };
}

describe("createTelemetryFrameScheduler", () => {
  it("publishes only the latest value when a burst schedules before the frame fires", () => {
    // Given a scheduler with an injected frame queue
    const frames = createFrameQueue();
    const publish = vi.fn();
    const scheduler = createTelemetryFrameScheduler<number>(publish, frames);

    // When several values arrive before the next frame
    scheduler.schedule(1);
    scheduler.schedule(2);
    scheduler.schedule(3);
    frames.flushNext();

    // Then the consumer sees exactly one publish with the newest value
    expect(publish).toHaveBeenCalledTimes(1);
    expect(publish).toHaveBeenCalledWith(3);
  });

  it("requests a single frame when a burst schedules before the frame fires", () => {
    // Given a scheduler with an injected frame queue
    const frames = createFrameQueue();
    const scheduler = createTelemetryFrameScheduler<number>(vi.fn(), frames);

    // When several values arrive before the next frame
    scheduler.schedule(1);
    scheduler.schedule(2);
    scheduler.schedule(3);

    // Then only one animation frame is pending
    expect(frames.requestFrame).toHaveBeenCalledTimes(1);
    expect(frames.scheduledCount()).toBe(1);
  });

  it("schedules a new one-shot frame when a value arrives after the previous frame fired", () => {
    // Given a scheduler that already published one value
    const frames = createFrameQueue();
    const publish = vi.fn();
    const scheduler = createTelemetryFrameScheduler<number>(publish, frames);
    scheduler.schedule(1);
    frames.flushNext();

    // When a new value arrives
    scheduler.schedule(2);
    frames.flushNext();

    // Then each value was published on its own frame
    expect(frames.requestFrame).toHaveBeenCalledTimes(2);
    expect(publish).toHaveBeenNthCalledWith(1, 1);
    expect(publish).toHaveBeenNthCalledWith(2, 2);
  });

  it("drops the pending value without publishing when cancelled before the frame fires", () => {
    // Given a scheduled value awaiting its frame
    const frames = createFrameQueue();
    const publish = vi.fn();
    const scheduler = createTelemetryFrameScheduler<number>(publish, frames);
    scheduler.schedule(5);

    // When the scheduler is cancelled before the frame fires
    scheduler.cancel();

    // Then the frame is cancelled and nothing is published
    expect(frames.cancelFrame).toHaveBeenCalledTimes(1);
    expect(frames.scheduledCount()).toBe(0);
    expect(publish).not.toHaveBeenCalled();
  });

  it("does not touch the frame queue when cancelled with nothing scheduled", () => {
    // Given an idle scheduler
    const frames = createFrameQueue();
    const scheduler = createTelemetryFrameScheduler<number>(vi.fn(), frames);

    // When cancel is invoked without a pending frame
    scheduler.cancel();

    // Then no cancellation reaches the frame source
    expect(frames.cancelFrame).not.toHaveBeenCalled();
  });

  it("schedules normally again when a value arrives after a cancellation", () => {
    // Given a scheduler whose pending frame was cancelled
    const frames = createFrameQueue();
    const publish = vi.fn();
    const scheduler = createTelemetryFrameScheduler<number>(publish, frames);
    scheduler.schedule(1);
    scheduler.cancel();

    // When a fresh value arrives
    scheduler.schedule(2);
    frames.flushNext();

    // Then only the fresh value is published
    expect(publish).toHaveBeenCalledTimes(1);
    expect(publish).toHaveBeenCalledWith(2);
  });

  it("publishes falsy values without dropping them when the frame fires", () => {
    // Given a scheduler whose latest value is falsy
    const frames = createFrameQueue();
    const publish = vi.fn();
    const scheduler = createTelemetryFrameScheduler<number>(publish, frames);

    // When the falsy value is scheduled and the frame fires
    scheduler.schedule(0);
    frames.flushNext();

    // Then the falsy value is still delivered
    expect(publish).toHaveBeenCalledTimes(1);
    expect(publish).toHaveBeenCalledWith(0);
  });
});
