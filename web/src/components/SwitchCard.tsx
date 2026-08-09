import { ArrowLeftRight } from "lucide-react";
import { Badge, Card } from "./ui";
import type { SwitchState } from "@/lib/types";
import { switchDescLabel, switchNameLabel, switchRouteLabel } from "@/lib/switches";
import { cn } from "@/lib/utils";
import { useI18n } from "@/lib/i18n";

function Segmented({
  value,
  options,
  onChange,
  disabled,
}: {
  value: string;
  options: { value: string; label: string }[];
  onChange: (v: string) => void;
  disabled?: boolean;
}) {
  return (
    <div className="grid w-full grid-cols-2 rounded-lg border border-line/70 bg-panel2/60 p-0.5 sm:w-auto">
      {options.map((opt) => (
        <button
          key={opt.value}
          disabled={disabled}
          onClick={() => onChange(opt.value)}
          className={cn(
            "min-h-9 rounded-md px-3 py-1 text-xs font-medium transition-colors",
            "disabled:cursor-not-allowed disabled:opacity-50",
            value === opt.value
              ? "bg-brand text-on-brand"
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
  onSet: (name: string, route: string) => void;
}) {
  const { t } = useI18n();
  const names = Object.keys(switches).sort();

  const handleSet = (name: string, route: string, requiresConfirm?: boolean) => {
    if (requiresConfirm && !window.confirm(t("switch.confirm", { name, route }))) return;
    onSet(name, route);
  };

  return (
    <Card title={t("switch.title")} subtitle={t("switch.subtitle")} icon={ArrowLeftRight}>
      <div className="space-y-4">
        {names.map((name) => {
          const sw = switches[name];
          const routes = sw.routes ?? (sw.route ? [sw.route] : []);
          const desc = switchDescLabel(t, name);
          return (
            <div
              key={name}
              className="grid gap-2 sm:grid-cols-[minmax(0,1fr)_auto] sm:items-center"
            >
              <div>
                <div className="font-medium text-ink">{switchNameLabel(t, name)}</div>
                {desc && <div className="text-xs text-ink-dim">{desc}</div>}
              </div>
              <Segmented
                value={sw.route}
                options={routes.map((route) => ({
                  value: route,
                  label: switchRouteLabel(t, route),
                }))}
                onChange={(v) => handleSet(name, v, sw.requires_confirm)}
                disabled={!sw.routes || sw.routes.length === 0}
              />
            </div>
          );
        })}

        <div className="flex flex-wrap gap-2 pt-1">
          {names.map((name) => (
            <Badge key={name} tone="brand">
              {name}: {switches[name].route || "—"}
            </Badge>
          ))}
        </div>
      </div>
    </Card>
  );
}
