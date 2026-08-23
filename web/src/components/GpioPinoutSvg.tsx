import { useId } from "react";
import type { SafeGpio } from "@/lib/types";
import { groupGpioLayout, type GpioLayoutGroup } from "@/lib/gpioLayout";
import { useI18n } from "@/lib/i18n";
import { GPIO_DIRECTION_STROKE, GPIO_FILL_HIGH, GPIO_FILL_LOW, type GpioAction } from "./GpioPin";
import {
  FILL_SELECTED,
  FILL_TRIGGER,
  FILL_UNSELECTED,
  GpioPinoutGroup,
  STROKE_DEFAULT,
  STROKE_TRIGGER,
  placeGroup,
  type GpioPinoutSvgVariant,
} from "./GpioPinoutGroup";

export type { GpioPinoutSvgVariant };

export interface GpioPinoutSvgProps {
  gpios: SafeGpio[];
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
  const { groups: layoutGroups, fallback } = groupGpioLayout(gpios);
  const selectedSet = new Set(selectedPins ?? []);
  const disabledSet = new Set(disabledPins ?? []);
  const trigger = triggerPin ?? null;

  const allGroups = fallback ? [...layoutGroups, fallback] : layoutGroups;
  if (allGroups.length === 0) {
    return (
      <div className="rounded-lg border border-line/60 bg-panel2/30 p-4 text-xs text-ink-dim">
        {t("logicAnalyzer.noLayout")}
      </div>
    );
  }

  const groups: { group: GpioLayoutGroup; label: string }[] = allGroups.map((group) => ({
    group: group.generic ? { ...group, label: t("logicAnalyzer.fallbackGroup") } : group,
    label: group.generic ? t("logicAnalyzer.fallbackSubtitle") : "",
  }));

  const placed = groups.map((g) => ({ ...g, ...placeGroup(g.group) }));
  const gap = 8;
  const totalWidth = placed.reduce(
    (sum, g, idx) => sum + g.width + (idx === 0 ? 0 : gap),
    0
  );
  const totalHeight = placed.reduce((max, g) => Math.max(max, g.height), 1);

  let cursorX = 0;
  const groupNodes = placed.map((g, idx) => {
    const node = (
      <g key={`g-${g.group.group}-${idx}`} transform={`translate(${cursorX}, 0)`}>
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
          label={g.label}
          t={t}
        />
      </g>
    );
    cursorX += g.width + (idx === placed.length - 1 ? 0 : gap);
    return node;
  });

  return (
    <div className="space-y-2">
      <svg
        role={variant === "gpio" ? "group" : "img"}
        aria-labelledby={titleId}
        viewBox={`0 0 ${totalWidth} ${totalHeight}`}
        className="mx-auto w-full"
        style={{ maxWidth: 224 }}
      >
        <title id={titleId}>
          {variant === "gpio" ? t("gpio.pinoutAria") : t("logicAnalyzer.pinoutAria")}
        </title>
        {groupNodes}
      </svg>
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
