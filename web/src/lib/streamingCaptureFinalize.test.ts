import assert from "node:assert/strict";
import test from "node:test";
import { IDBKeyRange, indexedDB } from "fake-indexeddb";

import { defaultLiveSubscribeMessage } from "./liveSubscribe.ts";
import {
  beginPowerCaptureArchive,
  listPowerCaptureArchives,
  readPowerCaptureSamples,
} from "./powerCaptureStore.ts";
import {
  discardStreamingCaptureArchive,
  finalizeStreamingCaptureArchive,
  type StreamingCaptureFinalizeHost,
  type StreamingFinalizeSocket,
} from "./streamingCaptureFinalize.ts";
import {
  createStreamingCaptureSession,
  streamingCaptureLease,
  streamingCaptureRecord,
  type StreamingCaptureSession,
} from "./streamingCaptureModel.ts";
import { queueStreamingSamples } from "./streamingCapturePersistence.ts";
import type {
  BoardCaptureProgress,
  BoardCaptureState,
  CaptureConfig,
  CaptureSample,
  PowerCapture,
} from "./types.ts";

Object.defineProperty(globalThis, "indexedDB", {
  configurable: true,
  value: indexedDB,
});
Object.defineProperty(globalThis, "IDBKeyRange", {
  configurable: true,
  value: IDBKeyRange,
});

const CONFIG: CaptureConfig = {
  trigger: "manual",
  source: "5v_out",
  edge: "either",
  thresholdUa: 0,
  rateHz: 100,
  preSamples: 1,
  postSamples: 2,
  streaming: true,
};

function sample(sequence: number, deviceTimeUs = sequence * 10_000): CaptureSample {
  return {
    offset: 0,
    triggered: false,
    sampleSequence: sequence,
    deviceTimeUs,
    readings: [{
      kind: "current",
      unit: "uA",
      name: "5v_out",
      signal: "5v_out-signal",
      value: 1000,
      power_enabled: true,
      current_ua: 1000,
      raw: 123,
      mv: 12,
      sensor_channel: "current",
    }],
  };
}

function samples(from: number, count: number): CaptureSample[] {
  return Array.from({ length: count }, (_, index) => sample(from + index));
}

class FakeSocket implements StreamingFinalizeSocket {
  readonly sent: string[] = [];
  readyState: number = WebSocket.OPEN;
  send(data: string): void {
    this.sent.push(data);
  }
}

interface FinalizeProbe {
  readonly sessionRef: { current: StreamingCaptureSession | null };
  readonly socketRef: { current: FakeSocket | null };
  readonly states: BoardCaptureState[];
  readonly captures: PowerCapture[];
  readonly errors: string[];
  readonly progressLog: (BoardCaptureProgress | null)[];
}

function createProbe(
  session: StreamingCaptureSession | null,
  socket: FakeSocket | null,
): FinalizeProbe {
  return {
    sessionRef: { current: session },
    socketRef: { current: socket },
    states: [],
    captures: [],
    errors: [],
    progressLog: [],
  };
}

function wireProbe(probe: FinalizeProbe): StreamingCaptureFinalizeHost {
  return {
    streamingCaptureRef: probe.sessionRef,
    socketRef: probe.socketRef,
    onState: (state) => probe.states.push(state),
    onProgress: (progress) => probe.progressLog.push(progress),
    onCapture: (capture) => probe.captures.push(capture),
    onError: (message) => probe.errors.push(message),
  };
}

async function startedSession(): Promise<StreamingCaptureSession> {
  const session = createStreamingCaptureSession(CONFIG, "owner-1");
  session.captureId = 7;
  session.capturedAt = 1_700_000_000_000;
  session.triggered = true;
  session.archiveStarted = true;
  await beginPowerCaptureArchive(streamingCaptureRecord(session), streamingCaptureLease(session));
  session.writeChain = Promise.resolve();
  return session;
}

