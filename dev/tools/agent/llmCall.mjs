// One LLM provider call with streaming (SSE), TTFT, normalised usage, response_format fallback and prompt-cache hints.
// Zero deps (global fetch, Node 20+). Error semantics match the old callProvider in llm_sidecar.mjs:
//   err.status (HTTP code), err.cooldownMs (429/5xx: max(rateCooldownMs, Retry-After)), timeout = AbortError.
const OR_BASE = "https://openrouter.ai/api/v1";
const ANTHROPIC_BASE = "https://api.anthropic.com";
const isNum = (n) => typeof n === "number" && Number.isFinite(n);

/** "<provider>|<baseUrl>|<model>" keys that rejected response_format; later calls skip it. */
export const unsupportedFormats = new Set();

/** Explicit cache_control markers needed (OpenRouter Anthropic / Gemini models). Other models cache prefixes automatically. */
export function cacheHint(provider, model) {
  if (provider === "anthropic") return false; // handled natively via the system block
  return /^(anthropic\/|google\/gemini)/i.test(String(model ?? ""));
}

function apiError(status, msg, rateCooldownMs, retryAfterMs = 0) {
  const err = new Error(`HTTP ${status ?? "?"}: ${String(msg).slice(0, 200)}`);
  if (typeof status === "number") {
    err.status = status;
    if (status === 429 || status >= 500) err.cooldownMs = Math.max(rateCooldownMs ?? 0, retryAfterMs);
  }
  return err;
}

// {error:{code,message}} body or stream chunk -> Error
function bodyError(j, rateCooldownMs) {
  const e = j.error;
  const code = typeof e === "object" && e ? (isNum(e.code) ? e.code : undefined) : undefined;
  const msg = typeof e === "string" ? e : (e?.message ?? JSON.stringify(e));
  return apiError(code, msg, rateCooldownMs);
}

const ANTHROPIC_ERR_STATUS = { overloaded_error: 529, rate_limit_error: 429, api_error: 500, invalid_request_error: 400, authentication_error: 401, permission_error: 403, not_found_error: 404 };

/** Incremental SSE parser: feed(text) calls onEvent({event,data}) per complete event; end() flushes. Handles LF/CRLF/CR, comments, multi data lines. */
export function createSseParser(onEvent) {
  let buf = "", data = [], event = "";
  const line = (l) => {
    if (l === "") {
      if (data.length) onEvent({ event, data: data.join("\n") });
      data = []; event = "";
    } else if (l[0] === ":") { /* comment (e.g. OPENROUTER PROCESSING) */ }
    else {
      const i = l.indexOf(":");
      const f = i < 0 ? l : l.slice(0, i);
      let v = i < 0 ? "" : l.slice(i + 1);
      if (v[0] === " ") v = v.slice(1);
      if (f === "data") data.push(v);
      else if (f === "event") event = v;
    }
  };
  const drain = (final) => {
    let start = 0;
    for (let i = 0; i < buf.length; i++) {
      const ch = buf[i];
      if (ch !== "\n" && ch !== "\r") continue;
      if (ch === "\r" && i === buf.length - 1 && !final) break; // may be the first half of CRLF
      line(buf.slice(start, i));
      if (ch === "\r" && buf[i + 1] === "\n") i++;
      start = i + 1;
    }
    buf = buf.slice(start);
  };
  return {
    feed(t) { buf += t; drain(false); },
    end() { drain(true); if (buf) { line(buf); buf = ""; } line(""); },
  };
}

/** OpenRouter `reasoning` body field for a reasoning mode: off -> disabled, low|medium|high -> effort, default / unset -> none (model default). */
export function reasoningParam(mode) {
  if (mode === "off") return { enabled: false };
  if (mode === "low" || mode === "medium" || mode === "high") return { effort: mode };
  return null;
}

/**
 * o = {provider, baseUrl, apiKey, model, system, user, maxTokens, timeoutMs, responseFormat, cacheSystem, stream (default true), rateCooldownMs, fetchImpl,
 *      reasoning: "off"|"default"|"low"|"medium"|"high" (openrouter only)}
 * -> {raw, usage:{input, output, cached?, cost?}, ttftMs, totalMs, streamed, responseFormatUsed}
 */
