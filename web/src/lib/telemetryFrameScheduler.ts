export interface TelemetryFrameSchedulerOptions {
  requestFrame?: (callback: () => void) => number;
  cancelFrame?: (handle: number) => void;
}

export interface TelemetryFrameScheduler<T> {
  schedule: (value: T) => void;
  cancel: () => void;
}

export function createTelemetryFrameScheduler<T>(
  publish: (value: T) => void,
  options: TelemetryFrameSchedulerOptions = {},
): TelemetryFrameScheduler<T> {
  const requestFrame = options.requestFrame ?? ((callback: () => void) => requestAnimationFrame(callback));
  const cancelFrame = options.cancelFrame ?? ((handle: number) => cancelAnimationFrame(handle));
  let frameHandle: number | null = null;
  let latest: { readonly value: T } | null = null;

  const flush = (): void => {
    frameHandle = null;
    const current = latest;
    latest = null;
    if (current !== null) {
      publish(current.value);
    }
  };

  return {
    schedule(value: T): void {
      latest = { value };
      if (frameHandle !== null) return;
      frameHandle = requestFrame(flush);
    },
    cancel(): void {
      latest = null;
      if (frameHandle === null) return;
      cancelFrame(frameHandle);
      frameHandle = null;
    },
  };
}
