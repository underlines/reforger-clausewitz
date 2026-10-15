// Offline test of agent/llmCall.mjs against a local mock server (no real API). node agent/test_llmCall.mjs
import assert from "node:assert/strict";
import http from "node:http";
import { chatCall, unsupportedFormats, cacheHint, createSseParser } from "./llmCall.mjs";

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let handler = null;
const reqs = [];
const server = http.createServer((req, res) => {
  let b = "";
  req.on("data", (d) => (b += d));
  req.on("end", () => {
    const rec = { url: req.url, headers: req.headers, body: b ? JSON.parse(b) : null };
    reqs.push(rec);
    res.on("close", () => {});
    handler(req, res, rec);
  });
});
await new Promise((r) => server.listen(0, "127.0.0.1", r));
const base = `http://127.0.0.1:${server.address().port}/v1`;
const sseHead = (res) => res.writeHead(200, { "content-type": "text/event-stream" });
const oa = (o) => `data: ${JSON.stringify(o)}\n\n`;
const delta = (c) => oa({ choices: [{ delta: { content: c } }] });
const common = { baseUrl: base, apiKey: "k", model: "m", system: "SYS", user: "USR", maxTokens: 100, timeoutMs: 5000, rateCooldownMs: 30000 };
const call = (o) => chatCall({ ...common, ...o });
const fails = async (p) => { try { await p; } catch (e) { return e; } assert.fail("expected rejection"); };

// --- SSE parser unit
{
  const ev = [];
  const p = createSseParser((e) => ev.push(e.data));
  for (const c of [": comment\r", "\ndata: a\r\n", "data: b\r\n\r\nda", "ta: c\n\nda", "ta: [DONE]\n\n"]) p.feed(c);
  p.end();
  assert.deepEqual(ev, ["a\nb", "c", "[DONE]"]);
}

// --- openai SSE, content split across TCP writes, comment, usage/cached/cost
handler = async (req, res) => {
  sseHead(res);
  res.write(": OPENROUTER PROCESSING\n\n");
  const full = delta('{"orders"') + delta(":[],") + delta('"keep":true}');
  res.write(full.slice(0, 30)); await sleep(20);
  res.write(full.slice(30, 77)); await sleep(20);
  res.write(full.slice(77));
  res.write(oa({ choices: [], usage: { prompt_tokens: 120, completion_tokens: 9, prompt_tokens_details: { cached_tokens: 100 }, cost: 0.0042 } }));
  res.end("data: [DONE]\n\n");
};
let r = await call({ provider: "openrouter", model: "openai/gpt-4o" });
assert.equal(r.raw, '{"orders":[],"keep":true}');
assert.deepEqual(r.usage, { input: 120, output: 9, cached: 100, cost: 0.0042 });
assert.equal(r.streamed, true);
assert.ok(r.ttftMs !== null && r.ttftMs <= r.totalMs);
assert.equal(r.responseFormatUsed, null);
// request body checks
{
  const b = reqs.at(-1).body;
  assert.equal(reqs.at(-1).url, "/v1/chat/completions");
  assert.equal(b.model, "openai/gpt-4o");
  assert.equal(b.max_tokens, 100);
  assert.equal(b.stream, true);
  assert.deepEqual(b.stream_options, { include_usage: true });
  assert.deepEqual(b.usage, { include: true });
  assert.equal(reqs.at(-1).headers.authorization, "Bearer k");
  assert.equal(b.messages[0].content, "SYS");
}
// reasoning: no field unless set; off -> disabled, low|medium|high -> effort, default -> none
assert.equal(reqs.at(-1).body.reasoning, undefined);
await call({ provider: "openrouter", reasoning: "off" });
assert.deepEqual(reqs.at(-1).body.reasoning, { enabled: false });
await call({ provider: "openrouter", reasoning: "medium" });
assert.deepEqual(reqs.at(-1).body.reasoning, { effort: "medium" });
await call({ provider: "openrouter", reasoning: "default" });
assert.equal(reqs.at(-1).body.reasoning, undefined);
// openai-compatible: no usage.include, no reasoning field
await call({ provider: "openai-compatible", reasoning: "off" });
assert.equal(reqs.at(-1).body.usage, undefined);
assert.equal(reqs.at(-1).body.reasoning, undefined);

