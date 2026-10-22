import {
  defaultScript,
  parseTestScriptDraft,
  serializeTestScriptDraft,
  type TestScript,
} from "./testScript.ts";

const STORAGE_KEY = "linkr-test-script";

export function loadStoredTestScript(): TestScript {
  const saved = localStorage.getItem(STORAGE_KEY);
  return saved === null ? defaultScript() : parseTestScriptDraft(saved);
}

export function persistTestScript(script: TestScript): void {
  // Never replace/drop an invalid intermediate editor value via strict serialization.
  // Actual storage/schema errors propagate to the UI; the in-memory draft stays owned there.
  localStorage.setItem(STORAGE_KEY, serializeTestScriptDraft(script));
}
