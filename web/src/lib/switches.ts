// Firmware-enumerated switch helpers: generic status parsing plus i18n
// translation-layer lookups with raw-string fallback for unknown items.

import type { SwitchState } from "./types";

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null;
}

// `prev` is the WebSocket merge base: switches/fields missing from the payload
// keep previous values. HTTP polls omit it, so missing routes default to "".
export function parseSwitches(raw: unknown, prev: SwitchState = {}): SwitchState {
  if (!isRecord(raw)) return prev;
  const next: SwitchState = { ...prev };
  for (const [name, value] of Object.entries(raw)) {
    if (!isRecord(value)) continue;
    const prior = prev[name];
    const route = typeof value.route === "string" ? value.route : (prior?.route ?? "");
    const routes = Array.isArray(value.routes)
      ? value.routes.filter((r): r is string => typeof r === "string")
      : prior?.routes;
    const requires_confirm =
      typeof value.requires_confirm === "boolean"
        ? value.requires_confirm
        : prior?.requires_confirm;
    next[name] = { route, routes, requires_confirm };
  }
  return next;
}

export type SwitchTranslate = (key: string) => string;

// t() returns the key itself when untranslated; that signals raw fallback.
function lookup(t: SwitchTranslate, key: string, raw: string): string {
  const label = t(key);
  return label === key ? raw : label;
}

export function switchNameLabel(t: SwitchTranslate, name: string): string {
  return lookup(t, `switch.name.${name}`, name);
}

export function switchDescLabel(t: SwitchTranslate, name: string): string {
  return lookup(t, `switch.desc.${name}`, "");
}

export function switchRouteLabel(t: SwitchTranslate, route: string): string {
  return lookup(t, `switch.route.${route}`, route);
}
