import {
  defaultScript,
  parseTestScript,
  serializeTestScript,
  type TestScript,
} from "./testScript.ts";

const STORAGE_KEY = "linkr-test-script";

export function loadStoredTestScript(): TestScript {
  try {
    const saved = localStorage.getItem(STORAGE_KEY);
    if (saved) return parseTestScript(saved, { validatePlan: false });
  } catch { /* ignore corrupted data */ }
  return defaultScript();
}

export function persistTestScript(script: TestScript): void {
  try {
    // Persist drafts even when the execution plan is temporarily invalid
    // (e.g. empty loop body while composing). Run still validates strictly.
    localStorage.setItem(STORAGE_KEY, serializeTestScript(script));
  } catch {
    // Keep the editor usable when storage is unavailable or full.
  }
}
