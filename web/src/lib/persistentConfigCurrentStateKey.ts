import type { PowerOutput, SafeGpio, SwitchInfo } from "./types";

export type PersistentConfigCurrentState = {
  readonly powerOutputs: readonly Readonly<Pick<PowerOutput, "name" | "state">>[];
  readonly switches: Readonly<Record<string, Readonly<Pick<SwitchInfo, "route">>>>;
  readonly gpios: readonly Readonly<Pick<SafeGpio, "name" | "direction" | "value">>[];
};

function compareName(
  left: { readonly name: string },
  right: { readonly name: string }
): number {
  if (left.name < right.name) return -1;
  if (left.name > right.name) return 1;
  return 0;
}

export function persistentConfigCurrentStateKey(
  state: PersistentConfigCurrentState | null | undefined
): string {
  if (!state) return "unavailable";

  const power = state.powerOutputs
    .map(({ name, state: value }) => ({ name, state: value }))
    .sort(compareName);
  const switches = Object.entries(state.switches)
    .map(([name, { route }]) => ({ name, route }))
    .sort(compareName);
  const gpios = state.gpios
    .map(({ name, direction, value }) => ({ name, direction, value }))
    .sort(compareName);

  return JSON.stringify({ power, switches, gpios });
}
