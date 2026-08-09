import { readFileSync } from "node:fs";
import { pathToFileURL } from "node:url";

import { McpServer } from "@modelcontextprotocol/server";
import { serveStdio } from "@modelcontextprotocol/server/stdio";
import * as z from "zod/v4";

import { LinkrMcpRuntime } from "./linkr-mcp-runtime.mjs";

const MCP_SCHEMA = "radxa-linkr-debugger.mcp.v1";
export const LINKR_MCP_VERSION = JSON.parse(
  readFileSync(new URL("../package.json", import.meta.url), "utf8"),
).version;
const channelSchema = z.enum(["uart0", "uart1"]);
const lineEndingSchema = z.enum(["none", "cr", "lf", "crlf"]);

function compactAvailability(value, fields) {
  if (!value || typeof value !== "object") return undefined;
  const result = { available: Boolean(value.available) };
  for (const field of fields) {
    if (value[field] !== undefined) result[field] = value[field];
  }
  return result;
}

export function summarizeBoardStatus(status) {
  const monitoring = status?.board_monitoring;
  const memory = monitoring?.memory;
  return {
    schema: status?.schema,
    ok: status?.ok,
    command: status?.command,
    project: status?.project,
    mcu: status?.mcu,
    usb: status?.usb,
    power_outputs: Array.isArray(status?.power_outputs)
      ? status.power_outputs.map(({ name, state, value }) => ({ name, state, value }))
      : [],
    switches: Object.fromEntries(Object.entries(status?.switches || {}).map(([name, value]) => [name, {
      route: value?.route,
    }])),
    adc_channels: Array.isArray(status?.adc_channels)
      ? status.adc_channels.map(({ name, unit }) => ({ name, unit }))
      : [],
    watchdog: status?.watchdog ? {
      supported: status.watchdog.supported,
      healthy: status.watchdog.healthy,
      armed: status.watchdog.armed,
    } : undefined,
    board_monitoring: monitoring ? {
      temperature: compactAvailability(monitoring.temperature, ["celsius"]),
      runtime: compactAvailability(monitoring.runtime, ["uptime_ms", "uptime_seconds"]),
      cpu: compactAvailability(monitoring.cpu, ["pressure_pct_x100", "usage_pct_x100"]),
      memory: memory ? {
        available: Boolean(memory.available),
        current_pressure_pct_x100: memory.current_pressure?.pressure_pct_x100,
        peak_pressure_pct_x100: memory.peak_pressure?.pressure_pct_x100,
      } : undefined,
    } : undefined,
    power_capture_protocol: status?.power_capture_protocol,
  };
}

function success(tool, result) {
  const output = { schema: MCP_SCHEMA, ok: true, tool, result };
  return {
    content: [{ type: "text", text: JSON.stringify(output, null, 2) }],
    structuredContent: output,
  };
}

function failure(tool, error) {
  const output = {
    schema: MCP_SCHEMA,
    ok: false,
    tool,
    error: {
      code: error?.code || "tool_failed",
      message: error instanceof Error ? error.message : String(error),
      ...(error?.details && Object.keys(error.details).length > 0 ? { details: error.details } : {}),
    },
  };
  return {
    isError: true,
    content: [{ type: "text", text: JSON.stringify(output, null, 2) }],
    structuredContent: output,
  };
}

function register(server, runtime, name, config, handler) {
  server.registerTool(name, config, async (args) => {
    try {
      return success(name, await handler(args, runtime));
    } catch (error) {
      return failure(name, error);
    }
  });
}

