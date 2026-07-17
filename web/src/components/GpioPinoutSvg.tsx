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

const PIN_RADIUS = 12;
const COL_GAP = 6;
const ROW_GAP = 4;
const PADDING = 6;
const HEADER_HEIGHT = 14;
const TRIGGER_BADGE_OFFSET = 2;
const TOUCH_TARGET_RADIUS = 16;

const FILL_UNSELECTED = "#1f2937";
const FILL_SELECTED = "#22c55e";
const FILL_TRIGGER = "#facc15";
const STROKE_TRIGGER = "#a16207";
const STROKE_DEFAULT = "#475569";
const LABEL_LIGHT = "#e2e8f0";
const LABEL_DARK = "#1a1a1a";
const LABEL_DIM = "#94a3b8";
const HEADER_TEXT = "#cbd5e1";
const CONNECTOR_OUTLINE = "#94a3b8";

interface PlacedCell {
  cell: GpioLayoutCell;
  cx: number;
  cy: number;
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
        rx={6}
        ry={6}
        fill="none"
        stroke={CONNECTOR_OUTLINE}
        strokeWidth={1}
        strokeDasharray="3 2"
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
        const stroke = isTrigger ? STROKE_TRIGGER : STROKE_DEFAULT;
        const strokeWidth = isTrigger ? 2 : 1;
        const labelFill = isTrigger ? LABEL_DARK : LABEL_LIGHT;
        return (
          <g
            key={`pin-${cell.pin}`}
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
              y={cy + 4}
              textAnchor="middle"
              fontFamily="monospace"
              fontSize={11}
              fontWeight={700}
              fill={labelFill}
            >
              {cell.layoutLabel}
            </text>
            {isTrigger && (
              <text
                x={cx}
                y={cy - PIN_RADIUS - TRIGGER_BADGE_OFFSET}
                textAnchor="middle"
                fontFamily="monospace"
                fontSize={9}
                fontWeight={700}
                fill={STROKE_TRIGGER}
              >
                TRIG
              </text>
            )}
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
  const gap = 4;
  const totalWidth = placed.reduce(
    (max, g) => Math.max(max, g.width),
    100
  );
  const totalHeight = placed.reduce(
    (sum, g, idx) => (idx === 0 ? g.height : sum + g.height + gap),
    0
  );

  let cursorY = 0;
  const groupNodes = placed.map((g, idx) => {
    const offsetX = (totalWidth - g.width) / 2;
    const node = (
      <g key={`g-${g.group.group}-${idx}`} transform={`translate(${offsetX}, ${cursorY})`}>
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
    cursorY += g.height + (idx === placed.length - 1 ? 0 : gap);
    return node;
  });

  return (
    <div className="space-y-1">
      <svg
        role="img"
        aria-labelledby={titleId}
        viewBox={`0 0 ${totalWidth} ${totalHeight}`}
        className="w-full"
        style={{ maxWidth: 160 }}
      >
        <title id={titleId}>{t("logicAnalyzer.pinoutAria")}</title>
        {groupNodes}
      </svg>
      <div className="flex flex-wrap items-center gap-2 text-[10px] text-ink-dim">
        <span className="inline-flex items-center gap-1">
          <span
            className="inline-block h-2 w-2 rounded-full"
            style={{ background: FILL_UNSELECTED, border: `1px solid ${STROKE_DEFAULT}` }}
          />
          {t("logicAnalyzer.pinout.legend.unselected")}
        </span>
        <span className="inline-flex items-center gap-1">
          <span
            className="inline-block h-2 w-2 rounded-full"
            style={{ background: FILL_SELECTED }}
          />
          {t("logicAnalyzer.pinout.legend.selected")}
        </span>
        <span className="inline-flex items-center gap-1">
          <span
            className="inline-block h-2 w-2 rounded-full"
            style={{ background: FILL_TRIGGER, border: `1px solid ${STROKE_TRIGGER}` }}
          />
          {t("logicAnalyzer.pinout.legend.trigger")}
        </span>
      </div>
    </div>
  );
}