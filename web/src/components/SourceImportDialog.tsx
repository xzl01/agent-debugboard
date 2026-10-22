import { useCallback, useRef, useState } from "react";
import { useI18n } from "@/lib/i18n";
import { Button, Dialog } from "./ui";

export interface ImportEntry<T> { readonly id: string; readonly label: string; readonly detail: string; readonly value: T }

/** Parsing and previewing never mutates the editor or dispatches a device request. */
export function SourceImportDialog<T>({ title, description, parse, onImport, onClose, allowAll = false }: {
  title: string; description: string;
  parse: (text: string) => readonly ImportEntry<T>[];
  onImport: (entry: ImportEntry<T> | null, text: string) => Promise<void> | void;
  onClose: () => void; allowAll?: boolean;
}) {
  const { t } = useI18n();
  const [text, setText] = useState("");
  const [entries, setEntries] = useState<readonly ImportEntry<T>[] | null>(null);
  const [selected, setSelected] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const fileRef = useRef<HTMLInputElement>(null);
  const update = (value: string) => { setText(value); setEntries(null); setSelected(""); setError(null); };
  const close = useCallback(() => { if (!busy) onClose(); }, [busy, onClose]);
  const preview = () => {
    try { const rows = parse(text); if (!rows.length) throw new Error(t("import.empty")); setEntries(rows); setSelected(rows.length === 1 ? `entry:${rows[0].id}` : ""); setError(null); }
    catch (cause) { setEntries(null); setError(cause instanceof Error ? cause.message : String(cause)); }
  };
  const apply = async () => {
    if (!entries || !selected || busy) return;
    setBusy(true); setError(null);
    try { await onImport(selected === "all" ? null : entries.find((entry) => `entry:${entry.id}` === selected)!, text); onClose(); }
    catch (cause) { setError(cause instanceof Error ? cause.message : String(cause)); }
    finally { setBusy(false); }
  };
  return <Dialog title={title} description={description} closeLabel={t("library.cancel")} onClose={close}>
    <div className="min-w-0 space-y-3">
      <Button disabled={busy} onClick={() => fileRef.current?.click()}>{t("import.file")}</Button>
      <input ref={fileRef} aria-label={t("import.file")} className="hidden" type="file" accept=".ndjson,.jsonl,.json,text/plain" onChange={(event) => {
        const file = event.target.files?.[0]; if (!file) return;
        const reader = new FileReader(); reader.onload = () => update(String(reader.result)); reader.onerror = () => setError(t("test.workflow.importReadError")); reader.readAsText(file); event.target.value = "";
      }} />
      <label className="grid min-w-0 gap-1.5 text-xs">{t("import.text")}<textarea aria-label={t("import.text")} disabled={busy} value={text} onChange={(event) => update(event.target.value)} spellCheck={false} className="min-h-40 w-full min-w-0 resize-y rounded-lg border border-line bg-panel2 p-3 font-mono text-xs [overflow-wrap:anywhere]" /></label>
      <Button disabled={busy || !text.trim()} onClick={preview}>{t("import.preview")}</Button>
      {entries && <fieldset className="min-w-0 space-y-2 rounded-lg border border-line p-3"><legend className="px-1 text-xs">{t("import.choose")}</legend>
        {entries.map((entry) => <label key={entry.id} className="flex min-w-0 items-start gap-2 rounded-lg bg-panel2 p-2 text-sm"><input type="radio" name="import-entry" value={entry.id} checked={selected === `entry:${entry.id}`} disabled={busy} onChange={() => setSelected(`entry:${entry.id}`)} /><span className="min-w-0 break-all">{entry.label}<span className="mt-1 block text-xs text-ink-dim">{entry.detail}</span></span></label>)}
        {allowAll && <label className="flex items-start gap-2 text-sm text-warn"><input type="radio" name="import-entry" checked={selected === "all"} disabled={busy} onChange={() => setSelected("all")} />{t("import.replaceAll")}</label>}
      </fieldset>}
      {error && <p role="alert" className="break-words text-xs text-danger">{error}</p>}
      <div className="flex flex-wrap justify-end gap-2"><Button disabled={busy} onClick={close}>{t("library.cancel")}</Button><Button variant="primary" disabled={busy || !entries || !selected} onClick={() => void apply()}>{t("import.confirm")}</Button></div>
    </div>
  </Dialog>;
}
