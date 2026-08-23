import { act } from "react";
import { createRoot } from "react-dom/client";
import { afterEach, describe, expect, it } from "vitest";
import { LanguageProvider, useI18n } from "@/lib/i18n";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

type Translate = (key: string, params?: Record<string, string | number>) => string;

function captureTranslate(lang: "en" | "zh"): { readonly t: Translate; readonly close: () => void } {
  localStorage.setItem("lang", lang);
  let captured: Translate | null = null;
  function Probe() {
    captured = useI18n().t;
    return null;
  }
  const host = document.createElement("div");
  document.body.append(host);
  const root = createRoot(host);
  act(() => {
    root.render(
      <LanguageProvider>
        <Probe />
      </LanguageProvider>,
    );
  });
  if (!captured) throw new Error("useI18n probe did not render");
  const t: Translate = captured;
  return {
    t,
    close: () => act(() => {
      root.unmount();
      host.remove();
    }),
  };
}

const TASK_KEYS = [
  "task.title",
  "task.subtitle",
  "task.refresh",
  "task.customTitle",
  "task.customSubtitle",
  "task.taskId",
  "task.storeCurrent",
  "task.none",
  "task.clearTasks",
  "task.requestCount",
  "task.run",
  "task.running",
  "task.runOk",
  "task.runFailed",
  "task.runFailedDetail",
  "task.source.builtin",
  "task.source.stored",
  "task.shadowedStored",
  "task.run.confirm",
  "task.cancel",
  "task.runCancelled",
  "task.runCleanupFailed",
  "task.error.taskBusy",
  "task.error.invalidBlob",
  "task.error.unknownTask",
  "task.error.invalidResponse",
  "task.error.catalogUnavailable",
] as const;

const REMOVED_KEYS = [
  "orchestration.title",
  "orchestration.subtitle",
  "orchestration.bootTask",
  "orchestration.setBoot",
  "orchestration.clearBoot",
  "orchestration.bootRunOk",
  "orchestration.bootRunFailed",
  "orchestration.bootRunFailedDetail",
  "orchestration.storeRecovery",
  "task.bootTask",
  "task.setBoot",
  "task.clearBoot",
  "task.presetsTitle",
  "task.presetsSubtitle",
  "task.loadPreset",
  "task.storePreset",
] as const;