// --- ttft after delayed first token; reasoning deltas not counted
handler = async (req, res) => {
  sseHead(res);
  res.write(oa({ choices: [{ delta: { reasoning: "thinking..." } }] }));
  res.write(oa({ choices: [{ delta: { content: "" } }] }));
  await sleep(250);
  res.write(delta("hi"));
  await sleep(50);
  res.write(delta(" there"));
  res.end(oa({ choices: [], usage: { prompt_tokens: 1, completion_tokens: 2 } }));
};
r = await call({ provider: "openai-compatible" });
assert.equal(r.raw, "hi there");
assert.ok(r.ttftMs >= 230 && r.ttftMs < r.totalMs, `ttft ${r.ttftMs} total ${r.totalMs}`);
assert.ok(r.totalMs >= 290);
assert.deepEqual(r.usage, { input: 1, output: 2 });

// --- no usage -> zeros
handler = (req, res) => { sseHead(res); res.end(delta("x")); };
r = await call({ provider: "openai-compatible" });
assert.deepEqual(r.usage, { input: 0, output: 0 });

// --- non-stream JSON
handler = (req, res) => { res.writeHead(200, { "content-type": "application/json" }); res.end(JSON.stringify({ choices: [{ message: { content: "plain" } }], usage: { prompt_tokens: 5, completion_tokens: 6, cost: 0.1 } })); };
r = await call({ provider: "openrouter", stream: false });
assert.equal(r.raw, "plain");
assert.equal(r.streamed, false);
assert.equal(r.ttftMs, null);
assert.deepEqual(r.usage, { input: 5, output: 6, cost: 0.1 });
assert.equal(reqs.at(-1).body.stream, undefined);
assert.equal(reqs.at(-1).body.stream_options, undefined);
// server ignores stream and answers JSON anyway
r = await call({ provider: "openrouter", stream: true });
assert.equal(r.raw, "plain");
assert.equal(r.streamed, false);

// --- anthropic SSE with cache tokens + cache_control on system
handler = async (req, res) => {
  sseHead(res);
  const ev = (t, o) => `event: ${t}\r\ndata: ${JSON.stringify({ type: t, ...o })}\r\n\r\n`;
  res.write(ev("message_start", { message: { usage: { input_tokens: 10, cache_read_input_tokens: 200, cache_creation_input_tokens: 30, output_tokens: 1 } } }));
  res.write(ev("content_block_start", { index: 0, content_block: { type: "text", text: "" } }));
  res.write("event: ping\ndata: {\"type\":\"ping\"}\n\n");
  const d = ev("content_block_delta", { index: 0, delta: { type: "text_delta", text: "Hel" } });
  res.write(d.slice(0, 40)); await sleep(20); res.write(d.slice(40));
  res.write(ev("content_block_delta", { index: 0, delta: { type: "text_delta", text: "lo" } }));
  res.write(ev("message_delta", { delta: { stop_reason: "end_turn" }, usage: { output_tokens: 7 } }));
  res.end(ev("message_stop", {}));
};
r = await call({ provider: "anthropic", baseUrl: base.replace("/v1", ""), cacheSystem: true, responseFormat: { type: "json_object" } });
assert.equal(r.raw, "Hello");
assert.deepEqual(r.usage, { input: 240, output: 7, cached: 200 });
assert.equal(r.responseFormatUsed, null);
assert.ok(r.ttftMs !== null);
{
  const q = reqs.at(-1);
  assert.equal(q.url, "/v1/messages");
  assert.equal(q.headers["x-api-key"], "k");
  assert.equal(q.body.stream, true);
  assert.equal(q.body.response_format, undefined);
  assert.deepEqual(q.body.system, [{ type: "text", text: "SYS", cache_control: { type: "ephemeral" } }]);
  assert.deepEqual(q.body.messages, [{ role: "user", content: "USR" }]);
}
await call({ provider: "anthropic", baseUrl: base.replace("/v1", ""), cacheSystem: false });
assert.equal(reqs.at(-1).body.system, "SYS");
// anthropic non-stream
handler = (req, res) => { res.writeHead(200, { "content-type": "application/json" }); res.end(JSON.stringify({ content: [{ type: "text", text: "A" }, { type: "text", text: "B" }], usage: { input_tokens: 3, output_tokens: 4, cache_read_input_tokens: 2 } })); };
r = await call({ provider: "anthropic", baseUrl: base.replace("/v1", ""), stream: false });
assert.equal(r.raw, "AB");
assert.deepEqual(r.usage, { input: 5, output: 4, cached: 2 });
// anthropic mid-stream error
handler = (req, res) => { sseHead(res); res.write('event: error\ndata: {"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}}\n\n'); res.end(); };
let e = await fails(call({ provider: "anthropic", baseUrl: base.replace("/v1", "") }));
assert.equal(e.status, 529);
assert.equal(e.cooldownMs, 30000);

