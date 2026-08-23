import path from "node:path";
import { fileURLToPath } from "node:url";
export {
  DOC_SURFACES, FROZEN_SUMMARY, REQUIRED_EXAMPLES, SKILL_CURRENT_SYNC_CONTRACT, WEB_CURRENT_SYNC_CONTRACT,
} from "./persistent-configuration-docs/contracts.mjs";
export { checkPersistentConfigurationDocs } from "./persistent-configuration-docs/validator.mjs";

export function formatFailures(failures) {
  return [
    "persistent-configuration documentation contract failed:",
    ...failures.map(({ code, surface, detail }) => `- [${code}] ${surface}: ${detail}`),
  ].join("\n");
}

async function main() {
  const args = process.argv.slice(2);
  if (args.length !== 2 || args[0] !== "--root") {
    console.error("usage: node scripts/check-persistent-configuration-docs.mjs --root <repository-root>");
    process.exitCode = 2;
    return;
  }
  const { checkPersistentConfigurationDocs } = await import("./persistent-configuration-docs/validator.mjs");
  const result = await checkPersistentConfigurationDocs(args[1]);
  if (!result.ok) {
    console.error(formatFailures(result.failures));
    process.exitCode = 1;
  }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) await main();
