import { useState } from "react";
import { Pin } from "lucide-react";
import { Badge, Button, Card } from "./ui";
import type { SafeGpio } from "@/lib/types";
import { useI18n } from "@/lib/i18n";

function GpioRow({
  gpio,
  onApply,
}: {
  gpio: SafeGpio;
  onApply: (identifier: string, direction: "input" | "output", value?: number) => void;
}) {
  const { t } = useI18n();
  const [direction, setDirection] = useState<"input" | "output">(
    gpio.direction === "output" ? "output" : "input"
  );
  const [value, setValue] = useState(gpio.value > 0);

  return (
    <li className="rounded-lg border border-line/50 bg-panel2/40 px-3 py-2.5">
      <div className="flex flex-wrap items-start justify-between gap-3">
        <div className="min-w-0">
          <div className="flex items-center gap-2">
            <span className="font-medium text-ink">{gpio.name}</span>
            <Badge tone="neutral">GP{gpio.pin}</Badge>
          </div>
          <div className="text-xs text-ink-dim break-words">{gpio.note}</div>
        </div>
        <div className="flex max-w-full flex-wrap items-center gap-2">
          <select
            value={direction}
            onChange={(e) => setDirection(e.target.value as "input" | "output")}
            className="rounded-md border border-line/70 bg-panel2 px-2 py-1 text-xs text-ink outline-none"
          >
            <option value="input">{t("gpio.input")}</option>
            <option value="output">{t("gpio.output")}</option>
          </select>
          {direction === "output" && (
            <select
              value={value ? "1" : "0"}
              onChange={(e) => setValue(e.target.value === "1")}
              className="rounded-md border border-line/70 bg-panel2 px-2 py-1 text-xs text-ink outline-none"
            >
              <option value="1">{t("gpio.high")}</option>
              <option value="0">{t("gpio.low")}</option>
            </select>
          )}
          <Button
            variant="ghost"
            className="px-2 py-1"
            onClick={() => onApply(gpio.name, direction, value ? 1 : 0)}
          >
            {t("gpio.set")}
          </Button>
        </div>
      </div>
      <div className="mt-1 text-[11px] text-ink-dim">
        {t("gpio.current")}: {direction} · {gpio.value > 0 ? t("gpio.high") : t("gpio.low")}
      </div>
    </li>
  );
}

export function GpioCard({
  gpios,
  onSet,
}: {
  gpios: SafeGpio[];
  onSet: (identifier: string, direction: "input" | "output", value?: number) => void;
}) {
  const { t } = useI18n();
  return (
    <Card
      title={t("gpio.title")}
      subtitle={t("gpio.subtitle")}
      icon={Pin}
      className="max-h-[440px]"
      contentClassName="flex min-h-0 flex-col"
    >
      {gpios.length === 0 ? (
        <p className="text-sm text-ink-dim">{t("gpio.none")}</p>
      ) : (
        <ul className="min-h-0 space-y-2.5 overflow-y-auto pr-1">
          {gpios.map((g) => (
            <GpioRow key={g.name + g.pin} gpio={g} onApply={onSet} />
          ))}
        </ul>
      )}
    </Card>
  );
}