// --- cacheHint / openrouter cache_control parts
assert.equal(cacheHint("openrouter", "anthropic/claude-sonnet-4.5"), true);
assert.equal(cacheHint("openrouter", "google/gemini-2.5-flash"), true);
assert.equal(cacheHint("openrouter", "openai/gpt-4o"), false);
assert.equal(cacheHint("anthropic", "claude-x"), false);
handler = (req, res) => { sseHead(res); res.end(delta("ok")); };
await call({ provider: "openrouter", model: "anthropic/claude-sonnet-4.5", cacheSystem: true });
assert.deepEqual(reqs.at(-1).body.messages[0].content, [{ type: "text", text: "SYS", cache_control: { type: "ephemeral" } }]);
await call({ provider: "openrouter", model: "openai/gpt-4o", cacheSystem: true });
assert.equal(reqs.at(-1).body.messages[0].content, "SYS");
await call({ provider: "openrouter", model: "anthropic/claude-sonnet-4.5", cacheSystem: false });
assert.equal(reqs.at(-1).body.messages[0].content, "SYS");

// --- response_format: sent, rejected with 400 -> retry without, memo, next call skips
const RF = { type: "json_schema", json_schema: { name: "x", schema: { type: "object" }, strict: false } };
handler = (req, res, rec) => {
  if (rec.body.response_format) { res.writeHead(400, { "content-type": "application/json" }); res.end('{"error":{"message":"response_format json_schema is not supported by this model"}}'); return; }
  sseHead(res); res.end(delta("{}"));
};
unsupportedFormats.clear();
let n0 = reqs.length;
r = await call({ provider: "openai-compatible", model: "nofmt", responseFormat: RF });
assert.equal(reqs.length - n0, 2);
assert.deepEqual(reqs[n0].body.response_format, RF);
assert.equal(reqs[n0 + 1].body.response_format, undefined);
assert.equal(r.raw, "{}");
assert.equal(r.responseFormatUsed, null);
assert.ok(unsupportedFormats.has(`openai-compatible|${base}|nofmt`));
n0 = reqs.length;
r = await call({ provider: "openai-compatible", model: "nofmt", responseFormat: RF });
assert.equal(reqs.length - n0, 1);
assert.equal(reqs[n0].body.response_format, undefined);
// accepted format is reported
handler = (req, res) => { sseHead(res); res.end(delta("{}")); };
r = await call({ provider: "openai-compatible", model: "fmtok", responseFormat: { type: "json_object" } });
assert.deepEqual(r.responseFormatUsed, { type: "json_object" });
assert.deepEqual(reqs.at(-1).body.response_format, { type: "json_object" });
// unrelated 400 does not retry
handler = (req, res) => { res.writeHead(400); res.end("bad key format"); };
n0 = reqs.length;
e = await fails(call({ provider: "openai-compatible", model: "other", responseFormat: { type: "json_object" } }));
assert.equal(e.status, 400);
assert.equal(e.cooldownMs, undefined);
assert.equal(reqs.length - n0, 1);

