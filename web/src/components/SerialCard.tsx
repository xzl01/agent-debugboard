import { forwardRef, useEffect, useImperativeHandle } from "react";
import { SquareTerminal } from "lucide-react";
import { SerialTerminalPane } from "./SerialTerminalPane";
import { useI18n } from "@/lib/i18n";
import { useJ4SerialSnapshot } from "@/lib/j4SerialReact";
import { j4ConnectionState, type J4SerialSession, type J4SerialConnectionState, type SerialAutomationHandle } from "@/lib/j4SerialSession";

export type { J4SerialChannel as SerialChannelId } from "@/lib/j4SerialProtocol";
export type { SerialAutomationHandle } from "@/lib/j4SerialSession";

export interface SerialConnectionSummary {
  readonly j4: boolean;
  readonly state: J4SerialConnectionState;
}

export const SerialCard = forwardRef<SerialAutomationHandle, {
  session: J4SerialSession;
  onConnectionChange?: (connections: SerialConnectionSummary) => void;
}>(function SerialCard({ session, onConnectionChange }, ref) {
  const { t } = useI18n();
  const snapshot = useJ4SerialSnapshot(session);
  const state = j4ConnectionState(snapshot);
  const connected = snapshot.phase === "connected";
  useEffect(() => { onConnectionChange?.({ j4: connected, state }); }, [connected, state, onConnectionChange]);
  useImperativeHandle(ref, () => session, [session]);

  return (
    <section data-testid="serial-workspace" className="flex min-h-0 flex-col gap-3 xl:h-[832px]">
      <header className="flex flex-wrap items-center gap-3 rounded-xl border border-line/70 bg-panel px-4 py-3">
        <SquareTerminal size={18} className="shrink-0 text-brand" aria-hidden="true" />
        <div className="min-w-0">
          <h2 className="text-sm font-semibold text-ink">{t("serial.title")}</h2>
          <p className="mt-0.5 text-xs text-ink-dim">{t("serial.subtitle")}</p>
        </div>
        <span className="ml-auto rounded-md border border-line/70 bg-panel2 px-2 py-1 font-mono text-[10px] text-ink-dim">J4 · MCU</span>
      </header>
      <SerialTerminalPane session={session} fillHeight />
    </section>
  );
});
