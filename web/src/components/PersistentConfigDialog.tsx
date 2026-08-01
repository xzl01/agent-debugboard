import { useEffect, useRef } from "react";
import { createPortal } from "react-dom";
import { Loader2, ShieldAlert, TriangleAlert } from "lucide-react";
import { Button } from "./ui";
import { useI18n } from "@/lib/i18n";

const FOCUSABLE_SELECTOR = [
  "button:not([disabled])",
  "[href]",
  'input:not([disabled]):not([type="hidden"])',
  "select:not([disabled])",
  "textarea:not([disabled])",
  '[tabindex]:not([tabindex="-1"])',
].join(", ");

export type PersistentConfigConfirmationKind = "save" | "apply" | "clear";

export function PersistentConfigDialog({
  kind,
  ids,
  busy,
  opener,
  onCancel,
  onConfirm,
}: {
  readonly kind: PersistentConfigConfirmationKind;
  readonly ids: readonly string[];
  readonly busy: boolean;
  readonly opener: HTMLElement;
  readonly onCancel: () => void;
  readonly onConfirm: () => void;
}) {
  const { t } = useI18n();
  const dialogRef = useRef<HTMLDialogElement>(null);
  const cancelRef = useRef<HTMLButtonElement>(null);
  const busyRef = useRef(busy);
  const cancelHandlerRef = useRef(onCancel);

  useEffect(() => {
    busyRef.current = busy;
    cancelHandlerRef.current = onCancel;
  }, [busy, onCancel]);

  useEffect(() => {
    const dialog = dialogRef.current;
    const previousOverflow = document.body.style.overflow;
    document.body.style.overflow = "hidden";
    cancelRef.current?.focus();

    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape" && !busyRef.current) {
        event.preventDefault();
        cancelHandlerRef.current();
        return;
      }
      if (event.key !== "Tab" || !dialog) return;
      const focusable = [...dialog.querySelectorAll<HTMLElement>(FOCUSABLE_SELECTOR)];
      const first = focusable[0];
      const last = focusable[focusable.length - 1];
      if (!first || !last) return;
      if (event.shiftKey && document.activeElement === first) {
        event.preventDefault();
        last.focus();
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault();
        first.focus();
      }
    };

    window.addEventListener("keydown", handleKeyDown);
    return () => {
      window.removeEventListener("keydown", handleKeyDown);
      document.body.style.overflow = previousOverflow;
      opener.focus();
    };
  }, [opener]);

  const confirmLabel = t(`config.dialog.${kind}.confirm`);
  return createPortal(
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-terminal/70 px-4 py-6">
      <dialog
        open
        ref={dialogRef}
        aria-modal="true"
        aria-labelledby={`persistent-config-${kind}-title`}
        aria-describedby={`persistent-config-${kind}-description`}
        aria-busy={busy}
        className="relative m-0 max-h-[calc(100dvh-3rem)] w-full max-w-lg overflow-y-auto rounded-xl border border-line/80 bg-panel p-0 shadow-2xl"
      >
        <header className="flex items-start gap-3 border-b border-line/60 px-4 py-3">
          <span className={`grid h-9 w-9 shrink-0 place-items-center rounded-xl ${kind === "clear" ? "bg-warn/15 text-warn" : "bg-danger/15 text-danger"}`}>
            {kind === "clear" ? <TriangleAlert size={17} /> : <ShieldAlert size={17} />}
          </span>
          <div className="min-w-0">
            <h2 id={`persistent-config-${kind}-title`} className="text-sm font-semibold text-ink">
              {t(`config.dialog.${kind}.title`)}
            </h2>
            <p id={`persistent-config-${kind}-description`} className="mt-1 text-xs leading-relaxed text-ink-dim">
              {t(`config.dialog.${kind}.body`)}
            </p>
          </div>
        </header>

        <div className="px-4 py-4">
          {ids.length > 0 && (
            <div className="rounded-lg border border-danger/30 bg-danger/10 px-3 py-2">
              <div className="text-xs font-medium text-danger">{t("config.dialog.dangerousIds")}</div>
              <ul className="mt-2 space-y-1" aria-label={t("config.dialog.dangerousIds")}>
                {ids.map((id) => (
                  <li key={id} className="break-all font-mono text-xs text-danger">{id}</li>
                ))}
              </ul>
            </div>
          )}
          <div className="mt-4 flex flex-col-reverse gap-2 sm:flex-row sm:justify-end">
            <Button ref={cancelRef} type="button" className="min-h-11" onClick={onCancel} disabled={busy}>
              {t("config.dialog.cancel")}
            </Button>
            <Button type="button" variant="danger" className="min-h-11" onClick={onConfirm} disabled={busy}>
              {busy && <Loader2 size={15} className="animate-spin" />}
              {confirmLabel}
            </Button>
          </div>
        </div>
      </dialog>
    </div>,
    document.body
  );
}
