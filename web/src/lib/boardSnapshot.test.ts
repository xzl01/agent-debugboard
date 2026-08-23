import assert from "node:assert/strict";
import test from "node:test";
import {
  EMPTY_BOARD_SNAPSHOT,
  mapBoardStatus,
  mergeBoardWsSnapshot,
} from "./boardSnapshot.ts";

test("EMPTY_BOARD_SNAPSHOT carries the documented boot-time defaults", () => {
  // Given the empty snapshot constant
  // When its fields are inspected
  // Then every section defaults to unavailable/empty
  assert.deepEqual(EMPTY_BOARD_SNAPSHOT.powerOutputs, []);
  assert.deepEqual(EMPTY_BOARD_SNAPSHOT.switches, {});
  assert.deepEqual(EMPTY_BOARD_SNAPSHOT.gpios, []);
  assert.deepEqual(EMPTY_BOARD_SNAPSHOT.adc, []);
  assert.deepEqual(EMPTY_BOARD_SNAPSHOT.watchdog, {
    supported: false,
    automatic: false,
    healthy: false,
    armed: false,
    timeout_ms: 0,
    bootloader_on_timeout: false,
    failing_service: "",
  });
  assert.deepEqual(EMPTY_BOARD_SNAPSHOT.monitoring, {
    temperature: { available: false },
    heap: { available: false },
    runtime: { available: false },
    cpu: { available: false },
  });
  assert.equal(EMPTY_BOARD_SNAPSHOT.config, undefined);
});

test("mapBoardStatus treats a non-record status as an empty payload", () => {
  // Given malformed HTTP status payloads
  // When they are mapped with an ADC reading list
  // Then defaults apply and the ADC list is passed through unchanged
  for (const status of [null, 42, "junk", [1, 2]]) {
    const snapshot = mapBoardStatus(status, []);
    assert.deepEqual(snapshot.powerOutputs, []);
    assert.deepEqual(snapshot.gpios, []);
    assert.deepEqual(snapshot.switches, {});
    assert.deepEqual(snapshot.watchdog, EMPTY_BOARD_SNAPSHOT.watchdog);
    assert.deepEqual(snapshot.monitoring, EMPTY_BOARD_SNAPSHOT.monitoring);
    assert.equal(snapshot.config, undefined);
    assert.equal(snapshot.powerCaptureProtocol, undefined);
  }
});

test("mergeBoardWsSnapshot merges monitoring memory without dropping previous detail", () => {
  // Given an HTTP-seeded snapshot with detailed memory monitoring
  const seeded = mapBoardStatus({
    board_monitoring: {
      memory: {
        available: true,
        physical: { total_bytes: 264 },
        stacks: { thread_count: 3 },
        current_pressure: { available: true, coverage: "all" },
        peak_pressure: { available: true, coverage: "all" },
      },
    },
  }, []);

  // When a WebSocket snapshot reports only a partial memory update
  const merged = mergeBoardWsSnapshot(seeded, {
    type: "snapshot",
    board_monitoring: {
      memory: {
        available: true,
        current_pressure: { available: true, reason: "pressure" },
      },
    },
  });

  // Then previous detail survives and the pressure entry is merged field-wise
  const memory = merged.monitoring.memory;
  assert.deepEqual(memory?.physical, { total_bytes: 264 });
  assert.deepEqual(memory?.stacks, { thread_count: 3 });
  assert.deepEqual(memory?.current_pressure, {
    available: true,
    coverage: "all",
    reason: "pressure",
  });
  assert.deepEqual(memory?.peak_pressure, { available: true, coverage: "all" });
  assert.deepEqual(merged.monitoring.heap, { available: false });
});

test("mergeBoardWsSnapshot applies per-field type fallbacks for the watchdog", () => {
  // Given an HTTP-seeded snapshot with a fully reported watchdog
  const seeded = mapBoardStatus({
    watchdog: {
      supported: true,
      automatic: true,
      healthy: true,
      armed: true,
      timeout_ms: 16000,
      bootloader_on_timeout: true,
      failing_service: "svc",
    },
  }, []);

  // When a WebSocket snapshot carries mistyped and partial watchdog fields
  const merged = mergeBoardWsSnapshot(seeded, {
    type: "snapshot",
    watchdog: { timeout_ms: 8000, armed: "yes", healthy: 1 },
  });

  // Then only correctly typed fields update; the rest keep previous values
  assert.deepEqual(merged.watchdog, {
    supported: true,
    automatic: true,
    healthy: true,
    armed: true,
    timeout_ms: 8000,
    bootloader_on_timeout: true,
    failing_service: "svc",
  });
});
