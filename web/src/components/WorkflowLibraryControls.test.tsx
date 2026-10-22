import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { IDBFactory } from "fake-indexeddb";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { LanguageProvider } from "@/lib/i18n";
import { defaultScript, parseTestScriptDraft, serializeTestScript, serializeTestScriptDraft, type TestScript } from "@/lib/testScript";
import { workflowLibrary, WorkflowStorageError } from "@/lib/workflowLibrary";
import { useWorkflowEditor, WorkflowLibraryControls } from "./WorkflowLibraryControls";
import { WorkflowComposer } from "./WorkflowComposer";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let editor: ReturnType<typeof useWorkflowEditor>;
let root: Root | null = null;
let host: HTMLDivElement;
function Harness({ composer = false }: { composer?: boolean }) {
  editor = useWorkflowEditor("New workflow");
  return <><WorkflowLibraryControls editor={editor} />{composer && <WorkflowComposer key={editor.documentEpoch} script={editor.script} onChange={editor.change} onImport={editor.importWorkflow} onRun={vi.fn()} />}</>;
}
async function flush() { await act(async () => { await new Promise((resolve) => setTimeout(resolve, 5)); }); }
async function until(check: () => boolean) { for (let i = 0; i < 100 && !check(); i++) await flush(); expect(check()).toBe(true); }
async function mount(composer = false) {
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
  await act(async () => { root!.render(<LanguageProvider><Harness composer={composer} /></LanguageProvider>); });
  await flush();
}
function button(text: string, parent: ParentNode = document.body) {
  const found = [...parent.querySelectorAll<HTMLButtonElement>("button")].find((item) => item.textContent?.trim() === text);
  if (!found) throw new Error(`Missing button ${text}`); return found;
}
function input(element: HTMLInputElement | HTMLTextAreaElement, value: string) {
  act(() => { Object.getOwnPropertyDescriptor(element instanceof HTMLTextAreaElement ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype, "value")!.set!.call(element, value); element.dispatchEvent(new Event("input", { bubbles: true })); });
}
async function save(name: string) {
  act(() => editor.change({ ...defaultScript(name), steps: [{ id: "power", type: "power_on", params: { rail: "5v_out" } }] }));
  await act(async () => { expect(await editor.save()).toBe(true); });
  return editor.activeId!;
}
beforeEach(async () => { await workflowLibrary.close(); vi.stubGlobal("indexedDB", new IDBFactory()); localStorage.clear(); localStorage.setItem("lang", "en"); });
afterEach(async () => { act(() => root?.unmount()); root = null; document.body.replaceChildren(); await workflowLibrary.close(); vi.restoreAllMocks(); vi.unstubAllGlobals(); localStorage.clear(); });

