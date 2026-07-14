import { ArrowLeftRight } from "lucide-react";
import { Badge, Card } from "./ui";
import type { SwitchState } from "@/lib/types";
import { cn } from "@/lib/utils";
import { useI18n } from "@/lib/i18n";

function Segmented({
  value,
  options,
  onChange,
}: {
  value: string;
  options: { value: string; label: string }[];
  onChange: (v: string) => void;
}) {
  return (
    <div className="grid w-full grid-cols-2 rounded-lg border border-line/70 bg-panel2/60 p-0.5 sm:w-auto">
      {options.map((opt) => (
        <button
          key={opt.value}
          onClick={() => onChange(opt.value)}
          className={cn(
            "min-h-9 rounded-md px-3 py-1 text-xs font-medium transition-colors",
            value === opt.value
              ? "bg-brand text-white"
              : "text-ink-dim hover:text-ink"
          )}
        >
          {opt.label}
        </button>
      ))}
    </div>
  );
}

export function SwitchCard({
  switches,
  onSet,
}: {
  switches: SwitchState;
  onSet: (name: "sd" | "usb", route: string) => void;
}) {
  const { t } = useI18n();
  return (
    <Card title={t("switch.title")} subtitle={t("switch.subtitle")} icon={ArrowLeftRight}>
      <div className="space-y-4">
        <div className="grid gap-2 sm:grid-cols-[minmax(0,1fr)_auto] sm:items-center">
          <div>
            <div className="font-medium text-ink">{t("switch.sd")}</div>
            <div className="text-xs text-ink-dim">{t("switch.sd.desc")}</div>
          </div>
          <Segmented
            value={switches.sd}
            options={[
              { value: "target", label: t("switch.route.sbc") },
              { value: "usb-reader", label: t("switch.route.pc") },
            ]}
            onChange={(v) => onSet("sd", v)}
          />
        </div>

        <div className="grid gap-2 sm:grid-cols-[minmax(0,1fr)_auto] sm:items-center">
          <div>
            <div className="font-medium text-ink">{t("switch.usb")}</div>
            <div className="text-xs text-ink-dim">{t("switch.usb.desc")}</div>
          </div>
          <Segmented
            value={switches.usb}
            options={[
              { value: "target", label: t("switch.route.sbc") },
              { value: "pc", label: t("switch.route.pc") },
            ]}
            onChange={(v) => onSet("usb", v)}
          />
        </div>

        <div className="flex flex-wrap gap-2 pt-1">
          <Badge tone="brand">sd: {switches.sd || "—"}</Badge>
          <Badge tone="brand">usb: {switches.usb || "—"}</Badge>
        </div>
      </div>
    </Card>
  );
}
