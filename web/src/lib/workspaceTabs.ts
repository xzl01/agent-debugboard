export type WorkspaceTabId = "terminal" | "powerAnalysis" | "logicAnalyzer" | "automation" | "configuration";

export const WORKSPACE_TABS: WorkspaceTabId[] = ["terminal", "powerAnalysis", "logicAnalyzer", "automation", "configuration"];

export function getWorkspaceTabId(tab: WorkspaceTabId) {
  return `workspace-tab-${tab}`;
}

export function getWorkspacePanelId(tab: WorkspaceTabId) {
  return `workspace-panel-${tab}`;
}

export function getNextWorkspaceTabIndex(currentIndex: number, key: string, count: number) {
  if (count <= 0) return null;

  switch (key) {
    case "ArrowRight":
      return (currentIndex + 1) % count;
    case "ArrowLeft":
      return (currentIndex - 1 + count) % count;
    case "Home":
      return 0;
    case "End":
      return count - 1;
    default:
      return null;
  }
}
