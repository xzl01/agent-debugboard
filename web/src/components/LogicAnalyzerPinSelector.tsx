/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

import { useId } from "react";
import { MousePointerClick } from "lucide-react";
import { GpioPhysicalPinoutSvg, physicalControlGpioName } from "./GpioPhysicalPinoutSvg";
import { logicPinState, type LogicPinState } from "./GpioSelectionPin";
import {
  FILL_DISABLED,
  FILL_SELECTED,
  FILL_TRIGGER,
  FILL_UNSELECTED,
  STROKE_DISABLED,
  STROKE_DEFAULT,
  STROKE_TRIGGER,
} from "./GpioPinoutGroup";
import { LOGIC_ANALYZER_WEB_CAPTURE_PINS, LOGIC_ANALYZER_SIGROK_DISABLED_PINS } from "@/lib/logicAnalyzer";
import type { GpioPhysicalLayout, SafeGpio } from "@/lib/types";
import { useI18n } from "@/lib/i18n";

/** Only GP12-GP16 are selectable in the Web UI. */
const CAPTURE_PINS = LOGIC_ANALYZER_WEB_CAPTURE_PINS;
/** Pins that render disabled capture controls when they appear on a connector. */
const UNAVAILABLE_PINS: readonly number[] = LOGIC_ANALYZER_SIGROK_DISABLED_PINS;
const CONTROL_PINS = new Set<number>([...CAPTURE_PINS, ...UNAVAILABLE_PINS]);

function logicPinStateLabel(state: LogicPinState, t: (key: string) => string): string {
  return t(`logicAnalyzer.pinout.legend.${state}`);
}

function SelectionChip({
  pin,
  state,
  disabled,
  instructionsId,
  onTogglePin,
  onSetTriggerPin,
  t,
}: {
  readonly pin: number;
  readonly state: LogicPinState;
  readonly disabled: boolean;
  readonly instructionsId: string;
  readonly onTogglePin: (pin: number) => void;
  readonly onSetTriggerPin: (pin: number | null) => void;
  readonly t: (key: string, params?: Record<string, string | number>) => string;
}) {
  const unavailable = state === "disabled";
  const isTrigger = state === "trigger";
  const interactive = !unavailable && !disabled;
  const toggle = () => {
    if (interactive) onTogglePin(pin);
  };
  const setTrigger = () => {
    if (interactive) onSetTriggerPin(isTrigger ? null : pin);
  };
  const tone = unavailable
    ? "border-line bg-panel2 text-ink-dim"
    : isTrigger
      ? "border-warn bg-warn/10 text-warn"
      : state === "selected"
        ? "border-brand bg-brand/10 text-brand"
        : "border-line bg-panel text-ink-dim hover:border-brand/40";
  return (
    <button
      type="button"
      data-logic-chip={pin}
      data-logic-pin-state={state}
      aria-pressed={state === "selected" || isTrigger}
      aria-disabled={interactive ? undefined : true}
      aria-keyshortcuts="Enter Space C"
      aria-label={t("logicAnalyzer.chipAria", { name: `GP${pin}`, state: logicPinStateLabel(state, t) })}
      aria-describedby={instructionsId}
      className={`inline-flex items-center gap-1 rounded-md border px-2 py-0.5 font-mono text-[11px] transition-colors focus:outline-none focus-visible:ring-2 focus-visible:ring-brand/40 ${tone} ${interactive ? "" : "cursor-not-allowed opacity-60"}`}
      onClick={setTrigger}
      onContextMenu={(event) => {
        event.preventDefault();
        toggle();
      }}
      onKeyDown={(event) => {
        if (event.ctrlKey || event.metaKey || event.altKey) {
          // Modifier combos never act on the chip. Modified Enter/Space only
          // suppress the native button activation; browser shortcuts such as
          // Ctrl/Cmd+C stay untouched.
          if (event.key === "Enter" || event.key === " ") event.preventDefault();
          return;
        }
        if (event.key === "c" || event.key === "C") {
          event.preventDefault();
          toggle();
        }
      }}
    >
      GP{pin}
    </button>
  );
}

function LegendSwatch({ fill, stroke, label }: { readonly fill: string; readonly stroke?: string; readonly label: string }) {
  return (
    <span className="inline-flex items-center gap-1">
      <span
        className="inline-block h-2.5 w-2.5 shrink-0 rounded-full"
        style={{ background: fill, border: `1px solid ${stroke ?? fill}` }}
      />
      {label}
    </span>
  );
}

export interface LogicAnalyzerPinSelectorProps {
  readonly physicalLayout: GpioPhysicalLayout | null;
  readonly boardGpios: readonly SafeGpio[];
  readonly selectedPins: readonly number[];
  readonly triggerPin: number | null;
  readonly triggerActive: boolean;
  readonly disabled?: boolean;
  readonly onTogglePin: (pin: number) => void;
  readonly onSetTriggerPin: (pin: number | null) => void;
}

