// Backup (fallback) model helpers shared by the sidecar and the HQ agent turn. Pure functions, no HTTP, no config file.
// Design: each LLM slot (strong, weak) may carry an optional `fallback` = { model, provider?, baseUrl?, apiKey?, timeoutSec? };
// omitted fields inherit from the slot. A call goes to the primary; on a RETRYABLE failure it is tried once on the fallback.
//   retryable:     HTTP 408/429/5xx, timeout, network error, "provider cooling down"
//   not retryable: 400/401/403/404, invalid model output, missing key/baseUrl (a config problem)

const RETRY_STATUS = (s) => s === 408 || s === 429 || (s >= 500 && s <= 599);
const NET_CODES = new Set(["ECONNRESET", "ECONNREFUSED", "ETIMEDOUT", "ENOTFOUND", "EAI_AGAIN", "EPIPE", "UND_ERR_SOCKET", "UND_ERR_CONNECT_TIMEOUT", "UND_ERR_HEADERS_TIMEOUT", "UND_ERR_BODY_TIMEOUT"]);

/** HTTP status carried by an error of ours (.status) or of the AI SDK (.statusCode, also inside RetryError.lastError / .cause). */
export function statusOf(e) {
  for (let x = e, i = 0; x && i < 4; x = x.lastError ?? x.cause, i++) {
    const s = x.status ?? x.statusCode;
    if (typeof s === "number") return s;
  }
  return null;
}

/** Should this failure be retried on the fallback model? */
export function isRetryable(e) {
  if (!e) return false;
  if (e.retryable === true || e.cooling === true || e.timeout === true) return true;
  const s = statusOf(e);
  if (s !== null) return RETRY_STATUS(s);
  for (let x = e, i = 0; x && i < 4; x = x.lastError ?? x.cause, i++) {
    if (x.name === "AbortError" || x.name === "TimeoutError") return true;
    if (NET_CODES.has(x.code)) return true;
    if (x.name === "TypeError" && /fetch failed|network|socket|terminated/i.test(String(x.message))) return true;
    if (x.isRetryable === true && x.name === "AI_APICallError") return true; // SDK call error without a status: connection level
  }
  return false;
}

/** Milliseconds a model should rest after this failure ("" = no cooldown): 429/5xx per Retry-After (>= floorMs), timeout/network only when a fallback exists. */
export function cooldownFor(e, floorMs, hasFallback) {
  if (typeof e?.cooldownMs === "number") return e.cooldownMs;
  const s = statusOf(e);
  if (s !== null && (s === 429 || s >= 500)) return floorMs;
  if (hasFallback && !e?.cooling && isRetryable(e)) return floorMs;
  return 0;
}

/**
 * Effective fallback config: the slot config overlaid with the fallback's own fields. The slot's apiKey is inherited only while the fallback
 * keeps the same provider and baseUrl (never send the main key to another host). Returns null when there is no usable fallback
 * (none configured, no model, or identical to the primary).
 */
export function resolveFallback(base, fb) {
  if (!fb || typeof fb !== "object" || typeof fb.model !== "string" || !fb.model) return null;
  const out = { ...base };
  delete out.fallback;
  delete out.weak;
  out.model = fb.model;
  if (fb.provider) out.provider = fb.provider;
  if (fb.baseUrl) out.baseUrl = fb.baseUrl;
  if (typeof fb.timeoutSec === "number") out.timeoutSec = fb.timeoutSec;
  const sameHost = out.provider === base.provider && (out.baseUrl || "") === (base.baseUrl || "");
  out.apiKey = fb.apiKey ? fb.apiKey : sameHost ? base.apiKey : "";
  if (out.provider === base.provider && out.model === base.model && (out.baseUrl || "") === (base.baseUrl || "") && out.apiKey === base.apiKey) return null;
  return out;
}

/** Public (key-free) view of a stored fallback. */
export function publicFallback(fb) {
  if (!fb) return null;
  return { model: fb.model ?? "", provider: fb.provider ?? "", baseUrl: fb.baseUrl ?? "", timeoutSec: typeof fb.timeoutSec === "number" ? fb.timeoutSec : null, apiKeySet: !!fb.apiKey, apiKeyLast4: fb.apiKey ? fb.apiKey.slice(-4) : "" };
}
