/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

import {
  FILL_DISABLED,
  FILL_SELECTED,
  FILL_TRIGGER,
  FILL_UNSELECTED,
  LABEL_DEFAULT,
  LABEL_DIM,
  LABEL_SELECTED,
  LABEL_TRIGGER,
  STROKE_DEFAULT,
  STROKE_DISABLED,
  STROKE_SELECTED,
  STROKE_TRIGGER,
} from "./GpioPinoutGroup";

export type LogicPinState = "unselected" | "selected" | "trigger" | "disabled";

/** Shared selected/trigger/disabled classification for physical circles and chips. */
export function logicPinState(
  pin: number,
  options: {
    readonly selectedPins: readonly number[];
    readonly disabledPins: readonly number[];
    readonly triggerPin: number | null;
    readonly triggerActive: boolean;
  }
): LogicPinState {
  if (options.disabledPins.includes(pin)) return "disabled";
  if (options.triggerActive && options.triggerPin === pin) return "trigger";
  if (options.selectedPins.includes(pin)) return "selected";
  return "unselected";
}

export interface GpioSelectionPinProps {
  readonly pin: number;
  readonly cx: number;
  readonly cy: number;
  readonly label: { readonly lines: readonly string[]; readonly fontSize: number };
  readonly state: LogicPinState;
  readonly ariaLabel: string;
  readonly instructionsId?: string;
  /** UI lock while a capture or stream runs; the pin keeps its state visuals. */
  readonly locked?: boolean;
  readonly onTogglePin: (pin: number) => void;
  readonly onSetTriggerPin: (pin: number | null) => void;
}

/**
 * Logic-analyzer selection control drawn on the physical pinout. Pointer and
 * keyboard semantics match the legacy pinout: left click (Enter / Space) sets
 * or clears the trigger pin, right click (C) toggles capture selection.
 * Non-capture contacts never receive this control.
 */
export function GpioSelectionPin({
  pin,
  cx,
  cy,
  label,
  state,
  ariaLabel,
  instructionsId,
  locked = false,
  onTogglePin,
  onSetTriggerPin,
}: GpioSelectionPinProps) {
  const unavailable = state === "disabled";
  const isTrigger = state === "trigger";
  const isSelected = state === "selected" || isTrigger;
  const interactive = !unavailable && !locked;
  const fill = unavailable
    ? FILL_DISABLED
    : isTrigger
      ? FILL_TRIGGER
      : isSelected
        ? FILL_SELECTED
        : FILL_UNSELECTED;
  const stroke = unavailable
    ? STROKE_DISABLED
    : isTrigger
      ? STROKE_TRIGGER
      : isSelected
        ? STROKE_SELECTED
        : STROKE_DEFAULT;
  const strokeWidth = unavailable ? 1 : isSelected ? 1.5 : 1;
  const labelFill = unavailable
    ? LABEL_DIM
    : isTrigger
      ? LABEL_TRIGGER
      : isSelected
        ? LABEL_SELECTED
        : LABEL_DEFAULT;

  const toggle = () => {
    if (interactive) onTogglePin(pin);
  };
  const setTrigger = () => {
    if (interactive) onSetTriggerPin(isTrigger ? null : pin);
  };

  return (
    <g
      data-logic-pin={pin}
      data-logic-pin-state={state}
      role="button"
      tabIndex={0}
      aria-label={ariaLabel}
      aria-pressed={isSelected}
      aria-disabled={interactive ? undefined : true}
      aria-keyshortcuts="Enter Space C"
      aria-describedby={instructionsId}
      className={`group transition-opacity duration-150 focus:outline-none ${interactive ? "hover:opacity-80" : ""}`}
      style={{ cursor: interactive ? "pointer" : "not-allowed", opacity: unavailable ? 0.65 : locked ? 0.5 : 1, touchAction: "manipulation" }}
      onClick={setTrigger}
      onContextMenu={(event) => {
        event.preventDefault();
        toggle();
      }}
      onKeyDown={(event) => {
        // Modifier combos never act on the pin: browser shortcuts such as
        // Ctrl/Cmd+C stay untouched and modified Enter/Space never trigger.
        if (event.ctrlKey || event.metaKey || event.altKey) return;
        if (event.key === "Enter" || event.key === " ") {
          event.preventDefault();
          setTrigger();
          return;
        }
        if (event.key === "c" || event.key === "C") {
          event.preventDefault();
          toggle();
        }
      }}
    >
      <circle cx={cx} cy={cy} r={11.5} fill={fill} />
      <circle
        cx={cx}
        cy={cy}
        r={14}
        fill="none"
        stroke={stroke}
        strokeWidth={strokeWidth}
        vectorEffect="non-scaling-stroke"
        className="pointer-events-none"
      />
      <text
        x={cx}
        y={label.lines.length === 1 ? cy + label.fontSize * 0.34 : cy - 1.5}
        textAnchor="middle"
        fontFamily="monospace"
        fontSize={label.fontSize}
        fontWeight={700}
        fill={labelFill}
        className="pointer-events-none"
      >
        {label.lines.map((line, index) => (
          <tspan key={line} x={cx} dy={index === 0 ? 0 : label.fontSize * 0.95}>
            {line}
          </tspan>
        ))}
      </text>
      <circle cx={cx} cy={cy} r={22} fill="transparent" pointerEvents="all" />
      <circle
        cx={cx}
        cy={cy}
        r={16}
        fill="none"
        stroke="rgb(var(--c-brand))"
        strokeWidth={1.5}
        vectorEffect="non-scaling-stroke"
        className="pointer-events-none opacity-0 transition-opacity duration-150 group-focus-visible:opacity-100"
      />
    </g>
  );
}
