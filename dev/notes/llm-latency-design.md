# LLM latency: implementation design (plan: llm-latency-plan.md)

Branch `feature/llm-latency`. All work is in the sidecar (`dev/tools`). **No game script changes**: the game's
`CLW_RemoteCommander.OnReply` already applies `sitrep`, `hqReplies/hqActions`, `orders`, `buy`, `missions`, `fobs` from ANY
/orders reply (also a `keep` reply), so background calls (SITREP, planner) deliver their result on the next poll reply
(the game polls every ~10 s, one request in flight per faction). A reply with only buy/fobs is applied like any other;
an empty reply without `keep` makes the game fall back to its rule commander, so delivery-only replies must still be
`{orders:[], keep:true, ...}` when nothing else is in them.

## Work packages
Phase A (parallel, disjoint files, pure modules + tests, no edits to `llm_sidecar.mjs`):
- **A1** `agent/promptKit.mjs` + `agent/test_promptKit.mjs`: section stats, terse reply spec/parser/schema, lean situation.
- **A2** `agent/llmCall.mjs` + `agent/test_llmCall.mjs`: one provider call with streaming, TTFT, usage, response_format, prompt caching.
- **A3** `dashboard/*` (app.js, index.html, style.css, i18n/en.json, i18n/de.json): "Latency" view + new settings UI.

Phase B (sequential, each owns `llm_sidecar.mjs` while it runs):
- **B1** steps 1, 2, 3, 6: extract prompts into `agent/commanderPrompt.mjs`; callProvider on top of llmCall; per-call metrics;
  SITREP as its own background call; terse orders; stable-first prompt order + conditional sections + caching.
- **B2** steps 4, 7: planner call (buy + fobs/logistics) split from the orders call; role -> slot tiering, HQ own cooldown, per-role timeouts.
- **B3** steps 5, 8: lean situation in all prompts; prompt budget test + large fixture + situation recording.

## Contracts

### llmCall.mjs (A2)
```js
export async function chatCall(o) -> {raw, usage:{input, output, cached?, cost?}, ttftMs, totalMs, streamed, responseFormatUsed}
// o = {provider:"anthropic"|"openai-compatible"|"openrouter", baseUrl, apiKey, model, system, user, maxTokens, timeoutMs,
//      responseFormat: null | {type:"json_object"} | {type:"json_schema", json_schema:{name, schema, strict:false}},
//      cacheSystem: bool, stream: bool (default true), rateCooldownMs, fetchImpl?}
export const unsupportedFormats: Set<string>  // "<provider>|<baseUrl>|<model>" that rejected response_format; later calls skip it
export function cacheHint(provider, model) -> bool  // true for openrouter anthropic/* and google/gemini* (explicit cache_control)
```
- Errors keep the current callProvider semantics: `err.status`, `err.cooldownMs` (429/5xx: max(rateCooldownMs, Retry-After)),
  timeout = AbortError (name "AbortError"), 200-with-`{error}` bodies throw like HTTP errors, a mid-stream `error` chunk throws.
- Streaming (openai-compatible/openrouter): `stream:true, stream_options:{include_usage:true}`, OpenRouter also `usage:{include:true}`;
  SSE `data:` lines, ignore `:` comment lines (OpenRouter "OPENROUTER PROCESSING"), `[DONE]`. ttftMs = first non-empty
  `delta.content` (reasoning deltas do not count); usage from the final chunk (`prompt_tokens`, `completion_tokens`,
  `prompt_tokens_details.cached_tokens`, `cost`). Anthropic: streaming SSE too (`content_block_delta` text, `message_start`/`message_delta`
  usage incl. `cache_read_input_tokens`); `system` as a block with `cache_control:{type:"ephemeral"}` when cacheSystem.
- response_format rejected (HTTP 400/404/422 whose body mentions response_format / json_schema / structured / "support"): add to
  unsupportedFormats, retry ONCE without it inside the same call (remaining timeout), `responseFormatUsed:null`.
