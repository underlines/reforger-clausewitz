// Direct test of runAgentTurn with the fixture. node agent/test_agent.mjs [fake|openrouter] [messageKey ...]
// openrouter reads dev/tools/sidecar-config.json (key and model); the key is never printed.
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { runAgentTurn } from "./agentTurn.mjs";
import { manifest, situation, messages } from "./fixtures/hq_fixture.mjs";

const DIR = path.dirname(fileURLToPath(import.meta.url));
const provider = process.argv[2] ?? "fake";
const keys = process.argv.slice(3).length ? process.argv.slice(3) : Object.keys(messages);
let config = { provider: "fake", model: "x", apiKey: "" };
if (provider !== "fake") {
  const saved = JSON.parse(fs.readFileSync(path.join(DIR, "..", "sidecar-config.json"), "utf8"));
  config = { provider, model: process.env.CLW_MODEL || saved.model, apiKey: saved.apiKey, baseUrl: saved.baseUrl };
}
for (const k of keys) {
  const r = await runAgentTurn({ manifest, situation, message: messages[k], config });
  const { steps, ...rest } = r;
  console.log(`--- ${k}: "${messages[k].text}"`);
  console.log(JSON.stringify({ ok: rest.ok, error: rest.error, reply: rest.replyText, actions: rest.actions, usage: rest.usage, latencyMs: rest.latencyMs, tools: rest.tools.length }));
  for (const s of steps) console.log(`  step ${s.n} [${s.finishReason}] calls=${s.toolCalls.map((c) => c.name + JSON.stringify(c.input)).join(", ") || "-"} text="${s.text}"`);
}
