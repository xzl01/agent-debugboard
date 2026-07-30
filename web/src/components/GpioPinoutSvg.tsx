import { useId } from "react";
import type { SafeGpio } from "@/lib/types";
import {
  groupGpioLayout,
  type GpioLayoutCell,
  type GpioLayoutGroup,
} from "@/lib/gpioLayout";
import { useI18n } from "@/lib/i18n";

export interface GpioPinoutSvgProps {
  gpios: SafeGpio[];
  selectedPins?: readonly number[];
  triggerPin?: number | null;
  triggerActive?: boolean;
  onTogglePin?: (pin: number) => void;
  onSetTriggerPin?: (pin: number | null) => void;
}

const PIN_RADIUS = 15;
const COL_GAP = 6;
const ROW_GAP = 4;
const PADDING = 6;
const HEADER_HEIGHT = 14;
const TOUCH_TARGET_RADIUS = 17;

const FILL_UNSELECTED = "rgb(var(--c-panel))";
const FILL_SELECTED = "rgb(var(--c-brand) / 0.14)";
const FILL_TRIGGER = "rgb(var(--c-warn) / 0.14)";
const STROKE_SELECTED = "rgb(var(--c-brand) / 0.75)";
const STROKE_TRIGGER = "rgb(var(--c-warn) / 0.8)";
const STROKE_DEFAULT = "rgb(var(--c-line))";
const LABEL_DEFAULT = "rgb(var(--c-ink))";
const LABEL_SELECTED = "rgb(var(--c-brand))";
const LABEL_TRIGGER = "rgb(var(--c-warn))";
const LABEL_DIM = "rgb(var(--c-ink-dim))";
const HEADER_TEXT = "rgb(var(--c-ink))";
const CONNECTOR_OUTLINE = "rgb(var(--c-line))";

interface PlacedCell {
  cell: GpioLayoutCell;
  cx: number;
  cy: number;
}

function pinLabelFontSize(label: string): number {
  if (label.length <= 4) return 9.5;
  if (label.length <= 5) return 8;
  return 7.5;
}

function pinLabelLines(label: string): string[] {
  if (label.length <= 5) return [label];
  return [label.slice(0, 4), label.slice(4)];
}

function placeGroup(group: GpioLayoutGroup): {
  width: number;
  height: number;
  cells: PlacedCell[];
} {
  const diameter = PIN_RADIUS * 2;
  const groupWidth =
    PADDING * 2 + group.columnCount * diameter + (group.columnCount - 1) * COL_GAP;
  const groupHeight =
    PADDING * 2 + HEADER_HEIGHT + group.rowCount * diameter + (group.rowCount - 1) * ROW_GAP;
  const topOffset = PADDING + HEADER_HEIGHT;
  const cells: PlacedCell[] = [];
  for (let row = 0; row < group.rowCount; row++) {
    const rowCells = group.rows.get(row) ?? [];
    for (let col = 0; col < group.columnCount; col++) {
      const cell = rowCells[col] ?? null;
      if (!cell) continue;
      const cx = PADDING + col * (diameter + COL_GAP) + PIN_RADIUS;
      const cy = topOffset + row * (diameter + ROW_GAP) + PIN_RADIUS;
      cells.push({ cell, cx, cy });
    }
  }
  return { width: groupWidth, height: groupHeight, cells };
}

