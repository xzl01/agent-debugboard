import assert from "node:assert/strict";
import test from "node:test";
import {
  getNextWorkspaceTabIndex,
  getWorkspacePanelId,
  getWorkspaceTabId,
} from "./workspaceTabs.ts";

test("moves workspace tab focus with arrow keys and home/end", () => {
  assert.equal(getNextWorkspaceTabIndex(0, "ArrowRight", 2), 1);
  assert.equal(getNextWorkspaceTabIndex(1, "ArrowRight", 2), 0);
  assert.equal(getNextWorkspaceTabIndex(0, "ArrowLeft", 2), 1);
  assert.equal(getNextWorkspaceTabIndex(1, "ArrowLeft", 2), 0);
  assert.equal(getNextWorkspaceTabIndex(1, "Home", 2), 0);
  assert.equal(getNextWorkspaceTabIndex(0, "End", 2), 1);
});

test("ignores unsupported keys and keeps stable ids", () => {
  assert.equal(getNextWorkspaceTabIndex(0, "Enter", 2), null);
  assert.equal(getWorkspaceTabId("terminal"), "workspace-tab-terminal");
  assert.equal(getWorkspacePanelId("logicAnalyzer"), "workspace-panel-logicAnalyzer");
});
