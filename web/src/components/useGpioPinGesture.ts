import { useEffect, useLayoutEffect, useRef, useState, type KeyboardEvent, type PointerEvent } from "react";

export const SHORT_PRESS_WINDOW_MS = 220;
export const LONG_PRESS_THRESHOLD_MS = 600;
export const POINTER_MOVE_TOLERANCE_PX = 8;

export type GpioAction =
  | { readonly kind: "input" }
  | { readonly kind: "outputLow" }
  | { readonly kind: "outputHigh" };

type GesturePhase =
  | { readonly kind: "idle" }
  | {
      readonly kind: "down";
      readonly pointerId: number;
      readonly startX: number;
      readonly startY: number;
      readonly startTime: number;
      readonly second: boolean;
    }
  | { readonly kind: "awaitSecond" }
  | { readonly kind: "holdFired"; readonly pointerId: number };

export interface GpioPinGestureHandlers {
  readonly holding: boolean;
  readonly onPointerDown: (event: PointerEvent<SVGGElement>) => void;
  readonly onPointerMove: (event: PointerEvent<SVGGElement>) => void;
  readonly onPointerUp: (event: PointerEvent<SVGGElement>) => void;
  readonly onPointerCancel: (event: PointerEvent<SVGGElement>) => void;
  readonly onLostPointerCapture: (event: PointerEvent<SVGGElement>) => void;
  readonly onKeyDown: (event: KeyboardEvent<SVGGElement>) => void;
}

function keyAction(key: string): GpioAction | null {
  switch (key) {
    case "Enter":
    case " ":
    case "0":
      return { kind: "outputLow" };
    case "1":
      return { kind: "outputHigh" };
    case "i":
    case "I":
      return { kind: "input" };
    default:
      return null;
  }
}

export function useGpioPinGesture({
  blocked,
  onAction,
}: {
  readonly blocked: boolean;
  readonly onAction: (action: GpioAction) => void;
}): GpioPinGestureHandlers {
  const phaseRef = useRef<GesturePhase>({ kind: "idle" });
  const timerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const blockedRef = useRef(blocked);
  const onActionRef = useRef(onAction);
  const [holding, setHolding] = useState(false);

  const clearTimer = () => {
    if (timerRef.current !== null) {
      clearTimeout(timerRef.current);
      timerRef.current = null;
    }
  };

  const cancelGesture = () => {
    clearTimer();
    phaseRef.current = { kind: "idle" };
    setHolding(false);
  };

  const fireHold = (pointerId: number) => {
    phaseRef.current = { kind: "holdFired", pointerId };
    setHolding(false);
    if (blockedRef.current) return;
    onActionRef.current({ kind: "outputHigh" });
  };

  useLayoutEffect(() => {
    blockedRef.current = blocked;
    onActionRef.current = onAction;
  }, [blocked, onAction]);

  // A pending transition cancels any in-progress gesture without writing.
  useEffect(() => {
    if (blocked) cancelGesture();
  }, [blocked]);

  useEffect(() => clearTimer, []);

  const handlePointerDown = (event: PointerEvent<SVGGElement>) => {
    if (event.button !== 0 || !event.isPrimary) return;
    if (blockedRef.current) return;
    const phase = phaseRef.current;
    if (phase.kind === "down" || phase.kind === "holdFired") return;
    event.currentTarget.setPointerCapture(event.pointerId);
    const second = phase.kind === "awaitSecond";
    clearTimer();
    phaseRef.current = {
      kind: "down",
      pointerId: event.pointerId,
      startX: event.clientX,
      startY: event.clientY,
      startTime: performance.now(),
      second,
    };
    setHolding(true);
    timerRef.current = setTimeout(() => {
      timerRef.current = null;
      const current = phaseRef.current;
      if (current.kind !== "down") return;
      fireHold(current.pointerId);
    }, LONG_PRESS_THRESHOLD_MS);
  };

  const handlePointerMove = (event: PointerEvent<SVGGElement>) => {
    const phase = phaseRef.current;
    if (phase.kind !== "down" || event.pointerId !== phase.pointerId) return;
    const moved = Math.hypot(event.clientX - phase.startX, event.clientY - phase.startY);
    if (moved > POINTER_MOVE_TOLERANCE_PX) cancelGesture();
  };

  const handlePointerUp = (event: PointerEvent<SVGGElement>) => {
    const phase = phaseRef.current;
    if (phase.kind === "holdFired" && event.pointerId === phase.pointerId) {
      phaseRef.current = { kind: "idle" };
      return;
    }
    if (phase.kind !== "down" || event.pointerId !== phase.pointerId) return;
    clearTimer();
    if (performance.now() - phase.startTime >= LONG_PRESS_THRESHOLD_MS) {
      fireHold(phase.pointerId);
      phaseRef.current = { kind: "idle" };
      return;
    }
    setHolding(false);
    if (phase.second) {
      phaseRef.current = { kind: "idle" };
      if (blockedRef.current) return;
      onActionRef.current({ kind: "input" });
      return;
    }
    phaseRef.current = { kind: "awaitSecond" };
    timerRef.current = setTimeout(() => {
      timerRef.current = null;
      phaseRef.current = { kind: "idle" };
      if (blockedRef.current) return;
      onActionRef.current({ kind: "outputLow" });
    }, SHORT_PRESS_WINDOW_MS);
  };

  const cancelTrackedPointer = (event: PointerEvent<SVGGElement>) => {
    const phase = phaseRef.current;
    if (
      (phase.kind === "down" || phase.kind === "holdFired") &&
      event.pointerId === phase.pointerId
    ) {
      cancelGesture();
    }
  };

  const handleKeyDown = (event: KeyboardEvent<SVGGElement>) => {
    if (event.key === "Escape") {
      if (phaseRef.current.kind === "idle") return;
      event.preventDefault();
      cancelGesture();
      return;
    }
    const action = keyAction(event.key);
    if (action === null) return;
    event.preventDefault();
    if (event.repeat) return;
    cancelGesture();
    if (blockedRef.current) return;
    onActionRef.current(action);
  };

  return {
    holding,
    onPointerDown: handlePointerDown,
    onPointerMove: handlePointerMove,
    onPointerUp: handlePointerUp,
    onPointerCancel: cancelTrackedPointer,
    onLostPointerCapture: cancelTrackedPointer,
    onKeyDown: handleKeyDown,
  };
}
