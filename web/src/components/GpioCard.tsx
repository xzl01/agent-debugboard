import { useEffect, useMemo, useState } from "react";
import { Pin, Search } from "lucide-react";
import { Badge, Button, Card, Toggle } from "./ui";
import type { SafeGpio } from "@/lib/types";
import { useI18n } from "@/lib/i18n";

function GpioRow({
  gpio,
  onApply,
  disabled,
  stale,
  compact,
}: {
  gpio: SafeGpio;
  onApply: (identifier: string, direction: "input" | "output", value?: number) => void;
  disabled?: boolean;
  stale?: boolean;
  compact?: boolean;
}) {
  const { t } = useI18n();
  const actualDirection = gpio.direction === "output" ? "output" : "input";
  const [direction, setDirection] = useState<"input" | "output">(
    actualDirection
  );
  const [value, setValue] = useState(gpio.value > 0);

  useEffect(() => {
    setDirection(gpio.direction === "output" ? "output" : "input");
    setValue(gpio.value > 0);
  }, [gpio.direction, gpio.value]);

  return (
    <li className={compact ? "flex h-9 items-center justify-between border-b border-line/50 last:border-b-0" : "rounded-lg border border-line/50 bg-panel2/40 px-3 py-2.5"}>
      <div className={compact ? "flex w-full items-center justify-between gap-3" : "flex flex-wrap items-start justify-between gap-3"}>
        <div className="min-w-0">
          <div className="flex items-center gap-2">
            <span className="font-medium text-ink">{gpio.name}</span>
            {compact ? (
              <span className="text-[10px] text-ink-dim">
                {direction === "output" ? t("gpio.output") : t("gpio.input")}
              </span>
            ) : (
              <Badge tone="neutral">GP{gpio.pin}</Badge>
            )}
          </div>
          {!compact && <div className="text-xs text-ink-dim break-words">{gpio.note}</div>}
        </div>
        {compact ? (
          direction === "output" ? (
            <div className="flex items-center gap-2 text-xs text-ink-dim">
              <span>{value ? t("gpio.high") : t("gpio.low")}</span>
              <Toggle
                checked={value}
                disabled={disabled}
                onChange={(next) => {
                  setValue(next);
                  onApply(gpio.name, "output", next ? 1 : 0);
                }}
              />
            </div>
          ) : (
            <span className="text-xs text-ink-dim">
              {t(stale ? "snapshot.last" : "gpio.current")}: {gpio.value > 0 ? t("gpio.high") : t("gpio.low")}
            </span>
          )
        ) : <div className="flex max-w-full flex-wrap items-center gap-2">
          <select
            value={direction}
            disabled={disabled}
            onChange={(e) => setDirection(e.target.value as "input" | "output")}
            className="rounded-md border border-line/70 bg-panel2 px-2 py-1 text-xs text-ink outline-none"
          >
            <option value="input">{t("gpio.input")}</option>
            <option value="output">{t("gpio.output")}</option>
          </select>
          {direction === "output" && (
            <select
              value={value ? "1" : "0"}
              disabled={disabled}
              onChange={(e) => setValue(e.target.value === "1")}
              className="rounded-md border border-line/70 bg-panel2 px-2 py-1 text-xs text-ink outline-none"
            >
              <option value="1">{t("gpio.high")}</option>
              <option value="0">{t("gpio.low")}</option>
            </select>
          )}
          <Button
            variant="ghost"
            disabled={disabled}
            className="px-2 py-1"
            onClick={() => onApply(gpio.name, direction, value ? 1 : 0)}
          >
            {t("gpio.set")}
          </Button>
        </div>}
      </div>
      {!compact && <div className="mt-1 text-[11px] text-ink-dim">
        {t(stale ? "snapshot.last" : "gpio.current")}: {t(`gpio.${actualDirection}`)} · {gpio.value > 0 ? t("gpio.high") : t("gpio.low")}
      </div>}
    </li>
  );
}

export function GpioCard({
  gpios,
  onSet,
  disabled = false,
  stale = false,
  compact = false,
  limit,
  onOpenDetails,
}: {
  gpios: SafeGpio[];
  onSet: (identifier: string, direction: "input" | "output", value?: number) => void;
  disabled?: boolean;
  stale?: boolean;
  compact?: boolean;
  limit?: number;
  onOpenDetails?: () => void;
}) {
  const { t } = useI18n();
  const [query, setQuery] = useState("");
  const filteredGpios = useMemo(() => {
    const normalized = query.trim().toLowerCase();
    if (compact || normalized.length === 0) return gpios;
    return gpios.filter((gpio) => [gpio.name, `GP${gpio.pin}`, gpio.note]
      .some((value) => value.toLowerCase().includes(normalized)));
  }, [compact, gpios, query]);
  return (
    <Card
      title={t("gpio.title")}
      subtitle={compact ? undefined : t("gpio.subtitle")}
      icon={compact ? undefined : Pin}
      right={compact || stale ? (
        <div className="flex items-center gap-2">
          {compact && <span className="text-[10px] text-ink-dim">{t("quick.commonPins")}</span>}
          {stale && <Badge tone="neutral">{t("snapshot.readOnly")}</Badge>}
        </div>
      ) : undefined}
      className={compact ? "rounded-none border-0 shadow-none" : "max-h-[440px]"}
      headerClassName={compact ? "min-h-11 px-3 py-2" : undefined}
      contentClassName={compact ? "flex min-h-0 flex-col p-3" : "flex min-h-0 flex-col"}
    >
      {gpios.length === 0 ? (
        <p className="text-sm text-ink-dim">{t("gpio.none")}</p>
      ) : (
        <>
          {!compact && (
            <label className="mb-3 block text-[11px] font-medium text-ink-dim">
              {t("gpio.filter")}
              <span className="relative mt-1 block">
                <Search size={14} className="pointer-events-none absolute left-2.5 top-1/2 -translate-y-1/2 text-ink-dim" />
                <input
                  type="search"
                  value={query}
                  data-testid="gpio-filter"
                  placeholder={t("gpio.filter.placeholder")}
                  onChange={(event) => setQuery(event.target.value)}
                  className="min-h-9 w-full rounded-lg border border-line/70 bg-panel2/40 py-2 pl-8 pr-3 text-xs text-ink outline-none placeholder:text-ink-dim/70 focus-visible:ring-2 focus-visible:ring-brand/30"
                />
              </span>
            </label>
          )}
          {filteredGpios.length === 0 ? (
            <p className="rounded-lg bg-panel2/40 px-3 py-4 text-center text-xs text-ink-dim">{t("gpio.filter.empty")}</p>
          ) : (
            <ul className={compact ? "min-h-0 overflow-y-auto" : "min-h-0 space-y-2.5 overflow-y-auto pr-1"}>
              {filteredGpios.slice(0, limit).map((g) => (
                <GpioRow key={g.name + g.pin} gpio={g} onApply={onSet} disabled={disabled} stale={stale} compact={compact} />
              ))}
            </ul>
          )}
        </>
      )}
      {compact && gpios.length > 0 && (
        <div className="mt-2 flex flex-wrap items-center justify-between gap-2">
          <p className="text-[11px] text-ink-dim">
            {t("quick.managePins", { shown: Math.min(limit ?? gpios.length, gpios.length), total: gpios.length })}
          </p>
          {onOpenDetails && (
            <Button variant="ghost" className="min-h-7 px-0 py-1 text-[11px] text-brand hover:bg-transparent" onClick={onOpenDetails}>
              {t("quick.manageControls")}
            </Button>
          )}
        </div>
      )}
    </Card>
  );
}
