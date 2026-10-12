// End-to-end HTTP test of the HQ chat path against a running sidecar (use a SECOND instance on another port):
//   CLW_PORT=8799 CLW_LLM=fake CLW_CONFIG=<scratch>/cfg.json node llm_sidecar.mjs
//   node agent/test_http.mjs http://127.0.0.1:8799 [messageKey ...]
// Plays the game: POST /manifest, then POST /orders with the pending hq message (re-sent every second like the 10 s poll) until the reply
// carries hqActions / hqReplies.
import { manifest, situation, messages } from "./fixtures/hq_fixture.mjs";

const base = process.argv[2] ?? "http://127.0.0.1:8799";
const keys = process.argv.slice(3).length ? process.argv.slice(3) : ["vehicle"];
const post = async (p, body) => (await fetch(base + p, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) })).json();

console.log("manifest", JSON.stringify(await post("/manifest", manifest)));
for (const k of keys) {
  const msg = messages[k];
  const t0 = Date.now();
  let got = null;
  for (let i = 0; i < 60 && !got; i++) {
    const reply = await post("/orders", { ...situation, manifest: manifest.hash, hq: [msg] });
    if (reply.hqReplies || reply.hqActions) got = reply;
    else await new Promise((r) => setTimeout(r, 1000));
  }
  console.log(`--- ${k}: "${msg.text}" (${Date.now() - t0} ms)`);
  console.log(JSON.stringify({ hqActions: got?.hqActions, hqReplies: got?.hqReplies, keep: got?.keep }));
}
const st = await (await fetch(base + "/api/state")).json();
for (const h of st.hq) console.log(`log: msg=${h.msg.id} state=${h.state} ok=${h.ok} steps=${h.steps.length} cost=${h.usage?.cost ?? 0} ${h.latencyMs}ms err=${h.error ?? "-"}`);
