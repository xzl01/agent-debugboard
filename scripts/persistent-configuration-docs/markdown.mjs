import { access } from "node:fs/promises";
import { constants as fsConstants } from "node:fs";
import path from "node:path";

const SHELL_LANGUAGES = new Set(["bash", "console", "sh", "shell", "zsh"]);

export function addFailure(failures, code, surface, detail) {
  failures.push({ code, surface, detail });
}

export function headingParts(heading) {
  const match = /^(#+)\s+(.+)$/.exec(heading);
  return { level: match[1].length, text: match[2] };
}

export function parseHeading(line) {
  const match = /^(#{1,6})\s+(.+?)\s*#*\s*$/.exec(line);
  return match ? { level: match[1].length, text: match[2] } : null;
}

export function headingIndex(text, heading) {
  const expected = headingParts(heading);
  return text.split(/\r?\n/).findIndex((line) => {
    const actual = parseHeading(line);
    return actual?.level === expected.level && actual.text === expected.text;
  });
}

export function sectionFor(text, expectedHeading) {
  const start = headingIndex(text, expectedHeading);
  if (start < 0) return null;
  const lines = text.split(/\r?\n/);
  const { level } = headingParts(expectedHeading);
  let end = lines.length;
  for (let index = start + 1; index < lines.length; index += 1) {
    const heading = parseHeading(lines[index]);
    if (heading && heading.level <= level) {
      end = index;
      break;
    }
  }
  return { content: lines.slice(start + 1, end).join("\n"), line: start + 1, start, end };
}

export function requireHeadings(content, headings, surface, failures) {
  for (const heading of headings) {
    if (headingIndex(content, heading) < 0) {
      addFailure(failures, "heading-missing", surface, `missing ${heading}`);
    }
  }
}

function links(content) {
  return [...content.matchAll(/\[[^\]]*\]\(([^)\s]+)(?:\s+"[^"]*")?\)/g)].map((match) => match[1]);
}

export function clauseAround(text, index) {
  const start = Math.max(text.lastIndexOf("\n", index), text.lastIndexOf(".", index), text.lastIndexOf(";", index)) + 1;
  const ends = [text.indexOf("\n", index), text.indexOf(".", index), text.indexOf(";", index)].filter((value) => value >= 0);
  return text.slice(start, ends.length === 0 ? text.length : Math.min(...ends)).toLowerCase();
}

export function negated(text, index) {
  return /\b(?:no|not|never|without)\b|\b(?:does|do|is|are)\s+not\b|不(?:提供|是|使用|支持)|没有|无/.test(clauseAround(text, index));
}

function localTarget(surfacePath, href) {
  const target = href.split("#", 1)[0].split("?", 1)[0];
  if (!target || /^(?:[a-z][a-z0-9+.-]*:|\/\/)/i.test(target)) return null;
  return path.posix.normalize(path.posix.join(path.posix.dirname(surfacePath), target));
}

async function exists(root, target) {
  try {
    await access(path.resolve(root, target), fsConstants.F_OK);
    return true;
  } catch {
    return false;
  }
}

export async function checkLinks(root, surface, section, failures) {
  const local = links(section.content)
    .map((href) => ({ href, target: localTarget(surface.path, href) }))
    .filter(({ target }) => target !== null);
  for (const required of surface.links) {
    if (!local.some(({ target }) => target === required)) {
      addFailure(failures, "link-required", surface.path, `link to ${required} is required`);
    }
  }
  for (const { href, target } of local) {
    if (!(await exists(root, target))) {
      addFailure(failures, "link-missing", surface.path, `local link ${href} resolves to missing ${target}`);
    }
  }
}

export function normalizeShell(command) {
  return command.replace(/\\\r?\n/g, " ").replace(/\r?\n/g, " ").replace(/\s+/g, " ").trim();
}

export function shellBlocks(content) {
  const lines = content.split(/\r?\n/);
  const blocks = [];
  for (let index = 0; index < lines.length; index += 1) {
    const open = /^```\s*([^\s`]*)\s*$/.exec(lines[index]);
    if (!open || !SHELL_LANGUAGES.has(open[1].toLowerCase())) continue;
    let close = index + 1;
    while (close < lines.length && !/^```\s*$/.test(lines[close])) close += 1;
    if (close < lines.length) blocks.push({ language: open[1].toLowerCase(), command: lines.slice(index + 1, close).join("\n"), line: index + 1 });
    index = close;
  }
  return blocks;
}

export function markedShellExamples(surface, section, failures) {
  const lines = section.content.split(/\r?\n/);
  const examples = [];
  for (let index = 0; index < lines.length; index += 1) {
    const open = /^```\s*([^\s`]*)\s*$/.exec(lines[index]);
    if (!open || !SHELL_LANGUAGES.has(open[1].toLowerCase())) continue;
    let close = index + 1;
    while (close < lines.length && !/^```\s*$/.test(lines[close])) close += 1;
    if (close === lines.length) {
      addFailure(failures, "example-unclosed", surface.path, `unclosed ${open[1]} block at section line ${index + 1}`);
      break;
    }
    let markerIndex = index - 1;
    while (markerIndex >= 0 && lines[markerIndex].trim() === "") markerIndex -= 1;
    const marker = markerIndex < 0 ? null : /^<!--\s*persistent-config-example:\s*([a-z0-9][a-z0-9-]*)\s*-->$/i.exec(lines[markerIndex].trim());
    if (!marker) {
      addFailure(failures, "example-unmarked", surface.path, `shell block at section line ${index + 1} lacks a persistent-config-example marker`);
    } else {
      examples.push({ id: marker[1], surface: surface.path, command: lines.slice(index + 1, close).join("\n") });
    }
    index = close;
  }
  return examples;
}

export function curlRequest(command) {
  const normalized = normalizeShell(command);
  const url = /https?:\/\/[^\s'"`]+/.exec(normalized)?.[0];
  const method = /(?:--request|-X)\s+([A-Z]+)/i.exec(normalized)?.[1]?.toUpperCase() ?? "GET";
  const data = /(?:--data(?:-raw|-binary|-ascii)?|-d)\s+(['"])(.*?)\1/.exec(normalized)?.[2];
  try {
    return { normalized, url, method, body: data === undefined ? undefined : JSON.parse(data), hasData: data !== undefined };
  } catch {
    return { normalized, url, method, body: null, hasData: data !== undefined };
  }
}

const CURRENT_SYNC_MARKER_OPEN = /^<!--\s*persistent-config-current-sync\s*:\s*$/;
const CURRENT_SYNC_MARKER_LINE = /^([a-z][a-z0-9-]*)\s*:\s*(.+?)\s*$/;
const CURRENT_SYNC_MARKER_CLOSE = /^-->\s*$/;
const CURRENT_SYNC_MARKER_INLINE_VALUE = /^<!--\s*persistent-config-current-sync\s*:\s*([a-z][a-z0-9-]*\s*:\s*.+?)\s*-->$/;

function collectMarkerEntries(content) {
  const entries = [];
  let i = 0;
  const lines = content.split(/\r?\n/);
  while (i < lines.length) {
    if (CURRENT_SYNC_MARKER_OPEN.exec(lines[i])) {
      let j = i + 1;
      const body = [];
      while (j < lines.length && !CURRENT_SYNC_MARKER_CLOSE.exec(lines[j])) {
        body.push(lines[j]);
        j += 1;
      }
      if (j >= lines.length) {
        entries.push({ ok: false, detail: `unclosed persistent-config-current-sync marker at section line ${i + 1}` });
        i = j;
        continue;
      }
      for (const entry of parseMarkerBody(body, i + 2)) entries.push(entry);
      i = j + 1;
      continue;
    }
    const inlineMatch = CURRENT_SYNC_MARKER_INLINE_VALUE.exec(lines[i]);
    if (inlineMatch) {
      const parsed = CURRENT_SYNC_MARKER_LINE.exec(inlineMatch[1]);
      if (parsed) entries.push({ ok: true, id: parsed[1], value: parsed[2], line: i + 1 });
      else entries.push({ ok: false, detail: `malformed persistent-config-current-sync marker at section line ${i + 1}` });
      i += 1;
      continue;
    }
    i += 1;
  }
  return entries;
}

function parseMarkerBody(body, baseLine) {
  const entries = [];
  for (const [offset, raw] of body.entries()) {
    const lineNo = baseLine + offset;
    const trimmed = raw.trim();
    if (trimmed === "" || trimmed.startsWith("#")) continue;
    const parsed = CURRENT_SYNC_MARKER_LINE.exec(trimmed);
    if (parsed) entries.push({ ok: true, id: parsed[1], value: parsed[2], line: lineNo });
    else entries.push({ ok: false, detail: `malformed persistent-config-current-sync entry at line ${lineNo}` });
  }
  return entries;
}

export function parseCurrentSyncMarkers(content) {
  const raw = collectMarkerEntries(content);
  const map = new Map();
  const duplicates = [];
  const malformed = [];
  for (const entry of raw) {
    if (!entry.ok) {
      malformed.push(entry.detail);
      continue;
    }
    if (map.has(entry.id)) duplicates.push(entry.id);
    else map.set(entry.id, entry.value);
  }
  return { map, duplicates, malformed };
}

export function checkCurrentSyncMarkers(surface, content, expected, failures) {
  const { map, duplicates, malformed } = parseCurrentSyncMarkers(content);
  for (const detail of malformed) {
    addFailure(failures, "current-sync-marker", surface.path, detail);
  }
  for (const id of duplicates) {
    addFailure(failures, "current-sync-marker", surface.path, `duplicate persistent-config-current-sync marker ID: ${id}`);
  }
  const expectedIds = new Set(expected.map(([id]) => id));
  const seenIds = new Set(map.keys());
  for (const id of expectedIds) {
    if (!seenIds.has(id)) {
      addFailure(failures, "current-sync-marker", surface.path, `missing persistent-config-current-sync marker: ${id}`);
    }
  }
  for (const id of seenIds) {
    if (!expectedIds.has(id)) {
      addFailure(failures, "current-sync-marker", surface.path, `unknown persistent-config-current-sync marker ID: ${id}`);
    }
  }
  const expectedMap = new Map(expected);
  for (const [id, value] of expectedMap) {
    if (map.get(id) !== undefined && map.get(id) !== value) {
      addFailure(failures, "current-sync-marker", surface.path, `drifted persistent-config-current-sync marker ${id}: expected ${JSON.stringify(value)} got ${JSON.stringify(map.get(id))}`);
    }
  }
}