test("finalizeStreamingCaptureArchive idles when no triggered session exists", async () => {
  // Given no session at all
  const empty = createProbe(null, new FakeSocket());

  // When finalization is requested
  await finalizeStreamingCaptureArchive(wireProbe(empty));

  // Then nothing happened
  assert.deepEqual(empty.states, []);
  assert.deepEqual(empty.socketRef.current?.sent, []);

  // And given an armed but untriggered session
  const untriggered = createProbe(createStreamingCaptureSession(CONFIG, "owner-1"), new FakeSocket());

  // When finalization is requested
  await finalizeStreamingCaptureArchive(wireProbe(untriggered), true, "Live WebSocket error");

  // Then the session is left untouched for the arming path to clean up
  assert.deepEqual(untriggered.states, []);
  assert.deepEqual(untriggered.socketRef.current?.sent, []);
  assert.equal(untriggered.sessionRef.current?.triggered, false);
});

test("finalizeStreamingCaptureArchive sends capture_stop and seals the archive after the ACK", async () => {
  // Given a triggered session with a pending tail of five samples
  const session = await startedSession();
  queueStreamingSamples(session, samples(1, 5), () => undefined);
  const socket = new FakeSocket();
  const probe = createProbe(session, socket);

  // When finalization starts
  const finalizing = finalizeStreamingCaptureArchive(wireProbe(probe));

  // Then the stop handshake is sent synchronously with the capture-scoped id
  assert.deepEqual(probe.states, ["receiving"]);
  assert.equal(socket.sent.length, 1);
  assert.deepEqual(JSON.parse(socket.sent[0] ?? ""), {
    type: "command",
    command: "capture_stop",
    id: "web-stop-7",
  });

  // And a concurrent finalization reuses the in-flight promise
  assert.equal(finalizeStreamingCaptureArchive(wireProbe(probe)), finalizing);

  // When the firmware acknowledges the stop
  assert.equal(session.stopHandshake?.acknowledge("web-stop-7"), true);
  await finalizing;

  // Then the pending tail was flushed and the archive completed
  assert.equal(probe.captures.length, 1);
  const archived = probe.captures[0];
  assert.equal(archived?.archiveId, session.archiveId);
  assert.equal(archived?.sampleCount, 5);
  assert.equal(archived?.incomplete, false);
  assert.equal(archived?.interruptionReason, undefined);
  const persisted = await readPowerCaptureSamples(session.archiveId);
  assert.equal(persisted.length, 5);

  // And state, progress, ref, and the live subscription are restored in order
  assert.deepEqual(probe.states, ["receiving", "idle"]);
  assert.deepEqual(probe.progressLog, [null]);
  assert.equal(probe.sessionRef.current, null);
  assert.deepEqual(probe.errors, []);
  assert.equal(socket.sent.length, 2);
  assert.deepEqual(JSON.parse(socket.sent[1] ?? ""), defaultLiveSubscribeMessage());
});

test("finalizeStreamingCaptureArchive marks the capture incomplete when the socket is gone", async () => {
  // Given a triggered session whose socket is already closed
  const session = await startedSession();
  queueStreamingSamples(session, samples(1, 3), () => undefined);
  const socket = new FakeSocket();
  socket.readyState = WebSocket.CLOSED;
  const probe = createProbe(session, socket);

  // When finalization runs
  await finalizeStreamingCaptureArchive(wireProbe(probe));

  // Then no capture_stop was sent and the archive sealed as interrupted
  assert.deepEqual(socket.sent, []);
  assert.equal(probe.captures.length, 1);
  assert.equal(probe.captures[0]?.incomplete, true);
  assert.equal(
    probe.captures[0]?.interruptionReason,
    "Live WebSocket disconnected before capture_stop was acknowledged",
  );
  assert.deepEqual(probe.errors, [
    "Live WebSocket disconnected before capture_stop was acknowledged",
  ]);
  assert.deepEqual(probe.states, ["receiving", "idle"]);
  assert.equal(probe.sessionRef.current, null);
});

