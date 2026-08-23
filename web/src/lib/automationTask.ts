export type AutomationTaskOwner =
  | "startup"
  | "test"
  | "task"
  | "power"
  | "persistent"
  | "ota"
  | "boot";

export interface AutomationTaskControl {
  owner: AutomationTaskOwner | null;
  acquire: (owner: AutomationTaskOwner) => boolean;
  release: (owner: AutomationTaskOwner) => void;
}

export interface AutomationTaskLock {
  owner: () => AutomationTaskOwner | null;
  acquire: (owner: AutomationTaskOwner) => boolean;
  release: (owner: AutomationTaskOwner) => void;
}

export function createAutomationTaskLock(): AutomationTaskLock {
  let currentOwner: AutomationTaskOwner | null = null;
  return {
    owner: () => currentOwner,
    acquire(owner) {
      if (currentOwner != null) return false;
      currentOwner = owner;
      return true;
    },
    release(owner) {
      if (currentOwner === owner) currentOwner = null;
    },
  };
}
