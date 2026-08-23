import type { GpioLayoutCell, GpioLayoutGroup } from "@/lib/gpioLayout";
import { GpioPin, pinLabelFontSize, pinLabelLines, type GpioAction } from "./GpioPin";

export type GpioPinoutSvgVariant = "logic-analyzer" | "gpio";

export interface GpioPinoutGroupProps {
  readonly group: GpioLayoutGroup;
  readonly variant: GpioPinoutSvgVariant;
  readonly selectedSet: ReadonlySet<number>;
  readonly disabledSet: ReadonlySet<number>;
  readonly triggerPin: number | null;
  readonly triggerActive: boolean;
  readonly onTogglePin: ((pin: number) => void) | undefined;
  readonly onSetTriggerPin: ((pin: number | null) => void) | undefined;
  readonly gpioPendingPin?: number | null;
  readonly gpioInstructionsId?: string;
  readonly onGpioAction?: ((pin: number, action: GpioAction) => void) | undefined;
  readonly label: string;
  readonly t: (key: string, params?: Record<string, string | number>) => string;
}

const PIN_RADIUS = 15;
const COL_GAP = 6;
const ROW_GAP = 4;
const PADDING = 6;
const HEADER_HEIGHT = 14;
const TOUCH_TARGET_RADIUS = 17;

export const FILL_UNSELECTED = "rgb(var(--c-panel))";
export const FILL_SELECTED = "rgb(var(--c-brand) / 0.14)";
export const FILL_TRIGGER = "rgb(var(--c-warn) / 0.14)";
export const STROKE_SELECTED = "rgb(var(--c-brand) / 0.75)";
export const STROKE_TRIGGER = "rgb(var(--c-warn) / 0.8)";
export const STROKE_DEFAULT = "rgb(var(--c-line))";
const LABEL_DEFAULT = "rgb(var(--c-ink))";
const LABEL_SELECTED = "rgb(var(--c-brand))";
const LABEL_TRIGGER = "rgb(var(--c-warn))";
const LABEL_DIM = "rgb(var(--c-ink-dim))";
const HEADER_TEXT = "rgb(var(--c-ink))";
const CONNECTOR_OUTLINE = "rgb(var(--c-line))";
const FILL_DISABLED = "rgb(var(--c-panel2))";
const STROKE_DISABLED = "rgb(var(--c-ink-dim) / 0.45)";

interface PlacedCell {
  cell: GpioLayoutCell;
  cx: number;
  cy: number;
}

export function placeGroup(group: GpioLayoutGroup): {
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

export function GpioPinoutGroup({
  group,
  variant,
  selectedSet,
  disabledSet,
  triggerPin,
  triggerActive,
  onTogglePin,
  onSetTriggerPin,
  gpioPendingPin = null,
  gpioInstructionsId,
  onGpioAction,
  label,
  t,
}: GpioPinoutGroupProps) {
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
        {label}
      </text>
      {cells.map(({ cell, cx, cy }) => {
        if (variant === "gpio") {
          return (
            <GpioPin
              key={`pin-${cell.pin}`}
              cell={cell}
              cx={cx}
              cy={cy}
              pendingPin={gpioPendingPin}
              instructionsId={gpioInstructionsId}
              onAction={onGpioAction}
              t={t}
            />
          );
        }
        const isDisabled = disabledSet.has(cell.pin);
        const isTrigger = triggerPin === cell.pin && triggerActive;
        const isSelected = selectedSet.has(cell.pin);
        const fill = isDisabled
          ? FILL_DISABLED
          : isTrigger
            ? FILL_TRIGGER
            : isSelected
              ? FILL_SELECTED
              : FILL_UNSELECTED;
        const stroke = isDisabled
          ? STROKE_DISABLED
          : isTrigger
            ? STROKE_TRIGGER
            : isSelected
              ? STROKE_SELECTED
              : STROKE_DEFAULT;
        const strokeWidth = isDisabled ? 1 : isTrigger || isSelected ? 1.5 : 1;
        const labelFill = isDisabled
          ? LABEL_DIM
          : isTrigger
            ? LABEL_TRIGGER
            : isSelected
              ? LABEL_SELECTED
              : LABEL_DEFAULT;
        const labelFontSize = pinLabelFontSize(cell.layoutLabel);
        const labelLines = pinLabelLines(cell.layoutLabel);
        return (
          <g
            key={`pin-${cell.pin}`}
            className={
              isDisabled
                ? "transition-opacity duration-150"
                : "transition-opacity duration-150 hover:opacity-80"
            }
            style={{ cursor: isDisabled ? "not-allowed" : "pointer", opacity: isDisabled ? 0.65 : 1 }}
            onClick={() => {
              if (isDisabled) return;
              if (!isTrigger) {
                onSetTriggerPin?.(cell.pin);
              } else {
                onSetTriggerPin?.(null);
              }
            }}
            onContextMenu={(event) => {
              event.preventDefault();
              if (isDisabled) return;
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
