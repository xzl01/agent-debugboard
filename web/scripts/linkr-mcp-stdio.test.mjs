import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import { fileURLToPath } from "node:url";

import { Client } from "@modelcontextprotocol/client";
import { StdioClientTransport } from "@modelcontextprotocol/client/stdio";

test("stdio MCP starts cleanly and lists tools without touching hardware", async () => {
  const transport = new StdioClientTransport({
    command: "npm",
    args: ["run", "--silent", "mcp:node"],
    cwd: fileURLToPath(new URL("..", import.meta.url)),
    env: { ...process.env, LINKR_MCP_AUTOSTART_GATEWAY: "0" },
    stderr: "pipe",
  });
  const client = new Client({ name: "linkr-mcp-stdio-test", version: "1.0.0" });
  try {
    await client.connect(transport);
    const packageVersion = JSON.parse(
      readFileSync(new URL("../package.json", import.meta.url), "utf8"),
    ).version;
    assert.equal(client.getServerVersion()?.version, packageVersion);
    const { tools } = await client.listTools();
    assert.equal(tools.length, 13);
    assert.equal(tools.some((tool) => tool.name === "linkr_serial_command"), true);
    assert.equal(tools.some((tool) => tool.name === "linkr_serial_login"), true);
    assert.equal(tools.some((tool) => tool.name === "linkr_serial_shell_command"), true);
  } finally {
    await client.close();
  }
});
