import { defaultLiveSubscribeMessage } from "./liveSubscribe.ts";
import { createPowerCaptureStopHandshake } from "./powerCaptureStop.ts";
import {
  deletePowerCaptureArchive,
  finishPowerCaptureArchive,
  interruptPowerCaptureArchive,
} from "./powerCaptureStore.ts";
import {
  streamingCaptureRecord,
  type StreamingCaptureSession,
} from "./streamingCaptureModel.ts";
import { flushStreamingCaptureChunk } from "./streamingCapturePersistence.ts";
import type {
  BoardCaptureProgress,
  BoardCaptureState,
  PowerCapture,
} from "./types.ts";

/** Minimal mutable reference shape, structurally compatible with React refs. */
export interface StreamingCaptureRefBox<T> {
  current: T;
}

/** Structural socket surface so offline tests can substitute a fake transport. */
export interface StreamingFinalizeSocket {
  readonly readyState: number;
  send(data: string): void;
}

export interface StreamingCaptureFinalizeHost {
  readonly streamingCaptureRef: StreamingCaptureRefBox<StreamingCaptureSession | null>;
  readonly socketRef: StreamingCaptureRefBox<StreamingFinalizeSocket | null>;
  readonly onState: (state: BoardCaptureState) => void;
  readonly onProgress: (progress: BoardCaptureProgress | null) => void;
  readonly onCapture: (capture: PowerCapture) => void;
  readonly onError: (message: string) => void;
}

export interface StreamingCaptureDiscardHost {
  readonly streamingCaptureRef: StreamingCaptureRefBox<StreamingCaptureSession | null>;
  readonly socketRef: StreamingCaptureRefBox<StreamingFinalizeSocket | null>;
}

export function discardStreamingCaptureArchive(host: StreamingCaptureDiscardHost): void {
  const builder = host.streamingCaptureRef.current;
  if (!builder) return;
  builder.finishing = true;
  builder.stopHandshake?.fail(new Error("Power capture was discarded"));
  if (builder.stopTimer) clearTimeout(builder.stopTimer);
  builder.stopTimer = null;
  host.streamingCaptureRef.current = null;
  if (builder.archiveStarted) {
    void builder.writeChain
      .then(() => deletePowerCaptureArchive(builder.archiveId))
      .catch(() => undefined);
  }
  if (host.socketRef.current?.readyState === WebSocket.OPEN) {
    host.socketRef.current.send(JSON.stringify(defaultLiveSubscribeMessage()));
  }
}

export function finalizeStreamingCaptureArchive(
  host: StreamingCaptureFinalizeHost,
  incomplete = false,
  interruptionReason?: string,
): Promise<void> {
  const builder = host.streamingCaptureRef.current;
  if (!builder || !builder.triggered) return Promise.resolve();
  if (incomplete) {
    builder.requestedIncomplete = true;
    builder.requestedInterruptionReason ??= interruptionReason;
    builder.stopHandshake?.fail(new Error(
      interruptionReason ?? "Power capture was interrupted while stopping",
    ));
  }
  if (builder.finalizePromise) return builder.finalizePromise;

  builder.finalizePromise = (async () => {
    if (builder.stopTimer) clearTimeout(builder.stopTimer);
    builder.stopTimer = null;
    host.onState("receiving");

    const socket = host.socketRef.current;
    if (socket?.readyState === WebSocket.OPEN) {
      const stopHandshake = createPowerCaptureStopHandshake(`web-stop-${builder.captureId}`);
      builder.stopHandshake = stopHandshake;
      try {
        socket.send(JSON.stringify({
          type: "command",
          command: "capture_stop",
          id: stopHandshake.requestId,
        }));
      } catch (reason) {
        stopHandshake.fail(reason instanceof Error ? reason : new Error(String(reason)));
      }
      try {
        await stopHandshake.promise;
      } catch (reason) {
        const message = reason instanceof Error ? reason.message : String(reason);
        builder.requestedIncomplete = true;
        builder.requestedInterruptionReason ??= message;
      }
    } else {
      builder.requestedIncomplete = true;
      builder.requestedInterruptionReason ??=
        "Live WebSocket disconnected before capture_stop was acknowledged";
    }

    // The ACK closes the capture boundary. Telemetry stays enabled while
    // capture_stop is in flight, then the final tail is flushed before the
    // archive metadata is sealed.
    builder.finishing = true;
    flushStreamingCaptureChunk(builder);

    try {
      await builder.writeChain;
      if (builder.writeError) throw builder.writeError;
      const completed = streamingCaptureRecord(
        builder,
        builder.requestedIncomplete,
        builder.requestedInterruptionReason,
      );
      const archived = await finishPowerCaptureArchive(completed, builder.chunkIndex);
      host.onCapture(archived.capture);
      if (builder.requestedInterruptionReason) {
        host.onError(builder.requestedInterruptionReason);
      }
    } catch (reason) {
      const message = reason instanceof Error ? reason.message : String(reason);
      const partial = streamingCaptureRecord(
        builder,
        true,
        builder.requestedInterruptionReason ?? message,
      );
      partial.sampleCount = builder.persistedSamples;
      partial.samples = partial.samples.filter(
        (sample) => sample.sampleSequence <= builder.lastPersistedSequence,
      );
      try {
        const archived = await interruptPowerCaptureArchive(
          builder.archiveId,
          partial,
          builder.requestedInterruptionReason ?? message,
          builder.writeError != null,
        );
        host.onCapture(archived.capture);
      } catch {
        // Opening the archive itself may have failed. The original storage
        // error is more useful than a secondary metadata update failure.
      }
      host.onError(message);
    } finally {
      if (host.streamingCaptureRef.current === builder) host.streamingCaptureRef.current = null;
      host.onProgress(null);
      host.onState("idle");
      if (host.socketRef.current?.readyState === WebSocket.OPEN) {
        host.socketRef.current.send(JSON.stringify(defaultLiveSubscribeMessage()));
      }
    }
  })();
  return builder.finalizePromise;
}
