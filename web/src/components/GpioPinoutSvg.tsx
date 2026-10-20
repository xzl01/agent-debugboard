import { useId } from "react";
import type { GpioLayoutGroupMetadata, SafeGpio } from "@/lib/types";
import { groupGpioLayout } from "@/lib/gpioLayout";
import { useI18n } from "@/lib/i18n";
import { GPIO_DIRECTION_STROKE, GPIO_FILL_HIGH, GPIO_FILL_LOW, type GpioAction } from "./GpioPin";
import {
  FILL_SELECTED,
  FILL_TRIGGER,
  FILL_UNSELECTED,
  GpioPinoutGroup,
  STROKE_DEFAULT,
  STROKE_TRIGGER,
  PINOUT_DISPLAY_SCALE,
  placeGroup,
  type GpioPinoutSvgVariant,
} from "./GpioPinoutGroup";

export type { GpioPinoutSvgVariant };

export interface GpioPinoutSvgProps {
  gpios: SafeGpio[];
  layoutGroups?: readonly GpioLayoutGroupMetadata[];
  variant?: GpioPinoutSvgVariant;
  selectedPins?: readonly number[];
  triggerPin?: number | null;
  triggerActive?: boolean;
  disabledPins?: readonly number[];
  onTogglePin?: (pin: number) => void;
  onSetTriggerPin?: (pin: number | null) => void;
  gpioPendingPin?: number | null;
  gpioInstructionsId?: string;
  onGpioAction?: (pin: number, action: GpioAction) => void;
}

export function GpioPinoutSvg({
  gpios,
  layoutGroups,
  variant = "logic-analyzer",
  selectedPins,
  triggerPin,
  triggerActive = false,
  disabledPins,
  onTogglePin,
  onSetTriggerPin,
  gpioPendingPin,
  gpioInstructionsId,
  onGpioAction,
}: GpioPinoutSvgProps) {
  const { t } = useI18n();
  const titleId = useId();
  const descriptionId = useId();
  const { groups: grouped, fallback } = groupGpioLayout(gpios, layoutGroups);
  const selectedSet = new Set(selectedPins ?? []);
  const disabledSet = new Set(disabledPins ?? []);
  const trigger = triggerPin ?? null;

  const allGroups = fallback ? [...grouped, fallback] : grouped;
  if (allGroups.length === 0) {
    return (
      <div className="rounded-lg border border-line/60 bg-panel2/30 p-4 text-xs text-ink-dim">
        {t("logicAnalyzer.noLayout")}
      </div>
    );
  }

  const placed = allGroups.map((rawGroup) => {
    const group = rawGroup.generic ? { ...rawGroup, label: t("logicAnalyzer.fallbackGroup") } : rawGroup;
    return { group, ...placeGroup(group) };
  });
  const description = [
    t("pinout.controllableOnly"),
    allGroups.some((group) => group.edge === "right") ? t("pinout.rightEdge") : "",
    allGroups.some((group) => !group.generic && group.edge !== "right") ? t("pinout.genericLayout") : "",
    fallback ? t("logicAnalyzer.fallbackSubtitle") : "",
  ].filter(Boolean).join(" ");

  const groupNodes = placed.map((g, idx) => (
    <div key={g.group.group} className="min-w-0 max-w-full overflow-x-auto">
      <svg
        role={variant === "gpio" ? "group" : "img"}
        aria-labelledby={`${titleId}-${idx}`}
        aria-describedby={descriptionId}
        viewBox={`0 0 ${g.width} ${g.height}`}
        className="block shrink-0"
        style={{ width: Math.ceil(g.width * PINOUT_DISPLAY_SCALE), height: Math.ceil(g.height * PINOUT_DISPLAY_SCALE) }}
      >
        <title id={`${titleId}-${idx}`}>
          {variant === "gpio" ? t("gpio.pinoutAria") : t("logicAnalyzer.pinoutAria")} — {g.group.label}
        </title>
        <GpioPinoutGroup
          group={g.group}
          variant={variant}
          selectedSet={selectedSet}
          disabledSet={disabledSet}
          triggerPin={trigger}
          triggerActive={triggerActive}
          onTogglePin={onTogglePin}
          onSetTriggerPin={onSetTriggerPin}
          gpioPendingPin={gpioPendingPin}
          gpioInstructionsId={gpioInstructionsId}
          onGpioAction={onGpioAction}
          t={t}
        />
      </svg>
    </div>
  ));

  return (
    <div className="min-w-0 space-y-2">
      <div className="flex flex-wrap items-start justify-center gap-2">
        {groupNodes}
      </div>
      <p id={descriptionId} className="text-[11px] leading-4 text-ink-dim">{description}</p>
      {variant === "gpio" ? (
        <div className="grid grid-cols-2 gap-1 border-t border-line/50 pt-2 text-[9px] leading-3 text-ink-dim">
          <span className="inline-flex items-center gap-1">
            <span
              className="inline-block h-2.5 w-2.5 shrink-0 rounded-full"
              style={{ background: GPIO_FILL_LOW, border: `1px solid ${GPIO_DIRECTION_STROKE}` }}
            />
            {t("gpio.low")}
          </span>
          <span className="inline-flex items-center gap-1">
            <span
              className="inline-block h-2.5 w-2.5 shrink-0 rounded-full"
              style={{ background: GPIO_FILL_HIGH, border: `1px solid ${GPIO_DIRECTION_STROKE}` }}
            />
            {t("gpio.high")}
          </span>
          <span className="inline-flex items-center gap-1">
            <span
              className="inline-block h-2.5 w-2.5 shrink-0 rounded-full"
              style={{ border: `1.5px dashed ${GPIO_DIRECTION_STROKE}` }}
            />
            {t("gpio.input")}
          </span>
          <span className="inline-flex items-center gap-1">
            <span
              className="inline-block h-2.5 w-2.5 shrink-0 rounded-full"
              style={{ border: `1.5px solid ${GPIO_DIRECTION_STROKE}` }}
            />
            {t("gpio.output")}
          </span>
        </div>
      ) : (
        <div className="grid grid-cols-3 gap-1 border-t border-line/50 pt-2 text-[9px] leading-3 text-ink-dim">
          <span className="inline-flex items-center gap-1">
            <span
              className="inline-block h-2.5 w-2.5 shrink-0 rounded-full"
              style={{ background: FILL_UNSELECTED, border: `1px solid ${STROKE_DEFAULT}` }}
            />
            {t("logicAnalyzer.pinout.legend.unselected")}
          </span>
          <span className="inline-flex items-center gap-1">
            <span
              className="inline-block h-2.5 w-2.5 shrink-0 rounded-full"
              style={{ background: FILL_SELECTED }}
            />
            {t("logicAnalyzer.pinout.legend.selected")}
          </span>
          <span className="inline-flex items-center gap-1">
            <span
              className="inline-block h-2.5 w-2.5 shrink-0 rounded-full"
              style={{ background: FILL_TRIGGER, border: `1px solid ${STROKE_TRIGGER}` }}
            />
            {t("logicAnalyzer.pinout.legend.trigger")}
          </span>
        </div>
      )}
    </div>
  );
}
