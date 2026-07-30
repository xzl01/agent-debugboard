import assert from "node:assert/strict";
import test from "node:test";
import {
  getNextWorkspaceTabIndex,
  getWorkspacePanelId,
  getWorkspaceTabId,
} from "./workspaceTabs.ts";

test("moves workspace tab focus with arrow keys and home/end", () => {
  assert.equal(getNextWorkspaceTabIndex(0, "ArrowRight", 4), 1);
  assert.equal(getNextWorkspaceTabIndex(3, "ArrowRight", 4), 0);
  assert.equal(getNextWorkspaceTabIndex(0, "ArrowLeft", 4), 3);
  assert.equal(getNextWorkspaceTabIndex(1, "ArrowLeft", 4), 0);
  assert.equal(getNextWorkspaceTabIndex(3, "Home", 4), 0);
  assert.equal(getNextWorkspaceTabIndex(0, "End", 4), 3);
});

test("ignores unsupported keys and keeps stable ids", () => {
  assert.equal(getNextWorkspaceTabIndex(0, "Enter", 2), null);
  assert.equal(getWorkspaceTabId("terminal"), "workspace-tab-terminal");
  assert.equal(getWorkspacePanelId("logicAnalyzer"), "workspace-panel-logicAnalyzer");
  assert.equal(getWorkspacePanelId("powerAnalysis"), "workspace-panel-powerAnalysis");
  assert.equal(getWorkspacePanelId("automation"), "workspace-panel-automation");
});
