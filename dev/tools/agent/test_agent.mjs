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

// ---- assertions (fake provider only): reports / backup / cancel / negatives, EN + DE
if (provider === "fake" && process.argv.length <= 3) {
  const base = messages.vehicle;
  const T = [ // [text, expected action | null (no call), expected args subset]
    ["2 enemy infantry about 100m north of my position", "REPORT_CONTACT", { kind: "inf", count: "2", distance: "100", direction: "N" }],
    ["contact front, 3 vehicles 400 meters", "REPORT_CONTACT", { kind: "veh", count: "3", distance: "400", direction: "ahead" }],
    ["2 feindliche Infanteristen 100m nördlich", "REPORT_CONTACT", { kind: "inf", count: "2", distance: "100", direction: "N" }],
    ["spotted a tank 1.2 km northeast", "REPORT_CONTACT", { kind: "armor", count: "1", distance: "1200", direction: "NE" }],
    ["Feind: drei Fahrzeuge, 1,5 km südwestlich", "REPORT_CONTACT", { kind: "veh", count: "3", distance: "1500", direction: "SW" }],
    ["enemy helicopter 300 yards bearing 270", "REPORT_CONTACT", { kind: "air", count: "1", distance: "274", direction: "270" }],
    ["Kontakt, zwölf Soldaten 250 m vor mir", "REPORT_CONTACT", { kind: "inf", count: "12", distance: "250", direction: "ahead" }],
    ["I see an enemy BMP 600 meters east", "REPORT_CONTACT", { kind: "armor", count: "1", distance: "600", direction: "E" }],
    ["enemy troops 80 m 135 degrees", "REPORT_CONTACT", { kind: "inf", distance: "80", direction: "135" }],
    ["enemy squad 200m", "REPORT_CONTACT", { kind: "inf", distance: "200", direction: "ahead" }],
    ["we're pinned down, need help", "REQUEST_BACKUP", {}],
    ["brauchen Hilfe, wir stehen unter Beschuss", "REQUEST_BACKUP", {}],
    ["send backup to my position, under fire", "REQUEST_BACKUP", {}],
    ["cancel my last report", "REPORT_CANCEL", {}],
    ["that report was wrong", "REPORT_CANCEL", {}],
    ["Meldung zurückziehen", "REPORT_CANCEL", {}],
    ["die letzte Meldung war falsch", "REPORT_CANCEL", {}],
    // negatives / questions
    ["I need a truck", "REQUEST_VEHICLE", null],
    ["send reinforcements", "REQUEST_REINFORCE", null],
    ["Verstärkung bitte", "REQUEST_REINFORCE", null],
    ["I need a truck, 500m away from the base", "REQUEST_VEHICLE", null],
    ["enemy infantry north", null, null], // distance missing: ask, no call
    ["enemy tank at 3 o'clock 400m", null, null], // relative direction: ask
    ["where is the enemy right now?", null, null], // query_contacts, not an action
  ];
  let bad = 0;
  for (const [text, want, args] of T) {
    const r = await runAgentTurn({ manifest, situation, message: { ...base, id: 99, text }, config });
    const a = r.actions[0];
    let ok = r.ok && r.actions.length === (want ? 1 : 0) && (!want || a.action === want);
    if (ok && args) for (const [k, v] of Object.entries(args)) if (a.args.find((x) => x.name === k)?.value !== v) ok = false;
    if (!ok) { bad++; console.log(`FAIL "${text}": want ${want} ${JSON.stringify(args)}, got ${JSON.stringify(r.actions)} reply="${r.replyText}" err=${r.error}`); }
  }
  // the three new tools are core (not deferred) and have the right schema
  const { buildTools } = await import("./agentTurn.mjs");
  const b = buildTools(manifest, situation, base);
  for (const n of ["REPORT_CONTACT", "REPORT_CANCEL", "REQUEST_BACKUP"]) if (!b.tools[n] || b.deferred.includes(n)) { bad++; console.log(`FAIL tool ${n} missing or deferred`); }
  // without the new tools the old fake rules still apply
  const old = { ...manifest, primitives: manifest.primitives.filter((e) => !["REPORT_CONTACT", "REPORT_CANCEL", "REQUEST_BACKUP"].includes(e.name)) };
  const r = await runAgentTurn({ manifest: old, situation, message: { ...base, text: "we need help" }, config });
  if (r.actions[0]?.action !== "REQUEST_REINFORCE") { bad++; console.log("FAIL old manifest: help -> REQUEST_REINFORCE"); }
  console.log(bad ? `${bad} assertion(s) FAILED` : `all ${T.length + 4} assertions passed`);
  if (bad) process.exitCode = 1;
}
