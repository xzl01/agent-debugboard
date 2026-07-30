import { useCallback, useRef, useState } from "react";
import type { TestScript } from "./types";

const MAX_HISTORY = 60;

interface HistoryState {
  past: TestScript[];
  future: TestScript[];
}

export function useWorkflowHistory() {
  const [history, setHistory] = useState<HistoryState>({ past: [], future: [] });
  const historyRef = useRef(history);
  historyRef.current = history;

  const push = useCallback((snapshot: TestScript) => {
    setHistory((current) => ({
      past: [...current.past.slice(-(MAX_HISTORY - 1)), snapshot],
      future: [],
    }));
  }, []);

  const undo = useCallback((current: TestScript): TestScript | null => {
    const snapshot = historyRef.current;
    const previous = snapshot.past.at(-1);
    if (!previous) return null;

    setHistory({
      past: snapshot.past.slice(0, -1),
      future: [current, ...snapshot.future].slice(0, MAX_HISTORY),
    });
    return previous;
  }, []);

  const redo = useCallback((current: TestScript): TestScript | null => {
    const snapshot = historyRef.current;
    const next = snapshot.future[0];
    if (!next) return null;

    setHistory({
      past: [...snapshot.past, current].slice(-MAX_HISTORY),
      future: snapshot.future.slice(1),
    });
    return next;
  }, []);

  return {
    push,
    undo,
    redo,
    canUndo: history.past.length > 0,
    canRedo: history.future.length > 0,
  };
}
