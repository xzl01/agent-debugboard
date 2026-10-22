import assert from "node:assert/strict";
import test from "node:test";
import { defaultScript, parseTestScriptDraft } from "./testScript.ts";
import { loadStoredTestScript, persistTestScript } from "./testScriptStorage.ts";

test("local editor storage saves invalid intermediate numbers instead of dropping the latest draft", () => {
  const descriptor = Object.getOwnPropertyDescriptor(globalThis, "localStorage");
  const storage = new Map<string, string>();
  Object.defineProperty(globalThis, "localStorage", { configurable: true, value: { getItem: (key: string) => storage.get(key) ?? null, setItem: (key: string, value: string) => { storage.set(key, value); } } });
  try {
    assert.equal(loadStoredTestScript().steps.length, 0);
    const script = defaultScript("Unsaved changes");
    script.steps = [{ id: "loop", type: "loop", params: { count: NaN, steps: [] } }];
    persistTestScript(script);
    assert.deepEqual(loadStoredTestScript(), script);
    assert.deepEqual(parseTestScriptDraft(storage.get("linkr-test-script")!), script);
    const before = storage.get("linkr-test-script");
    Object.defineProperty(globalThis, "localStorage", { configurable: true, value: { getItem: (key: string) => storage.get(key) ?? null, setItem: () => { throw new DOMException("full", "QuotaExceededError"); } } });
    assert.throws(() => persistTestScript(script), { name: "QuotaExceededError" });
    assert.equal(storage.get("linkr-test-script"), before);
    storage.set("linkr-test-script", "corrupt");
    assert.throws(() => loadStoredTestScript());
    assert.equal(storage.get("linkr-test-script"), "corrupt");
  } finally {
    if (descriptor) Object.defineProperty(globalThis, "localStorage", descriptor);
    else Reflect.deleteProperty(globalThis, "localStorage");
  }
});