- cacheSystem on openrouter/openai-compatible: when cacheHint() the system message content becomes
  `[{type:"text", text, cache_control:{type:"ephemeral"}}]`; otherwise plain string (OpenAI-style automatic prefix caching).

### promptKit.mjs (A1)
```js
export const approxTokens = (s) => Math.ceil(String(s ?? "").length / 4);
export function sectionStats(sections /* {name: text} */) -> {name: {chars, tokens}}
export function terseSpec(spec) -> string   // the REPLY paragraph of the commander prompt
//   spec = {orders:[names], buy:bool, fobs:[names], missions:bool, wake:{minSec,maxSec,defaultSec}|null, reasonWords:12}
export function terseSchema(spec) -> JSON schema (object, strict:false-friendly) for response_format json_schema
export function expandTerse(obj) -> canonical {orders, buy, missions, fobs, keep, next_check_sec, next_check_reason, sitrep?}
//   accepts the terse form AND the old verbose form (pass-through), so a model that ignores the spec still works
export function leanSituation(sit, opts) -> {static, dynamic, stats}   // see "lean situation"
```
Terse reply (one JSON object, keys optional):
```
{"o":[["<squad>","ATTACK","C","reason"],["<squad>","MOVE",[x,z],"reason"],["<squad>","FALL_BACK"]],
 "b":[["<catalog type>",1,"ATTACK","C","reason"]],
 "f":[["ESTABLISH_FOB","<site>"],["UPGRADE_FOB","<fob>"],["RESUPPLY","<fob>"],["CONVOY","<origin>","<dest>"]],
 "m":[["<human-led squad>",[["CAPTURE","D","brief"],["RECON",[x,z],"brief"]]]],
 "k":1, "n":[120,"reason"]}
```
Target slot: a string = objective id (or fob/site id), an `[x,z]` array = position, missing/null = none. Reason <= reasonWords words.
expandTerse output feeds the existing `validateV2` unchanged (`{squad,command,target|pos,reason}` etc.).

### Lean situation (A1 builds, B3 wires)
- `static` = rarely changing facts, rendered into the SYSTEM prompt after the rules (cacheable prefix): catalog (type, kind, cost; desc
  only when opts.catalogDesc), bases, objectives' id/name/pos/radius/value/side, fobSites' static fields, map/war mode info.
  Its JSON is stable for equal input (sorted keys where order is not meaningful) so the prefix stays byte-identical.
- `dynamic` = everything else for the USER message: objectives' owner/contested/progress, squads, contacts, war numbers, events, wake, ...
- Tabular encoding of every array of objects (generic, content-agnostic: unknown fields pass through):
  `{"cols":["id","kind",...],"rows":[[...],[...]]}`; nested objects one level flattened as `a.b` columns; positions -> `[x,z]` ints
  (y dropped); numbers rounded (|n| >= 100 -> int, else 2 decimals); null/""/false/empty columns dropped per row-set when all rows are empty.
- Relevance filtering (opts): contacts older than `contactMaxAgeSec` (default 600) dropped, at most `contactMax` (default 25, freshest first);
  `structures`, `debug` never sent.
- `stats` = {beforeChars, afterChars, staticChars, dynamicChars}.

### Metrics (B1 writes, A3 renders) - GET /api/state
- `llmCalls`: newest first, ring of 200: `{ts, faction, role, slot, model, ok, error, inTok, outTok, cachedTok, ttftMs, totalMs,
  promptChars, sections:{<name>:{chars,tokens}}, outputMode, fellBack}`;
  role in commander|orders|planner|sitrep|journal|hq|translate.
- `latency`: `{<role>: {n, okN, p50Ms, p90Ms, avgTtftMs, avgInTok, avgOutTok, avgCachedTok, lastMs, lastAt}}` over the ring.
- Every exchange (`exchanges[]`) gets `metrics` = the llmCalls entry of its commander call (or null).
- One log line per LLM call: `llm role=<r> faction=<f> model=<m> in=<n> out=<n> cached=<n> ttft=<ms>ms total=<ms>ms sections=<name>:<tok>,...`.