describe("task i18n coverage", () => {
  afterEach(() => {
    document.body.replaceChildren();
    localStorage.clear();
  });

  it("resolves every task key in English and Chinese without key fallback", () => {
    for (const lang of ["en", "zh"] as const) {
      const view = captureTranslate(lang);
      for (const key of TASK_KEYS) {
        expect(view.t(key), `${lang} missing ${key}`).not.toBe(key);
      }
      view.close();
    }
  });

  it("provides a real Chinese translation instead of falling back to English", () => {
    const en = captureTranslate("en");
    const zh = captureTranslate("zh");
    for (const key of TASK_KEYS) {
      expect(zh.t(key), `zh falls back to en for ${key}`).not.toBe(en.t(key));
    }
    en.close();
    zh.close();
  });

  it("interpolates the stored task request count", () => {
    for (const lang of ["en", "zh"] as const) {
      const view = captureTranslate(lang);
      const rendered = view.t("task.requestCount", { count: 5 });
      expect(rendered).toContain("5");
      expect(rendered).not.toContain("{count}");
      view.close();
    }
  });

  it("interpolates run progress, result, and failure details", () => {
    for (const lang of ["en", "zh"] as const) {
      const view = captureTranslate(lang);
      const running = view.t("task.running", { index: 2, total: 5 });
      expect(running).toContain("2");
      expect(running).toContain("5");
      expect(view.t("task.runOk", { count: 3 })).toContain("3");
      const failed = view.t("task.runFailed", { index: 2, total: 5 });
      expect(failed).toContain("2");
      expect(failed).toContain("5");
      const detail = view.t("task.runFailedDetail", {
        path: "/api/v1/power/5v_out",
        error: "timeout",
      });
      expect(detail).toContain("/api/v1/power/5v_out");
      expect(detail).toContain("timeout");
      expect(detail).not.toContain("{path}");
      expect(detail).not.toContain("{error}");
      view.close();
    }
  });

  it("interpolates task data error details", () => {
    for (const lang of ["en", "zh"] as const) {
      const view = captureTranslate(lang);
      expect(view.t("task.error.invalidBlob", { detail: "bad line" })).toContain("bad line");
      expect(view.t("task.error.unknownTask", { id: "ghost" })).toContain("ghost");
      expect(view.t("task.error.invalidResponse", { detail: "no blob" })).toContain("no blob");
      view.close();
    }
  });

  it("interpolates run confirmation, cancellation, and cleanup diagnostics", () => {
    for (const lang of ["en", "zh"] as const) {
      const view = captureTranslate(lang);
      const confirm = view.t("task.run.confirm", { id: "t1", source: "Stored", count: 2 });
      expect(confirm).toContain("t1");
      expect(confirm).toContain("Stored");
      expect(confirm).toContain("2");
      expect(confirm).not.toContain("{id}");
      const cancelled = view.t("task.runCancelled", { completed: 1, total: 5 });
      expect(cancelled).toContain("1");
      expect(cancelled).toContain("5");
      expect(cancelled).not.toContain("{completed}");
      expect(view.t("task.runCleanupFailed", { error: "offline" })).toContain("offline");
      view.close();
    }
  });

  it("describes cleanup failure generically without CON_MAS, rail, GPIO, or pin identity", () => {
    for (const lang of ["en", "zh"] as const) {
      const view = captureTranslate(lang);
      const text = view.t("task.runCleanupFailed", { error: "offline" });
      expect(text).toContain("offline");
      expect(text.toLowerCase(), `${lang} cleanup wording hardcodes board facts`).not.toMatch(
        /con_mas|gpio|rail|\bpin\b/,
      );
      view.close();
    }
  });

  it("renders cancellation as partial and uncertain, never as a rollback", () => {
    const en = captureTranslate("en");
    expect(en.t("task.runCancelled", { completed: 1, total: 5 }).toLowerCase()).toContain("partial");
    expect(en.t("task.runCancelled", { completed: 1, total: 5 }).toLowerCase()).not.toContain("rollback");
    en.close();
  });

  it("describes built-in provenance and shadowing without boot or default wording", () => {
    const SOURCE_KEYS = [
      "task.source.builtin",
      "task.source.stored",
      "task.shadowedStored",
    ] as const;
    for (const lang of ["en", "zh"] as const) {
      const view = captureTranslate(lang);
      for (const key of SOURCE_KEYS) {
        const text = view.t(key);
        expect(text, `${lang} ${key} carries boot/default wording`).not.toMatch(
          /boot|default|引导|默认|自动/i,
        );
      }
      view.close();
    }
    const en = captureTranslate("en");
    expect(en.t("task.source.builtin")).toMatch(/built-?in/i);
    expect(en.t("task.shadowedStored")).toMatch(/shadow/i);
    en.close();
    const zh = captureTranslate("zh");
    expect(zh.t("task.source.builtin")).toContain("内置");
    expect(zh.t("task.shadowedStored")).toContain("遮蔽");
    zh.close();
  });

  it("keeps removed orchestration and boot keys unresolvable in both languages", () => {
    for (const lang of ["en", "zh"] as const) {
      const view = captureTranslate(lang);
      for (const key of REMOVED_KEYS) {
        expect(view.t(key), `${lang} still resolves removed ${key}`).toBe(key);
      }
      view.close();
    }
  });

  it("keeps the removed dedicated target recovery UI keys unresolvable in both languages", () => {
    const REMOVED_RECOVERY_KEYS = [
      "targetRecovery.title",
      "targetRecovery.subtitle",
      "targetRecovery.mode",
      "targetRecovery.rail",
      "targetRecovery.noRails",
      "targetRecovery.high",
      "targetRecovery.low",
      "targetRecovery.sequence",
      "targetRecovery.confirm",
      "targetRecovery.enter",
      "targetRecovery.confirmAction",
      "targetRecovery.cancel",
      "targetRecovery.running",
      "targetRecovery.done",
      "targetRecovery.failed",
      "configuration.section.recovery",
      "statusBar.task.recovery",
      "quick.recovery",
      "test.hardware.anchor.recovery",
      "test.hardware.section.recovery",
    ] as const;
    for (const lang of ["en", "zh"] as const) {
      const view = captureTranslate(lang);
      for (const key of REMOVED_RECOVERY_KEYS) {
        expect(view.t(key), `${lang} still resolves removed ${key}`).toBe(key);
      }
      view.close();
    }
  });
});
