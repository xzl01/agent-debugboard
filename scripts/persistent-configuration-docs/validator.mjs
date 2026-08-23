import { readFile } from "node:fs/promises";
import path from "node:path";
import {
  APP_END_MARKER, APP_HEADINGS, CANONICAL_HEADINGS, DOC_SURFACES, ERROR_CODES, FORBIDDEN_CLAIMS,
  FROZEN_SUMMARY, HIL_HEADINGS, REQUIRED_EXAMPLES, REQUIRED_LITERALS, RESPONSE_FIELDS, SKILL_HEADINGS,
  SKILL_CURRENT_SYNC_CONTRACT, WEB_CURRENT_SYNC_CONTRACT,
} from "./contracts.mjs";
import {
  addFailure, checkCurrentSyncMarkers, checkLinks, curlRequest, headingIndex,
  markedShellExamples, negated, normalizeShell, requireHeadings, sectionFor,
} from "./markdown.mjs";
import { checkApplicationFacts, checkCanonicalFacts, checkHilCompletionFacts, checkHilFacts, checkSkillFacts } from "./requirements.mjs";

function sameJson(left, right) {
  return JSON.stringify(left) === JSON.stringify(right);
}

function checkForbidden(surface, section, failures) {
  for (const [name, expression] of FORBIDDEN_CLAIMS) {
    for (const match of section.content.matchAll(new RegExp(expression.source, `${expression.flags.replace("g", "")}g`))) {
      if (!negated(section.content, match.index)) {
        addFailure(failures, "forbidden-claim", surface.path, `${name}: ${match[0]}`);
        break;
      }
    }
  }
  const port = /https?:\/\/[^\s'"`]*:8080(?:\/|\b)/i.exec(section.content);
  if (port && !negated(section.content, port.index)) {
    addFailure(failures, "forbidden-port", surface.path, `persistent-config example uses ${port[0]}`);
  }
}

function checkCanonical(surface, section, failures) {
  requireHeadings(section.content, CANONICAL_HEADINGS, surface.path, failures);
  const normalized = section.content.replace(/\s+/g, " ").toLowerCase();
  for (const literal of REQUIRED_LITERALS) {
    if (!normalized.includes(literal.toLowerCase())) {
      addFailure(failures, "contract-literal", surface.path, `missing ${JSON.stringify(literal)}`);
    }
  }
  if (!/app-only[\s`]*zephyr\.uf2[\s\S]{0,120}invalid[\s\S]{0,120}ROM[- ]?BOOTSEL/i.test(section.content)) {
    addFailure(failures, "contract-literal", surface.path, "must state that app-only zephyr.uf2 is invalid for ROM BOOTSEL");
  }
  for (const field of RESPONSE_FIELDS) if (!section.content.includes(field)) addFailure(failures, "response-field", surface.path, `missing ${field}`);
  for (const code of ERROR_CODES) if (!section.content.includes(code)) addFailure(failures, "error-code", surface.path, `missing ${code}`);
  for (const grammar of ["config show", "config save [--confirm] <firmware-item-id>...", "config clear"]) {
    if (!section.content.includes(grammar)) addFailure(failures, "cdc-grammar", surface.path, `missing ${grammar}`);
  }
}

function summary(surface, section, failures) {
  if (headingIndex(section.content, surface.summaryHeading) < 0) addFailure(failures, "heading-missing", surface.path, `missing ${surface.summaryHeading}`);
  const actual = new Map();
  for (const line of section.content.split(/\r?\n/)) {
    const row = /^\|\s*`([^`]+)`\s*\|\s*`([^`]+)`\s*\|\s*$/.exec(line.trim());
    if (row) {
      if (row[1] === "busy" && row[2] !== "busy:capture\\|ota") {
        addFailure(failures, "summary-pipe-escape", surface.path, "busy summary value must escape its Markdown table pipe");
      }
      actual.set(row[1], row[2].replaceAll("\\|", "|"));
    }
  }
  const expected = new Map(FROZEN_SUMMARY);
  if (actual.size !== expected.size || [...expected].some(([id, value]) => actual.get(id) !== value)) {
    addFailure(failures, "summary-contract", surface.path, "frozen contract-summary IDs and literals differ");
  }
  return actual;
}

function checkPlacement(text, before, heading, after, surface, failures) {
  const start = text.indexOf(heading);
  const left = text.indexOf(before);
  const right = text.indexOf(after);
  if (left >= 0 && start >= 0 && left >= start) addFailure(failures, "placement", surface, `${heading} must follow ${before}`);
  if (right >= 0 && start >= 0 && start >= right) addFailure(failures, "placement", surface, `${heading} must precede ${after}`);
}

function checkSurfaceMap(surface, text, section, failures) {
  if (surface.path === "apps/radxa_linkr_debugger/README.md") {
    requireHeadings(section.content, APP_HEADINGS, surface.path, failures);
    checkPlacement(text, "curl -fsS http://172.29.203.1/api/v1/status", surface.heading, "Raw MCUboot OTA API", surface.path, failures);
    checkApplicationFacts(section.content, surface.path, failures);
  }
  if (surface.path === "skills/radxa-linkr-debugger/SKILL.md") {
    requireHeadings(section.content, SKILL_HEADINGS, surface.path, failures);
    checkPlacement(text, "## JSON Contract", surface.heading, "## Common Commands", surface.path, failures);
    checkSkillFacts(section.content, surface.path, failures);
    checkCurrentSyncMarkers(surface, section.content, SKILL_CURRENT_SYNC_CONTRACT, failures);
  }
  if (surface.path === "docs/testing/hil-functional-test-spec.md") {
    requireHeadings(section.content, HIL_HEADINGS, surface.path, failures);
    checkPlacement(text, "### 2c. 强制门户发现", surface.heading, "### 3. 电源输出 get/set", surface.path, failures);
    checkHilFacts(section.content, surface.path, failures);
  }
}

function checkExample(example, spec, failures) {
  if (spec.kind === "cli") {
    const command = normalizeShell(example.command);
    if (!/(?:^|\s)radxa-linkr-debuggerctl(?:\s|$)/.test(command) || !command.includes(spec.command)) {
      addFailure(failures, "example-contract", example.surface, `${example.id} must run radxa-linkr-debuggerctl ${spec.command}`);
    }
    return;
  }
  const request = curlRequest(example.command);
  const expectedUrl = `http://172.29.203.1${spec.path}`;
  if (!/\bcurl\b/.test(request.normalized) || request.url !== expectedUrl || request.method !== spec.method) {
    addFailure(failures, "example-contract", example.surface, `${example.id} must be ${spec.method} ${expectedUrl}`);
  }
  if (spec.body !== undefined && (!request.hasData || !sameJson(request.body, spec.body))) {
    addFailure(failures, "example-contract", example.surface, `${example.id} must send ${JSON.stringify(spec.body)}`);
  }
  if (spec.body === undefined && request.hasData) addFailure(failures, "example-contract", example.surface, `${example.id} must not send a request body`);
}

export async function checkPersistentConfigurationDocs(rootPath) {
  const root = path.resolve(rootPath);
  const failures = [];
  const loaded = await Promise.all(DOC_SURFACES.map(async (surface) => {
    try {
      return { surface, text: await readFile(path.resolve(root, surface.path), "utf8") };
    } catch (error) {
      if (error?.code !== "ENOENT") throw error;
      addFailure(failures, "surface-missing", surface.path, "required documentation surface is missing");
      return { surface, text: null };
    }
  }));
  const examples = [];
  const summaries = new Map();
  let canonical = false;
  for (const { surface, text } of loaded) {
    if (text === null) continue;
    let section = sectionFor(text, surface.heading);
    if (!section) {
      addFailure(failures, "section-missing", surface.path, `missing ${surface.heading}`);
      continue;
    }
    if (surface.path === "apps/radxa_linkr_debugger/README.md") {
      const end = section.content.indexOf(APP_END_MARKER);
      if (end < 0) {
        addFailure(failures, "section-boundary", surface.path, `missing ${APP_END_MARKER} boundary`);
      } else {
        section = { ...section, content: section.content.slice(0, end) };
      }
    }
    await checkLinks(root, surface, section, failures);
    examples.push(...markedShellExamples(surface, section, failures));
    checkForbidden(surface, section, failures);
    if (surface.path === "docs/reference/persistent-configuration.md" || surface.path === "docs/testing/hil-functional-test-spec.md") {
      checkHilCompletionFacts(section.content, surface.path, failures);
    }
    checkSurfaceMap(surface, text, section, failures);
    if (surface.path === "docs/reference/persistent-configuration.md") {
      canonical = true;
      checkCanonical(surface, section, failures);
      checkCanonicalFacts(section.content, surface.path, failures);
      checkCurrentSyncMarkers(surface, section.content, WEB_CURRENT_SYNC_CONTRACT, failures);
    }
    if (surface.summaryHeading) summaries.set(surface.path, summary(surface, section, failures));
  }
  const byId = new Map();
  for (const example of examples) {
    if (byId.has(example.id)) addFailure(failures, "example-duplicate", example.surface, `duplicate persistent-config-example ID ${example.id}`);
    else byId.set(example.id, example);
  }
  if (canonical) for (const [id, spec] of Object.entries(REQUIRED_EXAMPLES)) {
    const example = byId.get(id);
    if (!example) addFailure(failures, "example-missing", "docs/reference/persistent-configuration.md", `missing persistent-config-example: ${id}`);
    else checkExample(example, spec, failures);
  }
  const english = summaries.get("README.md");
  const chinese = summaries.get("README.zh-CN.md");
  if (english && chinese && !sameJson([...english], [...chinese])) addFailure(failures, "summary-parity", "README.zh-CN.md", "English and Chinese contract summaries differ");
  return { ok: failures.length === 0, failures, examples, documents: DOC_SURFACES.map((surface) => surface.path) };
}