export async function chatCall(o) {
  const t0 = Date.now();
  const anth = o.provider === "anthropic";
  const doFetch = o.fetchImpl ?? fetch;
  const stream = o.stream !== false;
  const rateCooldownMs = o.rateCooldownMs ?? 0;
  const base = anth ? (o.baseUrl || ANTHROPIC_BASE) : (o.provider === "openrouter" ? (o.baseUrl || OR_BASE) : o.baseUrl);
  if (anth && !o.apiKey) throw new Error("API key not set");
  if (!anth && !base) throw new Error("baseUrl not set");
  const fmtKey = `${o.provider}|${base}|${o.model}`;

  const ac = new AbortController();
  const timer = setTimeout(() => ac.abort(), o.timeoutMs);
  try {
    let fmt = !anth && o.responseFormat && !unsupportedFormats.has(fmtKey) ? o.responseFormat : null;
    for (;;) {
      let url, headers, body;
      if (anth) {
        url = base + "/v1/messages";
        headers = { "x-api-key": o.apiKey, "anthropic-version": "2023-06-01", "content-type": "application/json" };
        const system = o.cacheSystem ? [{ type: "text", text: o.system, cache_control: { type: "ephemeral" } }] : o.system;
        body = { model: o.model, max_tokens: o.maxTokens, system, messages: [{ role: "user", content: o.user }] };
        if (stream) body.stream = true;
      } else {
        url = base + "/chat/completions";
        headers = { "content-type": "application/json" };
        if (o.apiKey) headers.authorization = "Bearer " + o.apiKey;
        const sys = o.cacheSystem && cacheHint(o.provider, o.model) ? [{ type: "text", text: o.system, cache_control: { type: "ephemeral" } }] : o.system;
        body = { model: o.model, max_tokens: o.maxTokens, messages: [{ role: "system", content: sys }, { role: "user", content: o.user }] };
        if (fmt) body.response_format = fmt;
        if (stream) { body.stream = true; body.stream_options = { include_usage: true }; }
        if (o.provider === "openrouter") { body.usage = { include: true }; const rz = reasoningParam(o.reasoning); if (rz) body.reasoning = rz; }
      }
      if (stream) headers.accept = "text/event-stream";
      const r = await doFetch(url, { method: "POST", signal: ac.signal, headers, body: JSON.stringify(body) });
      if (!r.ok) {
        const text = await r.text();
        if (fmt && [400, 404, 422].includes(r.status) && /response_format|json_schema|structured|support/i.test(text)) {
          unsupportedFormats.add(fmtKey);
          fmt = null;
          continue; // retry once without it (same timer)
        }
        throw apiError(r.status, text, rateCooldownMs, (Number(r.headers.get("retry-after")) || 0) * 1000);
      }
      const res = await readBody(r, { anth, stream, rateCooldownMs, t0 });
      return { ...res, totalMs: Date.now() - t0, streamed: res.streamed, responseFormatUsed: fmt };
    }
  } finally {
    clearTimeout(timer);
  }
}

async function readBody(r, { anth, stream, rateCooldownMs, t0 }) {
  const ct = String(r.headers.get("content-type") ?? "");
  if (!stream || !/event-stream/i.test(ct)) {
    const j = await r.json();
    if (j?.error && !j.choices && !j.content) throw bodyError(j, rateCooldownMs);
    let raw, usage;
    if (anth) {
      raw = (j.content ?? []).filter((b) => b.type === "text").map((b) => b.text).join("");
      usage = anthUsage({}, j.usage);
    } else {
      raw = j.choices?.[0]?.message?.content ?? "";
      usage = oaUsage(j.usage);
    }
    return { raw, usage, ttftMs: null, streamed: false };
  }

  let raw = "", ttftMs = null, usageSrc = null;
  const anthAcc = {};
  const textDelta = (t) => {
    if (typeof t === "string" && t) { if (ttftMs === null) ttftMs = Date.now() - t0; raw += t; }
  };
  const onEvent = ({ data }) => {
    if (data.trim() === "[DONE]") return;
    let j;
    try { j = JSON.parse(data); } catch { return; } // tolerate junk lines
    if (anth) {
      if (j.type === "error") {
        const e = j.error ?? {};
        throw apiError(ANTHROPIC_ERR_STATUS[e.type], e.message ?? JSON.stringify(e), rateCooldownMs);
      }
      if (j.type === "message_start") anthUsage(anthAcc, j.message?.usage);
      else if (j.type === "message_delta") anthUsage(anthAcc, j.usage);
      else if (j.type === "content_block_delta" && j.delta?.type === "text_delta") textDelta(j.delta.text);
    } else {
      if (j.error) throw bodyError(j, rateCooldownMs);
      const c = j.choices?.[0];
      if (c?.delta) textDelta(c.delta.content); // delta.reasoning / reasoning_content do not count
      if (j.usage) usageSrc = j.usage;
    }
  };
  const parser = createSseParser(onEvent);
  const dec = new TextDecoder();
  const reader = r.body.getReader();
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      parser.feed(dec.decode(value, { stream: true }));
    }
    parser.feed(dec.decode());
    parser.end();
  } finally {
    reader.cancel().catch(() => {}); // rejects after an abort; must not become an unhandled rejection
  }
  const usage = anth ? finishAnth(anthAcc) : oaUsage(usageSrc);
  return { raw, usage, ttftMs, streamed: true };
}

function oaUsage(u) {
  const usage = { input: u?.prompt_tokens ?? 0, output: u?.completion_tokens ?? 0 };
  const cached = u?.prompt_tokens_details?.cached_tokens;
  if (isNum(cached)) usage.cached = cached;
  if (isNum(u?.cost)) usage.cost = u.cost;
  return usage;
}

// Accumulates Anthropic usage fields (message_start carries input/cache, message_delta carries output); also used for the non-stream usage.
function anthUsage(acc, u) {
  if (u) for (const k of ["input_tokens", "output_tokens", "cache_read_input_tokens", "cache_creation_input_tokens"]) if (isNum(u[k])) acc[k] = u[k];
  return finishAnth(acc);
}
function finishAnth(a) {
  // input includes cached/cache-written tokens, like OpenAI's prompt_tokens
  const usage = { input: (a.input_tokens ?? 0) + (a.cache_read_input_tokens ?? 0) + (a.cache_creation_input_tokens ?? 0), output: a.output_tokens ?? 0 };
  if (isNum(a.cache_read_input_tokens)) usage.cached = a.cache_read_input_tokens;
  return usage;
}
