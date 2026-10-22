import { useId } from "react";
import type { GpioPhysicalCell, GpioPhysicalLayout, SafeGpio } from "@/lib/types";
import { useI18n } from "@/lib/i18n";
import { GpioPin, type GpioAction } from "./GpioPin";
import { GpioSelectionPin, logicPinState } from "./GpioSelectionPin";

/** Final display pose: contacts down, headers side by side, reverse metadata
 * order (J13 left / J8 right on this board). Source columns become display
 * rows; source rows run right-to-left. This preserves handedness/adjacency.
 * Labels are laid out directly at their final upright coordinates.
 */
export function placePhysicalAssembly(layout: GpioPhysicalLayout) {
  const groups = [...layout.groups].sort((a, b) => b.order - a.order);
  const rows = Math.max(0, ...groups.map((group) => group.columns));
  let offset = 0;
  const placed = groups.map((group) => {
    const displayColumn = offset;
    offset += group.rows + 1;
    return {
      group,
      row: 0,
      column: displayColumn,
      cells: group.cells.map((cell) => ({
        cell,
        row: cell.column,
        column: displayColumn + group.rows - 1 - cell.row,
      })).sort((a, b) => a.row - b.row || a.column - b.column),
    };
  });
  return { columns: Math.max(0, offset - 1), rows, groups: placed };
}

/** Cells that can carry a live control: firmware-controllable, never a
 * dedicated route contact, and joined to the live GPIO set by firmware name. */
export function physicalControlGpioName(cell: GpioPhysicalCell): string | null {
  return cell.controllable && cell.role !== "dedicated" && cell.gpioName ? cell.gpioName : null;
}

/** Presentation of firmware-provided purpose, not a net-to-MCU pin map. */
export function physicalPinPurpose(cell: GpioPhysicalCell): string {
  const label = cell.label.trim();
  if (cell.role === "dedicated") {
    // The label owns the purpose. A leading net name is never an MCU number.
    const purpose = label.split("/").at(-1)!.trim().replace(/\bswitch\b/giu, "SW").toUpperCase();
    return /^GP\d+$/u.test(purpose) ? "DED" : purpose;
  }
  if (cell.role === "adc") return label.match(/\bADC\d+\b/iu)?.[0].toUpperCase() ?? "ADC";
  if (cell.role === "i2c") return label.match(/\b(?:SDA|SCL)\b/iu)?.[0].toUpperCase() ?? "I2C";
  if (cell.role === "ground") return "GND";
  if (cell.role === "usb") {
    const channel = /\b(?:DP|D\+)\b/iu.test(label) || label.includes("D+") ? "D+" : /\bDM\b/iu.test(label) || /D[−-]/u.test(label) ? "D−" : "USB";
    return `${/hub/iu.test(label) ? "HUB" : "USB"} ${channel}`;
  }
  if (cell.role === "power") {
    const voltage = label.match(/\d+(?:\.\d+)?\s*V/iu)?.[0].replace(/\s+/gu, "").toUpperCase();
    if (voltage) return `${voltage}${/hub/iu.test(label) ? " HUB" : ""}`;
  }
  return label.split("/").at(-1)!.trim() || cell.gpioName || cell.role;
}

function purposeLabel(purpose: string) {
  const words = purpose.split(/\s+/u);
  const lines = words.length === 1 && purpose.length > 5 ? [purpose.slice(0, 4), purpose.slice(4, 8)] : words.slice(0, 2);
  return { lines, fontSize: lines.length > 1 ? 7.5 : purpose.length > 4 ? 8 : 9.5 };
}

function useRoleLabel() {
  const { t } = useI18n();
  return (cell: GpioPhysicalCell) => {
    const key = `pinout.role.${cell.role}`;
    const translated = t(key);
    const role = translated === key ? cell.role : translated;
    return cell.controllable && !cell.outputCapable ? `${role} · ${t("pinout.inputOnlyShort")}` : role;
  };
}

export interface GpioPhysicalPinoutSvgProps {
  readonly layout: GpioPhysicalLayout;
  readonly gpios: readonly SafeGpio[];
  readonly pendingPin?: number | null;
  readonly instructionsId?: string;
  readonly disabled?: boolean;
  readonly onGpioAction?: (gpioName: string, action: GpioAction) => void;
  /**
   * Logic-analyzer selection mode. When `selectionPins` is set, only those
   * pins render selection controls and every other contact is read-only
   * context; live GPIO control gestures are not bound at all.
   */
  readonly selectionPins?: readonly number[];
  readonly selectedPins?: readonly number[];
  readonly disabledPins?: readonly number[];
  readonly triggerPin?: number | null;
  readonly triggerActive?: boolean;
  readonly onTogglePin?: (pin: number) => void;
  readonly onSetTriggerPin?: (pin: number | null) => void;
}