describe("one canvas and a lossless workflow library", () => {
  it("saves A/B with stable independent identities and reopens exact source after remount", async () => {
    await mount(); const a = await save("A"); const sourceA = serializeTestScript(editor.script);
    act(() => editor.newWorkflow()); const b = await save("B");
    expect(a).not.toBe(b); expect(editor.workflows.map((row) => row.name).sort()).toEqual(["A", "B"]);
    act(() => root!.unmount()); root = null; host.remove(); await workflowLibrary.close();
    await mount(); await until(() => editor.workflows.length === 2);
    expect(editor.activeId).toBe(b); expect(editor.dirty).toBe(false);
    act(() => editor.open(editor.workflows.find((row) => row.workflow_id === a)!));
    await until(() => editor.activeId === a);
    expect(serializeTestScript(editor.script)).toBe(sourceA);
    expect(editor.draftState).toBe("saved"); expect(editor.dirty).toBe(false);
  });
  it("cancels a dirty switch, then saves before continuing without losing either document", async () => {
    await mount(); const a = await save("A"); act(() => editor.newWorkflow()); const b = await save("B");
    act(() => editor.open(editor.workflows.find((row) => row.workflow_id === a)!)); await until(() => editor.activeId === a);
    act(() => editor.change({ ...editor.script, name: "A edited" }));
    expect(editor.draftState).toBe("saved"); expect(editor.dirty).toBe(true);
    act(() => editor.open(editor.workflows.find((row) => row.workflow_id === b)!));
    const dialog = document.querySelector('[role="dialog"]')!;
    act(() => button("Cancel", dialog).click());
    expect(editor.activeId).toBe(a); expect(editor.script.name).toBe("A edited");
    act(() => editor.open(editor.workflows.find((row) => row.workflow_id === b)!));
    act(() => button("Save to library", document.querySelector('[role="dialog"]')!).click());
    await until(() => editor.activeId === b);
    expect((await workflowLibrary.getWorkflow(a))?.name).toBe("A edited");
  });
  it("save-as cancellation is inert, while confirming creates a new independent entry", async () => {
    await mount(); const a = await save("A"); act(() => editor.saveAs());
    input(document.querySelector<HTMLInputElement>('[aria-label="New workflow name"]')!, "Copy");
    act(() => button("Cancel", document.querySelector('[role="dialog"]')!).click());
    expect(editor.activeId).toBe(a); expect(editor.workflows).toHaveLength(1);
    act(() => editor.saveAs()); input(document.querySelector<HTMLInputElement>('[aria-label="New workflow name"]')!, "Copy");
    act(() => button("Save as new workflow", document.querySelector('[role="dialog"]')!).click());
    await until(() => editor.workflows.length === 2);
    expect(editor.activeId).not.toBe(a); expect((await workflowLibrary.getWorkflow(a))?.name).toBe("A");
  });
  it("shows quota failure, retains dirty draft and never continues a pending replacement", async () => {
    await mount(); await save("A"); act(() => editor.change({ ...editor.script, name: "Unsaved" }));
    vi.spyOn(workflowLibrary, "saveWorkflow").mockRejectedValue(new WorkflowStorageError("storage_quota", "quota fixture"));
    act(() => editor.newWorkflow()); act(() => button("Save to library", document.querySelector('[role="dialog"]')!).click());
    await until(() => !editor.busy);
    expect(editor.script.name).toBe("Unsaved"); expect(editor.dirty).toBe(true);
    expect(document.querySelector('[role="dialog"]')?.textContent).toContain("Browser storage is full");
    expect(editor.workflows[0].name).toBe("A");
  });
  it("validates file/text workflow import before touching the current draft", async () => {
    await mount(true); await save("Keep me"); const before = serializeTestScript(editor.script);
    act(() => button("Import workflow", host).click());
    input(document.querySelector<HTMLTextAreaElement>("textarea")!, "# linkr-task.v1\n# task wrong-format\n");
    act(() => button("Preview import").click());
    expect(document.querySelector('[role="dialog"] [role="alert"]')).not.toBeNull();
    expect(button("Import selected — do not run").disabled).toBe(true);
    expect(serializeTestScript(editor.script)).toBe(before);
    expect(document.querySelector<HTMLInputElement>('input[type="file"]')).not.toBeNull();
  });
  it("reads a workflow file into preview and only replaces the draft after confirmation", async () => {
    await mount(true); const originalId = await save("Original");
    act(() => button("Import workflow", host).click());
    const fileInput = document.querySelector<HTMLInputElement>('input[type="file"]')!;
    const incoming = defaultScript("From file");
    Object.defineProperty(fileInput, "files", { value: [new File([serializeTestScript(incoming)], "workflow.ndjson")] });
    act(() => fileInput.dispatchEvent(new Event("change", { bubbles: true })));
    await until(() => document.querySelector<HTMLTextAreaElement>("textarea")?.value.includes("From file") === true);
    expect(editor.script.name).toBe("Original");
    act(() => button("Preview import").click()); act(() => button("Import selected — do not run").click());
    await until(() => editor.script.name === "From file");
    expect(editor.activeId).toBeNull(); expect(editor.dirty).toBe(true);
    expect((await workflowLibrary.getWorkflow(originalId))?.name).toBe("Original");
  });
  it("cancels replacement of a dirty draft and preserves the saved baseline", async () => {
    await mount(); const id = await save("Saved"); act(() => editor.change({ ...editor.script, name: "Dirty" }));
    act(() => editor.importWorkflow(defaultScript("Imported")));
    act(() => button("Cancel", document.querySelector('[role="dialog"]')!).click());
    expect(editor.script.name).toBe("Dirty"); expect(editor.activeId).toBe(id); expect(editor.dirty).toBe(true);
    expect((await workflowLibrary.getWorkflow(id))?.name).toBe("Saved");
  });
  it("reports unavailable IndexedDB without preventing editing or claiming a library save", async () => {
    vi.stubGlobal("indexedDB", undefined); await mount(); await until(() => editor.error !== null);
    act(() => editor.change(defaultScript("Keep editing")));
    await act(async () => { expect(await editor.save()).toBe(false); });
    expect(editor.script.name).toBe("Keep editing"); expect(editor.dirty).toBe(true);
    expect(host.textContent).toContain("Workflow storage is unavailable"); expect(editor.workflows).toHaveLength(0);
  });
  it("deletes only a confirmed library entry and keeps the editing buffer", async () => {
    await mount(); const id = await save("Delete me"); const entry = editor.workflows[0];
    vi.spyOn(window, "confirm").mockReturnValue(false);
    await act(async () => editor.remove(entry)); expect(await workflowLibrary.getWorkflow(id)).not.toBeNull();
    vi.mocked(window.confirm).mockReturnValue(true);
    await act(async () => editor.remove(entry)); expect(await workflowLibrary.getWorkflow(id)).toBeNull();
    expect(editor.script.name).toBe("Delete me"); expect(editor.activeId).toBeNull(); expect(editor.dirty).toBe(true);
  });
  it.each(["delay", "current_range"] as const)("keeps real invalid %s input mounted and draft-persisted while strict save/save-as reject it", async (kind) => {
    await mount(true);
    const valid: TestScript = { ...defaultScript("Editable"), steps: kind === "delay"
      ? [{ id: "delay", type: "delay", params: { ms: 10 } }]
      : [{ id: "adc", type: "adc_read", params: { channel: "5v_out" }, assert: { current_range: { min_a: 0, max_a: 3 } } }] };
    act(() => editor.replace(valid)); await act(async () => { expect(await editor.save()).toBe(true); });
    const id = editor.activeId!;
    const baseline = (await workflowLibrary.getWorkflow(id))!.source;
    input(host.querySelector<HTMLInputElement>(kind === "delay" ? '[aria-label="ms"]' : '[aria-label="Current range"]')!, kind === "delay" ? "-1" : "{}");
    expect(host.querySelector('[data-testid="workflow-library"]')).not.toBeNull();
    expect(host.querySelector('[data-testid="workflow-validation"]')?.textContent).toContain("Draft needs correction");
    expect(editor.dirty).toBe(true); expect(editor.draftState).toBe("saved");
    const draft = localStorage.getItem("linkr-test-script")!;
    expect(serializeTestScriptDraft(parseTestScriptDraft(draft))).toBe(draft);
    await act(async () => { expect(await editor.save()).toBe(false); });
    expect((await workflowLibrary.getWorkflow(id))!.source).toBe(baseline);
    act(() => editor.saveAs()); act(() => button("Save as new workflow", document.querySelector('[role="dialog"]')!).click());
    await until(() => !editor.busy);
    expect(editor.workflows).toHaveLength(1); expect(editor.activeId).toBe(id); expect(editor.dirty).toBe(true);
    expect(document.querySelector('[role="dialog"]')?.textContent).toContain("Draft needs correction");
    act(() => button("Cancel", document.querySelector('[role="dialog"]')!).click());
    act(() => root!.unmount()); root = null; host.remove(); await mount(true);
    expect(editor.activeId).toBe(id); expect(editor.dirty).toBe(true); expect(editor.validationError).not.toBeNull();
    expect(localStorage.getItem("linkr-test-script")).toBe(draft);
    input(host.querySelector<HTMLInputElement>(kind === "delay" ? '[aria-label="ms"]' : '[aria-label="Current range"]')!, kind === "delay" ? "20" : '{"min_a":0,"max_a":2}');
    expect(editor.validationError).toBeNull(); await act(async () => { expect(await editor.save()).toBe(true); });
    expect(editor.dirty).toBe(false);
  });
  it("cancels a delayed library open if real editing changes the draft before the read resolves", async () => {
    await mount(true); const a = await save("A"); const sourceA = await workflowLibrary.getWorkflow(a);
    act(() => editor.newWorkflow()); const b = await save("B");
    let resolve!: (record: typeof sourceA) => void;
    vi.spyOn(workflowLibrary, "getWorkflow").mockImplementationOnce(() => new Promise((done) => { resolve = done; }));
    act(() => editor.open(editor.workflows.find((record) => record.workflow_id === a)!));
    input(host.querySelector<HTMLInputElement>('[aria-label="Test Name"]')!, "B changed while reading");
    await act(async () => { resolve(sourceA); });
    expect(editor.activeId).toBe(b); expect(editor.script.name).toBe("B changed while reading"); expect(editor.dirty).toBe(true);
    expect(editor.error).toContain("Opening was cancelled");
    expect((await workflowLibrary.getWorkflow(b))?.name).toBe("B");
  });
  it.each(["step IDs", "JSON fields"])("rejects duplicate import %s without replacing even a clean saved draft", async (duplicate) => {
    await mount(true); const id = await save("Keep original"); const original = serializeTestScript(editor.script);
    const header = JSON.stringify({ schema: "linkr-test.v1", name: "Bad import", version: "1.0", created: "2026-10-22T00:00:00.000Z" });
    const step = '{"id":"same","type":"delay","params":{"ms":10}}';
    const invalid = duplicate === "step IDs" ? `${header}\n${step}\n${step}\n` : `${header}\n{"id":"d","type":"delay","params":{"ms":10,"ms":20}}\n`;
    act(() => button("Import workflow", host).click()); input(document.querySelector<HTMLTextAreaElement>("textarea")!, invalid);
    act(() => button("Preview import").click());
    expect(document.querySelector('[role="dialog"] [role="alert"]')).not.toBeNull();
    expect(button("Import selected — do not run").disabled).toBe(true);
    expect(editor.activeId).toBe(id); expect(serializeTestScript(editor.script)).toBe(original); expect(editor.dirty).toBe(false);
  });
});