### New config (B1/B2 validate, A3 renders in Settings) - GET/POST /api/config
- `sitrep: {enabled, intervalSec}` unchanged; the SITREP is always its own call now (role sitrep).
- `output: {mode: "json_object"|"json_schema"|"text", terse: bool, reasonWords: 4..30}` default `{mode:"json_object", terse:true, reasonWords:12}`.
- `planner: {enabled: bool, intervalSec: 60..900}` default `{enabled:true, intervalSec:180}`; disabled = buys/FOBs stay in the orders call.
- `roles: {commander, planner, sitrep, journal, hq, translate}` each `"strong"|"weak"`; default
  `{commander:"strong", planner:"strong", sitrep:"weak", journal:"weak", hq:"strong", translate:"weak"}`. A role on "weak" while no weak
  slot is configured uses strong (as translate does today).
- `escalate: {enabled: bool, slot: "strong"|"weak"}` default `{enabled:false, slot:"strong"}`: commander calls woken by an important trigger
  (contact near a squad, casualties, squad lost, objective lost/changed, tickets drop) use this slot; routine ticks use roles.commander.
- `roleTimeouts: {commander, planner, sitrep, journal, hq}` seconds, 0 = slot default; default all 0.
- `prompt: {lean: bool, staticBlock: bool, cache: bool, contactMax: 5..60, contactMaxAgeSec: 60..1800}` default
  `{lean:true, staticBlock:true, cache:true, contactMax:25, contactMaxAgeSec:600}`.
- Cooldowns: the HQ role has its own cooldown keys (`hq:<slot>|<model>`), so commander timeouts never cool down /hq.

## B2 as built (planner split, tiering)
Deviations / decisions (everything else as in the contracts above):
- Planner prompt = `commanderPrompt.buildPlannerPrompt` (rules = `PLANNER_SYSTEM`, buy/fob COMMANDS + terse b/f/k spec, conditional FOB/LOGISTICS paragraphs, user = situation | ORDERS IN FORCE | HQ journal block | `PLANNER WAKE: <trigger>`). No persona, env or help-call text. Orders call (`SYSTEM_ORDERS_V2`, split mode of `buildCommanderPrompt`) has no buy/FOB/logistics wording and a `plan` section (`planBlock`) after the situation. Split is active when `config.planner.enabled` and the manifest has buy or fob intents (`plannerSplit`).
- Planner cadence runs on EVERY running poll (the commander call is gated on most), not only on commander wakes. Manual wake (`/api/wake`) starts it and skips the 30 s failure backoff.
- Delivery: pending result (120 s) re-validated with `validateV2` at attach time; attached to normal, keep and gated replies, never when the reply is a failure / rule fallback (`!r.ok`, `rules`, or gated after a failed call). Exchange gets `planner` (delivery info), `slot`, `escalated`; the planner call itself is an `llmCalls` entry with role `planner` (+ `GET /api/state planner`). Gated deliveries have no exchange (as before for gated polls).
- Fake provider: planner inline after the orders part on every commander call (`fakeV2(sit, manifest, "all"|"orders"|"plan")`).
- Cooldown keys: role hq uses `hq:<slot>|<model>`; `slotChain` returns `{slot, scope, primary, fallback}`, `coolLeftMs/noteCooldown(ch, cfg)` take the chain. Without a backup the commander, planner, journal and sitrep calls use the cooldown; hq agent turns only when a backup exists (as before).
- `roles.hq = "weak"` makes the agent turn run on the weak slot's model (`runAgentTurn config: chain.primary`).
- Journal default slot is now weak (was strong), per the roles default of the contract.
- Escalation: `IMPORTANT_TRIGGER` regex over the wake trigger texts (contact N m from, casualties, lost, objective ..., tickets ...); not applied to scheduled/manual wakes.
- Test env override `CLW_PLANNER_MIN_GAP_MS` (default 45000).
