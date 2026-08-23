import assert from "node:assert/strict";
import test from "node:test";
import {
  WORKSPACE_TABS,
  getNextWorkspaceTabIndex,
  getWorkspacePanelId,
  getWorkspaceTabId,
} from "./workspaceTabs.ts";

test("keeps a five-tab workspace without a gpio tab", () => {
  assert.deepEqual(WORKSPACE_TABS, [
    "terminal",
    "powerAnalysis",
    "logicAnalyzer",
    "automation",
    "configuration",
  ]);
});

test("moves workspace tab focus with arrow keys and home/end", () => {
  assert.equal(getNextWorkspaceTabIndex(0, "ArrowRight", 5), 1);
  assert.equal(getNextWorkspaceTabIndex(4, "ArrowRight", 5), 0);
  assert.equal(getNextWorkspaceTabIndex(0, "ArrowLeft", 5), 4);
  assert.equal(getNextWorkspaceTabIndex(1, "ArrowLeft", 5), 0);
  assert.equal(getNextWorkspaceTabIndex(4, "Home", 5), 0);
  assert.equal(getNextWorkspaceTabIndex(0, "End", 5), 4);
});

test("ignores unsupported keys and keeps stable ids", () => {
  assert.equal(getNextWorkspaceTabIndex(0, "Enter", 2), null);
  assert.equal(getWorkspaceTabId("terminal"), "workspace-tab-terminal");
  assert.equal(getWorkspacePanelId("logicAnalyzer"), "workspace-panel-logicAnalyzer");
  assert.equal(getWorkspacePanelId("powerAnalysis"), "workspace-panel-powerAnalysis");
  assert.equal(getWorkspacePanelId("automation"), "workspace-panel-automation");
  assert.equal(getWorkspacePanelId("configuration"), "workspace-panel-configuration");
});