export function createLinkrMcpServer(runtime = new LinkrMcpRuntime()) {
  const server = new McpServer({
    name: "radxa-linkr-debugger",
    version: LINKR_MCP_VERSION,
  }, {
    capabilities: { tools: {} },
    instructions: "Operate the local Radxa Linkr Debugger through bounded board and shared Serial Broker tools. Read status before changing hardware state.",
  });

  register(server, runtime, "linkr_board_status", {
    title: "Read debugger status",
    description: "Read a compact power, route and health summary without changing hardware. Use detail=full only when GPIO and complete diagnostics are needed.",
    inputSchema: z.object({
      detail: z.enum(["summary", "full"]).default("summary"),
    }),
    annotations: { readOnlyHint: true, destructiveHint: false, idempotentHint: true },
  }, async (args, value) => {
    const status = await value.boardStatus();
    return args.detail === "full" ? status : summarizeBoardStatus(status);
  });

  register(server, runtime, "linkr_adc_read", {
    title: "Read current monitors",
    description: "Read the three power-rail ADC current monitors without changing hardware.",
    inputSchema: z.object({}),
    annotations: { readOnlyHint: true, destructiveHint: false, idempotentHint: true },
  }, (_args, value) => value.adcRead());

  register(server, runtime, "linkr_power_set", {
    title: "Set target power rail",
    description: "Turn a target 5V, 12V or 20V rail on or off. confirm must be true because this immediately changes target hardware power.",
    inputSchema: z.object({
      name: z.enum(["5v_out", "12v_out", "20v_out"]),
      state: z.enum(["on", "off"]),
      confirm: z.literal(true),
    }),
    annotations: { readOnlyHint: false, destructiveHint: true, idempotentHint: true },
  }, (args, value) => value.powerSet(args.name, args.state));

  register(server, runtime, "linkr_switch_route", {
    title: "Route target interfaces",
    description: "Route SD, target USB or UART VIO. confirm must be true because a route change can disconnect or electrically mismatch the target.",
    inputSchema: z.discriminatedUnion("name", [
      z.object({ name: z.literal("sd"), route: z.enum(["target", "usb-reader"]), confirm: z.literal(true) }),
      z.object({ name: z.literal("usb"), route: z.enum(["target", "pc"]), confirm: z.literal(true) }),
      z.object({ name: z.literal("vin"), route: z.enum(["1.8v", "3.3v"]), confirm: z.literal(true) }),
    ]),
    annotations: { readOnlyHint: false, destructiveHint: true, idempotentHint: true },
  }, (args, value) => value.switchRoute(args.name, args.route));

  register(server, runtime, "linkr_serial_connect", {
    title: "Connect target UART",
    description: "Subscribe to UART0 or UART1 through the shared host Serial Broker. Existing Web readers remain connected.",
    inputSchema: z.object({
      channel: channelSchema,
      baud: z.number().int().min(300).max(4_000_000).default(115200),
    }),
    annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: true },
  }, async (args, value) => (await value.serial()).open(args.channel, args.baud));

  register(server, runtime, "linkr_serial_status", {
    title: "Read target UART session status",
    description: "Read connection, baud, subscriber and current write-owner state without opening or subscribing to the UART.",
    inputSchema: z.object({ channel: channelSchema }),
    annotations: { readOnlyHint: true, destructiveHint: false, idempotentHint: true },
  }, async (args, value) => (await value.serial()).status(args.channel));

  register(server, runtime, "linkr_serial_read", {
    title: "Read target UART incrementally",
    description: "Read retained UART text after a cursor. Reuse next_cursor to avoid repeatedly returning the whole log.",
    inputSchema: z.object({
      channel: channelSchema,
      cursor: z.number().int().nonnegative().optional(),
      max_chars: z.number().int().min(1).max(65_536).default(32_768),
      wait_ms: z.number().int().min(0).max(60_000).default(0),
    }),
    annotations: { readOnlyHint: true, destructiveHint: false, idempotentHint: true },
  }, async (args, value) => (await value.serial()).read(args.channel, {
    cursor: args.cursor,
    maxChars: args.max_chars,
    waitMs: args.wait_ms,
  }));

  register(server, runtime, "linkr_serial_expect", {
    title: "Wait for target UART output",
    description: "Wait for text or a regular expression after a cursor and return only the bounded captured window.",
    inputSchema: z.object({
      channel: channelSchema,
      pattern: z.string().min(1).max(2_048),
      regex: z.boolean().default(false),
      case_sensitive: z.boolean().default(true),
      cursor: z.number().int().nonnegative().optional(),
      timeout_ms: z.number().int().min(1).max(600_000).default(30_000),
      max_chars: z.number().int().min(1).max(262_144).default(131_072),
    }),
    annotations: { readOnlyHint: true, destructiveHint: false, idempotentHint: true },
  }, async (args, value) => (await value.serial()).expect(args.channel, {
    pattern: args.pattern,
    regex: args.regex,
    caseSensitive: args.case_sensitive,
    cursor: args.cursor,
    timeoutMs: args.timeout_ms,
    maxChars: args.max_chars,
  }));

  register(server, runtime, "linkr_serial_write", {
    title: "Write target UART",
    description: "Write text to an already connected target UART. A short Broker claim prevents another client from interleaving bytes.",
    inputSchema: z.object({
      channel: channelSchema,
      text: z.string().max(65_536),
      line_ending: lineEndingSchema.default("none"),
      exclusive: z.boolean().default(true),
    }),
    annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: false },
  }, async (args, value) => {
    const serial = await value.serial();
    if (!args.exclusive) return serial.write(args.channel, args.text, { lineEnding: args.line_ending });
    await serial.claim(args.channel, "mcp-write");
    try {
      return await serial.write(args.channel, args.text, { lineEnding: args.line_ending });
    } finally {
      await serial.release(args.channel);
    }
  });

  register(server, runtime, "linkr_serial_command", {
    title: "Run target UART command",
    description: "Claim an already connected UART, send one command, wait for a prompt, return the bounded output, and release the claim.",
    inputSchema: z.object({
      channel: channelSchema,
      command: z.string().min(1).max(65_536),
      prompt: z.string().min(1).max(2_048),
      prompt_regex: z.boolean().default(false),
      line_ending: lineEndingSchema.default("cr"),
      timeout_ms: z.number().int().min(1).max(600_000).default(30_000),
    }),
    annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: false },
  }, async (args, value) => (await value.serial()).command(args.channel, {
    command: args.command,
    prompt: args.prompt,
    promptRegex: args.prompt_regex,
    lineEnding: args.line_ending,
    timeoutMs: args.timeout_ms,
  }));

  register(server, runtime, "linkr_serial_shell_command", {
    title: "Run a target shell command with exit status",
    description: "Claim an authenticated POSIX shell, run one command or script, capture its exit code and bounded output, wait for the prompt, then release the UART.",
    inputSchema: z.object({
      channel: channelSchema,
      command: z.string().min(1).max(65_536),
      prompt: z.string().min(1).max(2_048).default("[#$>]\\s*$"),
      prompt_regex: z.boolean().default(true),
      require_zero: z.boolean().default(true),
      line_ending: lineEndingSchema.default("cr"),
      timeout_ms: z.number().int().min(1).max(600_000).default(30_000),
    }),
    annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: false },
  }, async (args, value) => (await value.serial()).shellCommand(args.channel, {
    command: args.command,
    prompt: args.prompt,
    promptRegex: args.prompt_regex,
    requireZero: args.require_zero,
    lineEnding: args.line_ending,
    timeoutMs: args.timeout_ms,
  }));

  register(server, runtime, "linkr_serial_login", {
    title: "Log in to a target UART shell",
    description: "Claim an already connected UART, detect an existing shell or complete username/password login, and release it. Password text is never returned.",
    inputSchema: z.object({
      channel: channelSchema,
      username: z.string().min(1).max(256),
      password: z.string().max(4_096),
      login_prompt: z.string().min(1).max(2_048).default("login:"),
      password_prompt: z.string().min(1).max(2_048).default("Password:"),
      shell_prompt: z.string().min(1).max(2_048).default("[#$>]\\s*$"),
      shell_prompt_regex: z.boolean().default(true),
      line_ending: lineEndingSchema.default("cr"),
      timeout_ms: z.number().int().min(1).max(600_000).default(30_000),
    }),
    annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: false },
  }, async (args, value) => (await value.serial()).login(args.channel, {
    username: args.username,
    password: args.password,
    loginPrompt: args.login_prompt,
    passwordPrompt: args.password_prompt,
    shellPrompt: args.shell_prompt,
    shellPromptRegex: args.shell_prompt_regex,
    lineEnding: args.line_ending,
    timeoutMs: args.timeout_ms,
  }));

  register(server, runtime, "linkr_serial_disconnect", {
    title: "Disconnect target UART",
    description: "Release this MCP client's claim and subscription without disconnecting other Web or Agent subscribers.",
    inputSchema: z.object({ channel: channelSchema }),
    annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: true },
  }, async (args, value) => value.serialDisconnect(args.channel));

  const closeServer = server.close.bind(server);
  server.close = async () => {
    await runtime.close();
    await closeServer();
  };
  return server;
}

export function startLinkrMcpStdio() {
  return serveStdio(() => createLinkrMcpServer(), {
    onerror: (error) => process.stderr.write(`[linkr-mcp] ${error.message}\n`),
  });
}

if (import.meta.url === pathToFileURL(process.argv[1] || "").href) {
  const handle = startLinkrMcpStdio();
  let closing = false;
  const close = () => {
    if (closing) return;
    closing = true;
    void handle.close();
  };
  process.once("SIGINT", close);
  process.once("SIGTERM", close);
}
