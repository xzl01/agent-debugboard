export const POWER_CAPTURE_STOP_ACK_TIMEOUT_MS = 3_000;

export interface PowerCaptureStopHandshake {
  readonly requestId: string;
  readonly promise: Promise<void>;
  readonly acceptsTelemetry: boolean;
  acknowledge(requestId: string): boolean;
  fail(reason: Error): boolean;
}

export function createPowerCaptureStopHandshake(
  requestId: string,
  timeoutMs = POWER_CAPTURE_STOP_ACK_TIMEOUT_MS,
): PowerCaptureStopHandshake {
  let state: "pending" | "acknowledged" | "failed" = "pending";
  let resolvePromise!: () => void;
  let rejectPromise!: (reason: Error) => void;
  const promise = new Promise<void>((resolve, reject) => {
    resolvePromise = resolve;
    rejectPromise = reject;
  });
  const timer = setTimeout(() => {
    if (state !== "pending") return;
    state = "failed";
    rejectPromise(new Error("Timed out waiting for the debugger to acknowledge capture_stop"));
  }, Math.max(1, timeoutMs));

  const settle = (next: "acknowledged" | "failed", reason?: Error) => {
    if (state !== "pending") return false;
    state = next;
    clearTimeout(timer);
    if (reason) rejectPromise(reason);
    else resolvePromise();
    return true;
  };

  return {
    requestId,
    promise,
    get acceptsTelemetry() {
      return state === "pending";
    },
    acknowledge(responseId) {
      if (responseId !== requestId) return false;
      return settle("acknowledged");
    },
    fail(reason) {
      return settle("failed", reason);
    },
  };
}