/** The physical circles are the only GPIO controls. Inert pins bind no events. */
export function GpioPhysicalPinoutSvg({
  layout,
  gpios,
  pendingPin = null,
  instructionsId,
  disabled = false,
  onGpioAction,
  selectionPins,
  selectedPins,
  disabledPins,
  triggerPin = null,
  triggerActive = false,
  onTogglePin,
  onSetTriggerPin,
}: GpioPhysicalPinoutSvgProps) {
  const { t } = useI18n();
  const roleLabel = useRoleLabel();
  const id = useId();
  const assembly = placePhysicalAssembly(layout);
  const liveByName = new Map(gpios.map((gpio) => [gpio.name, gpio]));
  const selectionMode = selectionPins != null;
  const selectionPinSet = new Set(selectionPins ?? []);
  const selectionDisabledSet = new Set(disabledPins ?? []);
  const pitchX = 46;
  const pitchY = 66;
  const padding = 10;
  const top = 48;
  const width = Math.max(1, assembly.columns) * pitchX + padding * 2;
  const height = Math.max(1, assembly.rows) * pitchY + top - 12;
  const point = (row: number, column: number) => ({ x: padding + (column + 0.5) * pitchX, y: top + row * pitchY });

  return (
    <div className="min-w-0 space-y-2" data-physical-reference="">
      <p id={`${id}-description`} className="flex items-center justify-between gap-2 text-[11px] leading-4 text-ink-dim">
        <span>{t("pinout.pinsDown")}</span>
        <span aria-hidden="true" title={t("pinout.scrollHint")} className="font-mono sm:hidden">↔</span>
        <span className="sr-only">{t("pinout.physicalDescription")} {t("pinout.scrollHint")}</span>
      </p>
      <div className="min-w-0 max-w-full overflow-x-auto overscroll-x-contain rounded-lg border border-line/50 bg-panel/70" data-physical-viewport="">
      <svg role="group" aria-labelledby={`${id}-title`} aria-describedby={instructionsId ?? `${id}-description`}
        viewBox={`0 0 ${width} ${height}`} className="mx-auto block shrink-0" style={{ width, height }}
        data-physical-assembly="pins-down">
        <title id={`${id}-title`}>{assembly.groups.map(({ group }) => group.name).join(" / ")} · {t("pinout.pinsDown")}</title>
        {assembly.groups.map(({ group, row, column, cells }) => {
          const first = point(row, column);
          const frameX = first.x - pitchX / 2;
          const frameY = first.y - 22;
          const frameWidth = group.rows * pitchX;
          const frameHeight = group.columns * pitchY - 4;
          const sideExit = group.edge === "right";
          return (
            <g key={group.name} data-physical-group={group.name} data-layout-group={group.name} data-layout-edge={group.edge} data-contacts-direction={sideExit ? "down" : undefined}>
              <text x={frameX + 3} y={frameY - 8} fontFamily="monospace" fontSize={14} fontWeight={700} fill="rgb(var(--c-ink))">{group.name}</text>
              <g aria-hidden="true" pointerEvents="none">
                {sideExit ? (
                  <path d={`M ${frameX + 0.5} ${frameY + frameHeight} V ${frameY + 7} Q ${frameX + 0.5} ${frameY + 0.5} ${frameX + 7} ${frameY + 0.5} H ${frameX + frameWidth - 7} Q ${frameX + frameWidth - 0.5} ${frameY + 0.5} ${frameX + frameWidth - 0.5} ${frameY + 7} V ${frameY + frameHeight}`}
                    fill="rgb(var(--c-panel2) / 0.45)" stroke="rgb(var(--c-line))" />
                ) : (
                  <rect x={frameX + 0.5} y={frameY + 0.5} width={frameWidth - 1} height={frameHeight} rx={7} fill="rgb(var(--c-panel2) / 0.45)" stroke="rgb(var(--c-line))" />
                )}
                {sideExit && Array.from({ length: group.rows }, (_, index) => (
                  <line key={index} data-physical-contact="" x1={frameX + (index + 0.5) * pitchX} x2={frameX + (index + 0.5) * pitchX}
                    y1={frameY + frameHeight - 2} y2={frameY + frameHeight + 7}
                    stroke="rgb(var(--c-ink-dim))" strokeWidth={1.5} strokeLinecap="round" />
                ))}
              </g>
              {cells.map(({ cell, row: displayRow, column: displayColumn }) => {
                const { x: cx, y: cy } = point(displayRow, displayColumn);
                const controlName = physicalControlGpioName(cell);
                const gpio = controlName ? liveByName.get(controlName) : undefined;
                const purpose = physicalPinPurpose(cell);
                const label = purposeLabel(purpose);
                const dedicated = cell.role === "dedicated";
                const identity = t("pinout.cell", { group: group.name, pin: cell.pinNumber, label: purpose,
                  signal: dedicated ? t("pinout.netName", { name: cell.signal }) : cell.signal, role: roleLabel(cell) });
                const selectionPin =
                  selectionMode && gpio && (selectionPinSet.has(gpio.pin) || selectionDisabledSet.has(gpio.pin))
                    ? gpio.pin
                    : null;
                const selectionState = selectionPin == null ? null : logicPinState(selectionPin, {
                  selectedPins: selectedPins ?? [],
                  disabledPins: disabledPins ?? [],
                  triggerPin,
                  triggerActive,
                });
                return (
                  <g key={cell.pinNumber} data-physical-cell={cell.pinNumber} data-row={cell.row} data-column={cell.column}
                    data-display-row={displayRow} data-display-column={displayColumn} data-controllable={Boolean(gpio)}>
                    {selectionPin != null && selectionState != null ? (
                      <GpioSelectionPin
                        pin={selectionPin} cx={cx} cy={cy} label={label}
                        state={selectionState}
                        ariaLabel={t("logicAnalyzer.pinAria", {
                          name: `GP${selectionPin}`,
                          group: group.name,
                          pin: cell.pinNumber,
                          state: t(`logicAnalyzer.pinout.legend.${selectionState}`),
                        })}
                        instructionsId={instructionsId} locked={disabled}
                        onTogglePin={onTogglePin ?? (() => {})} onSetTriggerPin={onSetTriggerPin ?? (() => {})} />
                    ) : gpio && !selectionMode ? <GpioPin
                      cell={{ ...gpio, layoutGroup: group.name, layoutLabel: purpose, layoutRow: cell.row, layoutColumn: cell.column }}
                      cx={cx} cy={cy} pendingPin={pendingPin} instructionsId={instructionsId} disabled={disabled}
                      outputCapable={cell.outputCapable} hitTargetRadius={22} label={label} t={t}
                      onAction={(_pin, action) => onGpioAction?.(gpio.name, action)}
                      ariaLabel={`${t("gpio.pinAria", { name: gpio.name, pin: gpio.pin, direction: t(gpio.direction === "output" ? "gpio.output" : "gpio.input"), level: t(gpio.value > 0 ? "gpio.high" : "gpio.low") })}. ${identity}${cell.outputCapable ? "" : `. ${t("pinout.inputOnly")}`}`} /> : (
                      <g data-physical-inert="" data-physical-role={cell.role} role="img"
                        aria-label={`${identity}${dedicated ? `. ${t("pinout.routeOwned")}` : ""}`}>
                        <title>{identity}{dedicated ? `. ${t("pinout.routeOwned")}` : ""}</title>
                        <circle cx={cx} cy={cy} r={11.5} fill={dedicated ? "rgb(var(--c-brand) / 0.08)" : "rgb(var(--c-panel2))"} />
                        <circle cx={cx} cy={cy} r={14} fill="none" stroke={dedicated ? "rgb(var(--c-brand) / 0.55)" : "rgb(var(--c-line))"} strokeWidth={2.5} />
                        <text x={cx} y={cy + (label.lines.length === 1 ? label.fontSize * 0.34 : -1.5)} textAnchor="middle" fontFamily="monospace" fontSize={label.fontSize}
                          fontWeight={700} fill={dedicated ? "rgb(var(--c-brand))" : "rgb(var(--c-ink-dim))"}>
                          {label.lines.map((line, index) => <tspan key={index} x={cx} dy={index ? label.fontSize * 0.95 : 0}>{line}</tspan>)}
                        </text>
                      </g>
                    )}
                    <text x={cx - 17} y={cy - 18} textAnchor="middle" fontFamily="monospace" fontSize={9} fontWeight={700} fill="rgb(var(--c-ink-dim))" aria-hidden="true" pointerEvents="none" data-physical-number="">
                      {cell.pinNumber}
                    </text>
                  </g>
                );
              })}
            </g>
          );
        })}
      </svg>
      </div>
    </div>
  );
}
