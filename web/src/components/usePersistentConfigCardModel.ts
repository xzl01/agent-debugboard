import { useMemo, useRef, useState, type MouseEvent } from "react";
import type { UsePersistentConfig } from "@/hooks/usePersistentConfig";
import {
  groupPersistentConfigItems,
  PersistentConfigApiError,
} from "@/lib/persistentConfig";
import type { PersistentConfigConfirmationKind } from "./PersistentConfigDialog";

type Confirmation = {
  readonly kind: PersistentConfigConfirmationKind;
  readonly ids: readonly string[];
  readonly selectedIds: readonly string[];
  readonly opener: HTMLElement;
};
type Notice = "saved" | "applied" | "cleared" | "refreshed" | null;

function assertNever(value: never): never {
  throw new TypeError(`Unexpected persistent config action: ${String(value)}`);
}

export function usePersistentConfigCardModel(
  state: UsePersistentConfig,
  connected: boolean
) {
  const [selectionOverrides, setSelectionOverrides] = useState<ReadonlyMap<string, boolean>>(
    () => new Map()
  );
  const [confirmation, setConfirmation] = useState<Confirmation | null>(null);
  const [notice, setNotice] = useState<Notice>(null);
  const requestPendingRef = useRef(false);
  const groups = useMemo(
    () => groupPersistentConfigItems(state.config?.items ?? []),
    [state.config]
  );
  const visibleItems = useMemo(
    () => [...groups.power, ...groups.switch, ...groups.gpio],
    [groups]
  );
  const hasRetryableItem = visibleItems.some(
    (item) => item.applyState === "pending" || item.applyState === "failed"
  );
  const selection = useMemo(() => {
    const selectedIds = new Set<string>();
    const selectedItems: (typeof visibleItems)[number][] = [];
    const dangerousSelectedIds: string[] = [];
    const dangerousReplayIds: string[] = [];
    for (const item of visibleItems) {
      const selected = selectionOverrides.get(item.id) ?? item.selected;
      if (selected) {
        selectedIds.add(item.id);
        selectedItems.push(item);
        if (item.risk === "confirmation_required") dangerousSelectedIds.push(item.id);
      }
      if (item.saved !== null && item.risk === "confirmation_required") {
        dangerousReplayIds.push(item.id);
      }
    }
    return { selectedIds, selectedItems, dangerousSelectedIds, dangerousReplayIds };
  }, [selectionOverrides, visibleItems]);
  const disabled = !connected || state.busy !== null || state.loading;
  const saveDisabled = disabled || selection.selectedIds.size === 0;
  const applyDisabled = disabled || !hasRetryableItem;
  const clearDisabled = disabled || state.config?.snapshot.present !== true;

  const toggle = (id: string) => {
    setSelectionOverrides((previous) => {
      const next = new Map(previous);
      next.set(id, !selection.selectedIds.has(id));
      return next;
    });
    setNotice(null);
  };

  const runSave = async (ids: readonly string[], confirm: boolean, opener: HTMLElement) => {
    if (requestPendingRef.current) return;
    requestPendingRef.current = true;
    setNotice(null);
    try {
      await state.save(ids, confirm);
      setSelectionOverrides(new Map());
      setConfirmation(null);
      setNotice("saved");
    } catch (error) {
      if (!(error instanceof PersistentConfigApiError)) throw error;
      if (error.detail.kind === "confirmation_required") {
        setConfirmation({ kind: "save", ids: error.detail.dangerousIds, selectedIds: ids, opener });
      } else {
        setConfirmation(null);
      }
    } finally {
      requestPendingRef.current = false;
    }
  };

  const runApply = async (confirm: boolean, opener: HTMLElement) => {
    if (requestPendingRef.current) return;
    const selectedIds = [...selection.selectedIds];
    requestPendingRef.current = true;
    setNotice(null);
    try {
      await state.apply(confirm);
      setSelectionOverrides((previous) => {
        const next = new Map(previous);
        for (const id of selectedIds) next.set(id, false);
        return next;
      });
      setConfirmation(null);
      setNotice("applied");
    } catch (error) {
      if (!(error instanceof PersistentConfigApiError)) throw error;
      if (error.detail.kind === "confirmation_required") {
        setConfirmation({ kind: "apply", ids: error.detail.dangerousIds, selectedIds: [], opener });
      } else {
        setConfirmation(null);
      }
    } finally {
      requestPendingRef.current = false;
    }
  };

  const runClear = async () => {
    if (requestPendingRef.current) return;
    requestPendingRef.current = true;
    setNotice(null);
    try {
      await state.clear();
      setSelectionOverrides(new Map());
      setConfirmation(null);
      setNotice("cleared");
    } catch (error) {
      if (!(error instanceof PersistentConfigApiError)) throw error;
      setConfirmation(null);
    } finally {
      requestPendingRef.current = false;
    }
  };

  const runRefresh = async () => {
    if (requestPendingRef.current) return;
    requestPendingRef.current = true;
    setNotice(null);
    try {
      await state.refresh();
      setNotice("refreshed");
    } catch (error) {
      if (!(error instanceof PersistentConfigApiError)) throw error;
    } finally {
      requestPendingRef.current = false;
    }
  };

  const save = (event: MouseEvent<HTMLButtonElement>) => {
    if (saveDisabled) return;
    const ids = selection.selectedItems.map((item) => item.id);
    if (selection.dangerousSelectedIds.length > 0) {
      setConfirmation({ kind: "save", ids: selection.dangerousSelectedIds, selectedIds: ids, opener: event.currentTarget });
    } else {
      void runSave(ids, false, event.currentTarget);
    }
  };

  const apply = (event: MouseEvent<HTMLButtonElement>) => {
    if (applyDisabled) return;
    if (selection.dangerousReplayIds.length > 0) {
      setConfirmation({ kind: "apply", ids: selection.dangerousReplayIds, selectedIds: [], opener: event.currentTarget });
    } else {
      void runApply(false, event.currentTarget);
    }
  };

  const clear = (event: MouseEvent<HTMLButtonElement>) => {
    if (!clearDisabled) {
      setConfirmation({ kind: "clear", ids: [], selectedIds: [], opener: event.currentTarget });
    }
  };

  const confirm = () => {
    if (!confirmation) return;
    const action = confirmation;
    setConfirmation(null);
    switch (action.kind) {
      case "save":
        void runSave(action.selectedIds, true, action.opener);
        return;
      case "apply":
        void runApply(true, action.opener);
        return;
      case "clear":
        void runClear();
        return;
      default:
        return assertNever(action.kind);
    }
  };

  return {
    groups,
    visibleItems,
    selectedIds: selection.selectedIds,
    confirmation,
    notice,
    disabled,
    saveDisabled,
    applyDisabled,
    clearDisabled,
    toggle,
    save,
    apply,
    clear,
    refresh: runRefresh,
    confirm,
    cancelConfirmation: () => setConfirmation(null),
  };
}