function renderGroup(
  group: GpioLayoutGroup,
  selectedSet: ReadonlySet<number>,
  triggerPin: number | null,
  triggerActive: boolean,
  onTogglePin: ((pin: number) => void) | undefined,
  onSetTriggerPin: ((pin: number | null) => void) | undefined,
  labelForGroup: string
) {
  const { width, height, cells } = placeGroup(group);
  return (
    <g key={group.group}>
      <rect
        x={0.5}
        y={0.5}
        width={width - 1}
        height={height - 1}
        rx={10}
        ry={10}
        fill="rgb(var(--c-panel2) / 0.45)"
        stroke={CONNECTOR_OUTLINE}
        strokeWidth={1}
      />
      <text
        x={width / 2}
        y={PADDING + 4}
        textAnchor="middle"
        fontFamily="monospace"
        fontSize={10}
        fontWeight={700}
        fill={HEADER_TEXT}
      >
        {group.label}
      </text>
      <text
        x={width / 2}
        y={PADDING + 13}
        textAnchor="middle"
        fontFamily="monospace"
        fontSize={8}
        fill={LABEL_DIM}
      >
        {labelForGroup}
      </text>
      {cells.map(({ cell, cx, cy }) => {
        const isTrigger = triggerPin === cell.pin && triggerActive;
        const isSelected = selectedSet.has(cell.pin);
        const fill = isTrigger
          ? FILL_TRIGGER
          : isSelected
            ? FILL_SELECTED
            : FILL_UNSELECTED;
        const stroke = isTrigger
          ? STROKE_TRIGGER
          : isSelected
            ? STROKE_SELECTED
            : STROKE_DEFAULT;
        const strokeWidth = isTrigger || isSelected ? 1.5 : 1;
        const labelFill = isTrigger
          ? LABEL_TRIGGER
          : isSelected
            ? LABEL_SELECTED
            : LABEL_DEFAULT;
        const labelFontSize = pinLabelFontSize(cell.layoutLabel);
        const labelLines = pinLabelLines(cell.layoutLabel);
        return (
          <g
            key={`pin-${cell.pin}`}
            className="transition-opacity duration-150 hover:opacity-80"
            style={{ cursor: "pointer" }}
            onClick={() => {
              if (isTrigger) {
                onSetTriggerPin?.(null);
              } else {
                onSetTriggerPin?.(cell.pin);
              }
            }}
            onContextMenu={(event) => {
              event.preventDefault();
              onTogglePin?.(cell.pin);
            }}
          >
            <circle
              cx={cx}
              cy={cy}
              r={PIN_RADIUS}
              fill={fill}
              stroke={stroke}
              strokeWidth={strokeWidth}
            />
            <text
              x={cx}
              y={labelLines.length === 1 ? cy + labelFontSize * 0.34 : cy - 1.5}
              textAnchor="middle"
              fontFamily="monospace"
              fontSize={labelFontSize}
              fontWeight={700}
              fill={labelFill}
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
              r={TOUCH_TARGET_RADIUS}
              fill="transparent"
              pointerEvents="all"
            />
          </g>
        );
      })}
    </g>
  );
}

export function GpioPinoutSvg({
  gpios,
  selectedPins,
  triggerPin,
  triggerActive = false,
  onTogglePin,
  onSetTriggerPin,
}: GpioPinoutSvgProps) {
  const { t } = useI18n();
  const titleId = useId();
  const { j13, j16, fallback } = groupGpioLayout(gpios);
  const selectedSet = new Set(selectedPins ?? []);
  const trigger = triggerPin ?? null;

  if (!j13 && !j16) {
    return (
      <div className="rounded-lg border border-line/60 bg-panel2/30 p-4 text-xs text-ink-dim">
        {t("logicAnalyzer.noLayout")}
      </div>
    );
  }

  const groups: { group: GpioLayoutGroup; label: string }[] = [];
  if (j13) groups.push({ group: j13, label: t("logicAnalyzer.j13Subtitle") });
  if (j16) groups.push({ group: j16, label: t("logicAnalyzer.j16Subtitle") });

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
        {renderGroup(
          g.group,
          selectedSet,
          trigger,
          triggerActive,
          onTogglePin,
          onSetTriggerPin,
          g.label
        )}
      </g>
    );
    cursorX += g.width + (idx === placed.length - 1 ? 0 : gap);
    return node;
  });

  return (
    <div className="space-y-2">
      <svg
        role="img"
        aria-labelledby={titleId}
        viewBox={`0 0 ${totalWidth} ${totalHeight}`}
        className="mx-auto w-full"
        style={{ maxWidth: 224 }}
      >
        <title id={titleId}>{t("logicAnalyzer.pinoutAria")}</title>
        {groupNodes}
      </svg>
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
    </div>
  );
}