test("finalizeStreamingCaptureArchive records a stop handshake failure as the interruption reason", async () => {
  // Given a triggered session with an open socket
  const session = await startedSession();
  queueStreamingSamples(session, samples(1, 4), () => undefined);
  const socket = new FakeSocket();
  const probe = createProbe(session, socket);

  // When finalization starts but the firmware reports a stop failure
  const finalizing = finalizeStreamingCaptureArchive(wireProbe(probe));
  assert.equal(socket.sent.length, 1);
  assert.equal(session.stopHandshake?.fail(new Error("firmware rejected capture_stop")), true);
  await finalizing;

  // Then the archive completed as incomplete with the handshake message
  assert.equal(probe.captures.length, 1);
  assert.equal(probe.captures[0]?.incomplete, true);
  assert.equal(probe.captures[0]?.interruptionReason, "firmware rejected capture_stop");
  assert.deepEqual(probe.errors, ["firmware rejected capture_stop"]);

  // And the default live subscription is still restored
  assert.equal(socket.sent.length, 2);
  assert.deepEqual(JSON.parse(socket.sent[1] ?? ""), defaultLiveSubscribeMessage());
});

test("finalizeStreamingCaptureArchive truncates the preview to the persisted tail on write failure", async () => {
  // Given a session that persisted one 200-sample chunk before storage failed
  const session = await startedSession();
  queueStreamingSamples(session, samples(1, 200), () => undefined);
  await session.writeChain;
  assert.equal(session.persistedSamples, 200);

  // And a later write failure left 150 ingested samples stuck in the tail
  queueStreamingSamples(session, samples(201, 150), () => undefined);
  const storageError = new Error("IndexedDB quota exceeded");
  session.writeError = storageError;
  session.writeChain = Promise.reject(storageError);
  session.writeChain.catch(() => undefined);
  const probe = createProbe(session, new FakeSocket());

  // When finalization runs
  const finalizing = finalizeStreamingCaptureArchive(wireProbe(probe));
  session.stopHandshake?.acknowledge("web-stop-7");
  await finalizing;

  // Then the archived capture is truncated to the persisted sequences
  assert.equal(probe.captures.length, 1);
  const archived = probe.captures[0];
  assert.equal(archived?.incomplete, true);
  assert.equal(archived?.sampleCount, 200);
  assert.equal(archived?.samples.length, 200);
  assert.equal(archived?.interruptionReason, "IndexedDB quota exceeded");
  assert.deepEqual(probe.errors, ["IndexedDB quota exceeded"]);
  assert.deepEqual(probe.states, ["receiving", "idle"]);
  assert.equal(probe.sessionRef.current, null);
});

test("discardStreamingCaptureArchive abandons the session and deletes a started archive", async () => {
  // Given no session at all
  const idleSocket = new FakeSocket();
  discardStreamingCaptureArchive({
    streamingCaptureRef: { current: null },
    socketRef: { current: idleSocket },
  });

  // Then nothing was sent or torn down
  assert.deepEqual(idleSocket.sent, []);

  // And given a triggered session with a started archive and an open socket
  const session = await startedSession();
  queueStreamingSamples(session, samples(1, 5), () => undefined);
  await session.writeChain;
  const socket = new FakeSocket();
  const sessionRef: { current: StreamingCaptureSession | null } = { current: session };

  // When the session is discarded
  discardStreamingCaptureArchive({
    streamingCaptureRef: sessionRef,
    socketRef: { current: socket },
  });

  // Then the session is dropped immediately and the live subscription restored
  assert.equal(sessionRef.current, null);
  assert.equal(session.finishing, true);
  assert.equal(socket.sent.length, 1);
  assert.deepEqual(JSON.parse(socket.sent[0] ?? ""), defaultLiveSubscribeMessage());

  // And the archive is deleted once the queued writes settle
  await session.writeChain;
  await new Promise((resolve) => setTimeout(resolve, 0));
  const archives = await listPowerCaptureArchives();
  assert.equal(archives.some((archive) => archive.archiveId === session.archiveId), false);
});
