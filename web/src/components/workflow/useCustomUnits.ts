import { useCallback, useEffect, useRef, useState } from "react";
import type { CustomUnitTemplate } from "./types";
import { CUSTOM_UNITS_KEY } from "./types";
import { loadCustomUnits } from "./utils";

const SAVE_DELAY_MS = 200;

function saveCustomUnits(customUnits: CustomUnitTemplate[]) {
  try {
    localStorage.setItem(CUSTOM_UNITS_KEY, JSON.stringify(customUnits));
  } catch {
    // Storage may be disabled or full. The in-memory editor remains usable.
  }
}

export function useCustomUnits() {
  const [customUnits, setCustomUnits] = useState<CustomUnitTemplate[]>(loadCustomUnits);
  const latestRef = useRef(customUnits);
  latestRef.current = customUnits;

  useEffect(() => {
    const timer = globalThis.setTimeout(() => saveCustomUnits(customUnits), SAVE_DELAY_MS);
    return () => globalThis.clearTimeout(timer);
  }, [customUnits]);

  useEffect(() => () => saveCustomUnits(latestRef.current), []);

  const addCustomUnitTemplate = useCallback((template: CustomUnitTemplate) => {
    setCustomUnits((current) => [...current, template]);
  }, []);

  const removeCustomUnit = useCallback((id: string) => {
    setCustomUnits((current) => current.filter((unit) => unit.id !== id));
  }, []);

  return { customUnits, addCustomUnitTemplate, removeCustomUnit };
}