/**
 * Capture-pin picker on the board's physical connector layout. Pins that the
 * physical layout cannot express (no controllable contact) keep their own
 * selection entry below the diagram, so switching layouts never drops a
 * capture pin. Pointer semantics match the legacy pinout: left click
 * (Enter / Space) sets or clears the trigger pin, right click (C) toggles
 * capture selection.
 */
export function LogicAnalyzerPinSelector({
  physicalLayout,
  boardGpios,
  selectedPins,
  triggerPin,
  triggerActive,
  disabled = false,
  onTogglePin,
  onSetTriggerPin,
}: LogicAnalyzerPinSelectorProps) {
  const { t } = useI18n();
  const instructionsId = useId();
  const stateOptions = { selectedPins, disabledPins: UNAVAILABLE_PINS, triggerPin, triggerActive };

  const liveByName = new Map(boardGpios.map((gpio) => [gpio.name, gpio]));
  const diagramPins = new Set<number>();
  if (physicalLayout) {
    for (const group of physicalLayout.groups) {
      for (const cell of group.cells) {
        // Same control predicate as the physical renderer, so a pin is either
        // selectable on the diagram or in the extra area, never neither.
        const controlName = physicalControlGpioName(cell);
        const gpio = controlName ? liveByName.get(controlName) : undefined;
        if (gpio && CONTROL_PINS.has(gpio.pin)) diagramPins.add(gpio.pin);
      }
    }
  }
  const chipPins = CAPTURE_PINS.filter((pin) => !diagramPins.has(pin));

  return (
    <div className="min-w-0 space-y-3" data-logic-pin-selector="">
      <div className="mt-3 grid grid-cols-2 overflow-hidden rounded-lg border border-line/50 bg-panel/70 text-[10px] leading-4 text-ink-dim">
        <span className="inline-flex min-w-0 items-center gap-1.5 px-2.5 py-2">
          <MousePointerClick size={13} className="shrink-0 text-warn" />
          {t("logicAnalyzer.leftClickAction")}
        </span>
        <span className="inline-flex min-w-0 items-center gap-1.5 border-l border-line/50 px-2.5 py-2">
          <MousePointerClick size={13} className="shrink-0 text-brand" />
          {t("logicAnalyzer.rightClickAction")}
        </span>
      </div>

      {physicalLayout ? (
        <GpioPhysicalPinoutSvg
          layout={physicalLayout}
          gpios={boardGpios}
          instructionsId={instructionsId}
          disabled={disabled}
          selectionPins={CAPTURE_PINS}
          selectedPins={selectedPins}
          disabledPins={UNAVAILABLE_PINS}
          triggerPin={triggerPin}
          triggerActive={triggerActive}
          onTogglePin={onTogglePin}
          onSetTriggerPin={onSetTriggerPin}
        />
      ) : (
        <p className="rounded-lg border border-line/50 bg-panel/70 px-3 py-2 text-[11px] text-ink-dim">
          {t("logicAnalyzer.pinoutUnavailable")}
        </p>
      )}

      {chipPins.length > 0 && (
        <div data-logic-extra-pins="">
          <div className="text-[10px] font-medium uppercase tracking-wide text-ink-dim">
            {t("logicAnalyzer.offLayoutPins")}
          </div>
          <div className="mt-1.5 flex flex-wrap gap-1.5">
            {chipPins.map((pin) => (
              <SelectionChip
                key={pin}
                pin={pin}
                state={logicPinState(pin, stateOptions)}
                disabled={disabled}
                instructionsId={instructionsId}
                onTogglePin={onTogglePin}
                onSetTriggerPin={onSetTriggerPin}
                t={t}
              />
            ))}
          </div>
        </div>
      )}

      <div className="grid grid-cols-2 gap-1 border-t border-line/50 pt-2 text-[9px] leading-3 text-ink-dim sm:grid-cols-4">
        <LegendSwatch fill={FILL_UNSELECTED} stroke={STROKE_DEFAULT} label={t("logicAnalyzer.pinout.legend.unselected")} />
        <LegendSwatch fill={FILL_SELECTED} label={t("logicAnalyzer.pinout.legend.selected")} />
        <LegendSwatch fill={FILL_TRIGGER} stroke={STROKE_TRIGGER} label={t("logicAnalyzer.pinout.legend.trigger")} />
        <LegendSwatch fill={FILL_DISABLED} stroke={STROKE_DISABLED} label={t("logicAnalyzer.pinout.legend.disabled")} />
      </div>

      <p id={instructionsId} className="text-[10px] text-ink-dim">
        {t("logicAnalyzer.pinKeys")}
      </p>
      <div className="space-y-1 text-[10px] text-ink-dim">
        <p>{t("logicAnalyzer.sigrokPinsHint")}</p>
        <p className="text-warn">{t("logicAnalyzer.j4PauseHint")}</p>
      </div>
    </div>
  );
}
