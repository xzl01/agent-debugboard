import { act } from "react";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import {
  armStreamingCapture,
  emitCaptureTriggered,
  powerCaptureArchiveInfo,
  stopAndAcknowledgeCapture,
} from "./useBoard.fixtures";
import {
  flushMicrotasks,
  powerCaptureStoreMocks,
  renderLiveBoard,
  setupUseBoardHarness,
  teardownUseBoardHarness,
  telemetryBatchMessage,
} from "./useBoard.testUtils";

function sequenceRange(start: number, count: number): number[] {
  return Array.from({ length: count }, (_, index) => start + index);
}

describe("useBoard streaming capture persistence", () => {
  beforeEach(setupUseBoardHarness);
  afterEach(teardownUseBoardHarness);

  it("ignores samples whose sequence was already stored", async () => {
    // Given a recording capture triggered at sequence 5
    const finishMock = powerCaptureStoreMocks.finishPowerCaptureArchive;
    finishMock.mockImplementation(
      async (capture) => powerCaptureArchiveInfo(capture),
    );
    const { view, socket } = await renderLiveBoard();
    await armStreamingCapture(view, socket);
    emitCaptureTriggered(socket, { sample_sequence: 5, device_t_mono_us: 10000 });

    // When a batch repeats an already stored sequence
    act(() => {
      socket.emitMessage(telemetryBatchMessage([5, 6, 6, 7]));
    });
    await stopAndAcknowledgeCapture(view, socket);

    // Then only the new sequences are recorded
    const record = finishMock.mock.calls[0]?.[0];
    expect(record?.sampleCount).toBe(3);
    expect(record?.samples.map((sample) => sample.sampleSequence)).toEqual([5, 6, 7]);
    expect(record?.preSamples).toBe(0);
    expect(record?.postSamples).toBe(2);
    expect(record?.samples[0]?.triggered).toBe(true);
    view.close();
  });

  it("writes one archive chunk per 200 stored samples", async () => {
    // Given a recording capture
    const chunkMock = powerCaptureStoreMocks.appendPowerCaptureChunk;
    const { view, socket } = await renderLiveBoard();
    await armStreamingCapture(view, socket);
    emitCaptureTriggered(socket);

    // When the first 200 samples arrive
    act(() => {
      socket.emitMessage(telemetryBatchMessage(sequenceRange(1, 200)));
    });
    await flushMicrotasks();

    // Then exactly one 200-sample chunk was queued
    expect(chunkMock).toHaveBeenCalledTimes(1);
    expect(chunkMock.mock.calls[0]?.[1]).toBe(0);
    expect(chunkMock.mock.calls[0]?.[2]).toHaveLength(200);
    expect(chunkMock.mock.calls[0]?.[2]?.[0]?.sampleSequence).toBe(1);

    // When another 200 samples arrive
    act(() => {
      socket.emitMessage(telemetryBatchMessage(sequenceRange(201, 200)));
    });
    await flushMicrotasks();

    // Then a second chunk was queued with the next index
    expect(chunkMock).toHaveBeenCalledTimes(2);
    expect(chunkMock.mock.calls[1]?.[1]).toBe(1);
    view.close();
  });

  it("stops with the storage backpressure error after eight queued chunks", async () => {
    // Given archive chunk writes that never settle
    powerCaptureStoreMocks.appendPowerCaptureChunk.mockReturnValue(
      new Promise(() => {}),
    );
    const { view, socket } = await renderLiveBoard();
    await armStreamingCapture(view, socket);
    emitCaptureTriggered(socket);

    // When enough samples arrive to exceed the eight-chunk queue bound
    act(() => {
      socket.emitMessage(telemetryBatchMessage(sequenceRange(1, 1800)));
    });
    await flushMicrotasks();

    // Then the capture stops with the documented backpressure error
    expect(view.current().error).toBe(
      "Host storage is not keeping up with the capture stream; " +
      "recording was stopped before browser memory could grow without limit",
    );
    expect(view.current().captureState).toBe("receiving");
    // Archive writes are serialized through the write chain, so only the head
    // chunk is in flight while the other seven stay queued behind it.
    expect(powerCaptureStoreMocks.appendPowerCaptureChunk).toHaveBeenCalledTimes(1);
    expect(powerCaptureStoreMocks.appendPowerCaptureChunk.mock.calls[0]?.[1]).toBe(0);
    view.close();
  });

  it("marks a completed capture incomplete when the debugger dropped samples", async () => {
    // Given a recording capture
    const { view, socket } = await renderLiveBoard();
    await armStreamingCapture(view, socket);
    emitCaptureTriggered(socket);
    powerCaptureStoreMocks.finishPowerCaptureArchive.mockImplementation(
      async (capture) => powerCaptureArchiveInfo(capture),
    );

    // When a batch reports dropped samples
    act(() => {
      socket.emitMessage({ ...telemetryBatchMessage([1, 2]), dropped_samples: 3 });
    });
    await stopAndAcknowledgeCapture(view, socket);

    // Then the stored capture is incomplete with the documented reason
    const capture = view.current().captures.at(-1);
    expect(capture?.incomplete).toBe(true);
    expect(capture?.interruptionReason).toBe("The debugger reported 3 dropped samples");
    expect(view.current().error).toBeNull();
    view.close();
  });
});
