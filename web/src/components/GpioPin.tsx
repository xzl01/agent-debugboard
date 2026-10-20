import type { GpioLayoutCell } from "@/lib/gpioLayout";
import { useGpioPinGesture, type GpioAction } from "./useGpioPinGesture";

export type { GpioAction };

export const GPIO_FILL_HIGH = "rgb(var(--c-danger))";
export const GPIO_FILL_LOW = "rgb(var(--c-gpio-low))";
export const GPIO_DIRECTION_STROKE = "rgb(var(--c-ink-dim))";
export const GPIO_LABEL_ON_LEVEL = "rgb(var(--c-gpio-on-level))";
export const GPIO_RING_BRAND = "rgb(var(--c-brand))";
export const GPIO_PENDING_STROKE = "rgb(var(--c-warn))";
export const GPIO_INPUT_DASH = "3 2";

export const GPIO_LEVEL_DISC_RADIUS = 11.5;
export const GPIO_DIRECTION_RING_RADIUS = 14;
export const GPIO_FOCUS_RING_RADIUS = 16;
export const GPIO_HIT_TARGET_RADIUS = 17;

export function pinLabelFontSize(label: string): number {
  if (label.length <= 4) return 9.5;
  if (label.length <= 5) return 8;
  return 7.5;
}

export function pinLabelLines(label: string): string[] {
  if (label.length <= 5) return [label];
  return [label.slice(0, 4), label.slice(4)];
}

export interface GpioPinProps {
  readonly cell: GpioLayoutCell;
  readonly cx: number;
  readonly cy: number;
  readonly pendingPin: number | null;
  readonly instructionsId: string | undefined;
  readonly onAction: ((pin: number, action: GpioAction) => void) | undefined;
  readonly t: (key: string, params?: Record<string, string | number>) => string;
  readonly disabled?: boolean;
  readonly outputCapable?: boolean;
  readonly ariaLabel?: string;
  readonly hitTargetRadius?: number;
  readonly label?: { readonly lines: readonly string[]; readonly fontSize: number };
}

export function GpioPin({ cell, cx, cy, pendingPin, instructionsId, onAction, t, disabled = false, outputCapable = true, ariaLabel, hitTargetRadius = GPIO_HIT_TARGET_RADIUS, label }: GpioPinProps) {
  const isHigh = cell.value > 0;
  const anyPending = pendingPin !== null;
  const blocked = anyPending || disabled;
  const isPendingTarget = pendingPin === cell.pin;
  const labelFontSize = label?.fontSize ?? pinLabelFontSize(cell.layoutLabel);
  const labelLines = label?.lines ?? pinLabelLines(cell.layoutLabel);
  const gesture = useGpioPinGesture({
    blocked,
    onAction: (action) => {
      if (action.kind === "input" || outputCapable) onAction?.(cell.pin, action);
    },
  });

  const stateClass = blocked
    ? isPendingTarget
      ? "opacity-40"
      : ""
    : "hover:opacity-80";

  return (
    <g
      className={`group transition-opacity duration-150 focus:outline-none ${stateClass}`}
      style={{ cursor: blocked ? "not-allowed" : "pointer", touchAction: "manipulation" }}
      role="button"
      tabIndex={0}
      aria-disabled={blocked || undefined}
      aria-busy={isPendingTarget || undefined}
      aria-describedby={instructionsId}
      aria-keyshortcuts={outputCapable ? "Enter Space 0 1 I" : "I"}
      aria-label={ariaLabel ?? t("gpio.pinAria", {
        name: cell.name,
        pin: cell.pin,
        direction: cell.direction === "output" ? t("gpio.output") : t("gpio.input"),
        level: isHigh ? t("gpio.high") : t("gpio.low"),
      })}
      onPointerDown={gesture.onPointerDown}
      onPointerMove={gesture.onPointerMove}
      onPointerUp={gesture.onPointerUp}
      onPointerCancel={gesture.onPointerCancel}
      onLostPointerCapture={gesture.onLostPointerCapture}
      onKeyDown={gesture.onKeyDown}
    >
      <circle
        cx={cx}
        cy={cy}
        r={GPIO_LEVEL_DISC_RADIUS}
        fill={isHigh ? GPIO_FILL_HIGH : GPIO_FILL_LOW}
      />
      <circle
        cx={cx}
        cy={cy}
        r={GPIO_DIRECTION_RING_RADIUS}
        fill="none"
        stroke={GPIO_DIRECTION_STROKE}
        strokeWidth={2.5}
        strokeDasharray={cell.direction === "output" ? undefined : GPIO_INPUT_DASH}
        vectorEffect="non-scaling-stroke"
        className="pointer-events-none"
      />
      {gesture.holding && outputCapable && (
        <circle
          cx={cx}
          cy={cy}
          r={GPIO_DIRECTION_RING_RADIUS}
          fill="none"
          stroke={GPIO_FILL_HIGH}
          strokeWidth={2.5}
          pathLength={1}
          vectorEffect="non-scaling-stroke"
          className="pointer-events-none gpio-hold-arc"
        />
      )}
      <text
        x={cx}
        y={labelLines.length === 1 ? cy + labelFontSize * 0.34 : cy - 1.5}
        textAnchor="middle"
        fontFamily="monospace"
        fontSize={labelFontSize}
        fontWeight={700}
        fill={GPIO_LABEL_ON_LEVEL}
      >
        {labelLines.map((line, index) => (
          <tspan key={line} x={cx} dy={index === 0 ? 0 : labelFontSize * 0.95}>
            {line}
          </tspan>
        ))}
      </text>
      <circle
        cx={cx}
        cy={cy}
        r={hitTargetRadius}
        fill="transparent"
        pointerEvents="all"
      />
      {isPendingTarget && (
        <circle
          cx={cx}
          cy={cy}
          r={GPIO_FOCUS_RING_RADIUS}
          fill="none"
          stroke={GPIO_PENDING_STROKE}
          strokeWidth={1.5}
          vectorEffect="non-scaling-stroke"
          aria-hidden="true"
          className="pointer-events-none"
        />
      )}
      <circle
        cx={cx}
        cy={cy}
        r={GPIO_FOCUS_RING_RADIUS}
        fill="none"
        stroke={GPIO_RING_BRAND}
        strokeWidth={1.5}
        vectorEffect="non-scaling-stroke"
        className="pointer-events-none opacity-0 transition-opacity duration-150 group-focus-visible:opacity-100"
      />
    </g>
  );
}