// --- 429 Retry-After, 500, 200-with-error, mid-stream error
handler = (req, res) => { res.writeHead(429, { "retry-after": "90" }); res.end("slow down"); };
e = await fails(call({ provider: "openrouter" }));
assert.equal(e.status, 429);
assert.equal(e.cooldownMs, 90000);
assert.match(e.message, /^HTTP 429: slow down/);
handler = (req, res) => { res.writeHead(429); res.end("x"); };
e = await fails(call({ provider: "openrouter" }));
assert.equal(e.cooldownMs, 30000);
handler = (req, res) => { res.writeHead(500); res.end("boom"); };
e = await fails(call({ provider: "openrouter" }));
assert.equal(e.status, 500);
assert.equal(e.cooldownMs, 30000);
handler = (req, res) => { res.writeHead(200, { "content-type": "application/json" }); res.end(JSON.stringify({ error: { code: 502, message: "upstream down" } })); };
e = await fails(call({ provider: "openrouter", stream: false }));
assert.equal(e.status, 502);
assert.equal(e.cooldownMs, 30000);
assert.match(e.message, /upstream down/);
e = await fails(call({ provider: "openrouter", stream: true })); // JSON answer to a streaming request
assert.equal(e.status, 502);
handler = (req, res) => { sseHead(res); res.write(delta("par")); res.end(oa({ error: { code: 503, message: "provider died" }, choices: [{ delta: { content: "" }, finish_reason: "error" }] })); };
e = await fails(call({ provider: "openrouter" }));
assert.equal(e.status, 503);
assert.match(e.message, /provider died/);
handler = (req, res) => { res.writeHead(200, { "content-type": "application/json" }); res.end(JSON.stringify({ error: { message: "no code" } })); };
e = await fails(call({ provider: "openrouter", stream: false }));
assert.equal(e.status, undefined);

// --- config errors
e = await fails(chatCall({ ...common, provider: "anthropic", apiKey: "" }));
assert.match(e.message, /API key not set/);
e = await fails(chatCall({ ...common, provider: "openai-compatible", baseUrl: "" }));
assert.match(e.message, /baseUrl not set/);

// --- timeout: no response at all, and abort mid-stream
const open = new Set();
handler = (req, res) => { open.add(res); };
let t = Date.now();
e = await fails(call({ provider: "openrouter", timeoutMs: 200 }));
assert.equal(e.name, "AbortError");
assert.ok(Date.now() - t < 2000);
handler = (req, res) => { open.add(res); sseHead(res); res.write(delta("partial")); };
t = Date.now();
e = await fails(call({ provider: "openrouter", timeoutMs: 300 }));
assert.equal(e.name, "AbortError");
assert.ok(Date.now() - t < 2000);
// timer covers the retry: rejected format, then a hang -> total bounded by one timeout
handler = (req, res, rec) => {
  if (rec.body.response_format) { res.writeHead(400); res.end("response_format unsupported"); return; }
  open.add(res);
};
unsupportedFormats.clear();
t = Date.now();
e = await fails(call({ provider: "openrouter", model: "hang", responseFormat: { type: "json_object" }, timeoutMs: 300 }));
assert.equal(e.name, "AbortError");
assert.ok(Date.now() - t < 1500);

for (const res of open) res.destroy();
server.closeAllConnections?.();
server.close();
console.log("test_llmCall: all passed");
