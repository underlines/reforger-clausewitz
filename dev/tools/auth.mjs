// Admin login + game token for the Clausewitz sidecar (plain Node, no deps). Design: dev/notes/loadouts-arsenal-plan.md section R5.
//   - one admin account; password = scrypt + random salt, stored by the sidecar in sidecar-config.json as admin:{salt,hash}
//     (no admin stored = DEFAULT_PASSWORD applies and the dashboard shows a "change password" banner)
//   - session = random token in cookie clw_session (HttpOnly; SameSite=Strict; Path=/; Secure behind https), only the SHA-256 of the
//     token is kept server-side, in <dataDir>/sessions.json (survives tabs and sidecar restarts), 30 days
//   - login rate limit: MAX_FAILS failures per FAIL_WINDOW_MS per remote address
//   - game channel: header X-CLW-Token compared in constant time with config gameToken (unset = open, backwards compatible)
import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";

export const DEFAULT_PASSWORD = "clausewitz";
export const COOKIE = "clw_session";
export const SESSION_MS = 30 * 24 * 3600 * 1000;
export const MIN_PASSWORD = 8;
const MAX_PASSWORD = 200;
const MAX_FAILS = 5, FAIL_WINDOW_MS = 5 * 60 * 1000;
const SCRYPT_LEN = 64;

const sha256 = (s) => crypto.createHash("sha256").update(String(s)).digest("hex");

// constant-time string compare (hash both so the lengths do not leak)
export function safeEqual(a, b) {
  return crypto.timingSafeEqual(crypto.createHash("sha256").update(String(a)).digest(), crypto.createHash("sha256").update(String(b)).digest());
}

export function hashPassword(pw, saltHex) {
  const salt = saltHex ?? crypto.randomBytes(16).toString("hex");
  const hash = crypto.scryptSync(String(pw), Buffer.from(salt, "hex"), SCRYPT_LEN).toString("hex");
  return { salt, hash };
}

export const validAdmin = (a) => !!a && typeof a === "object" && /^[0-9a-f]{16,128}$/.test(a.salt ?? "") && /^[0-9a-f]{128}$/.test(a.hash ?? "");

// admin = {salt, hash} | null (null = default password)
export function verifyPassword(pw, admin) {
  if (typeof pw !== "string" || pw.length === 0 || pw.length > MAX_PASSWORD) return false;
  if (!validAdmin(admin)) return safeEqual(pw, DEFAULT_PASSWORD);
  const { hash } = hashPassword(pw, admin.salt);
  return safeEqual(hash, admin.hash);
}

export function parseCookies(header) {
  const out = {};
  for (const part of String(header ?? "").split(";")) {
    const i = part.indexOf("=");
    if (i > 0) out[part.slice(0, i).trim()] = part.slice(i + 1).trim();
  }
  return out;
}

export const isSecure = (req) => !!req.socket?.encrypted || String(req.headers["x-forwarded-proto"] ?? "").split(",")[0].trim().toLowerCase() === "https";
export const remoteOf = (req) => req.socket?.remoteAddress ?? "?";

// X-CLW-Token check for the game endpoints: no token configured = open
export function gameTokenOk(req, gameToken) {
  if (!gameToken) return true;
  const got = req.headers["x-clw-token"];
  return typeof got === "string" && safeEqual(got, gameToken);
}

export function createAuth({ dataDir, getAdmin, setAdmin, now = () => Date.now() }) {
  const file = path.join(dataDir, "sessions.json");
  let sessions = {}; // sha256(token) -> expiresAt (ms)
  const fails = new Map(); // ip -> [timestamps]

  function load() {
    try {
      const j = JSON.parse(fs.readFileSync(file, "utf8"));
      if (j && typeof j === "object" && !Array.isArray(j)) sessions = j;
    } catch { sessions = {}; }
    prune();
  }
  function persist() {
    try {
      fs.mkdirSync(dataDir, { recursive: true });
      const tmp = file + ".tmp";
      fs.writeFileSync(tmp, JSON.stringify(sessions));
      fs.renameSync(tmp, file);
    } catch (e) { console.log(`could not save sessions: ${e.message}`); }
  }
  function prune() {
    const t = now();
    let changed = false;
    for (const [k, exp] of Object.entries(sessions)) if (typeof exp !== "number" || exp <= t) { delete sessions[k]; changed = true; }
    return changed;
  }
  load();

  const tokenOf = (req) => parseCookies(req.headers.cookie)[COOKIE] ?? "";
  const isLoggedIn = (req) => {
    const tok = tokenOf(req);
    if (!/^[0-9a-f]{64}$/.test(tok)) return false;
    const exp = sessions[sha256(tok)];
    return typeof exp === "number" && exp > now();
  };

  function cookieHeader(value, maxAgeSec, req) {
    return `${COOKIE}=${value}; HttpOnly; SameSite=Strict; Path=/; Max-Age=${maxAgeSec}${isSecure(req) ? "; Secure" : ""}`;
  }

  // rate limit: returns 0 when allowed, else seconds to wait
  function blockedFor(ip) {
    const t = now(), list = (fails.get(ip) ?? []).filter((x) => t - x < FAIL_WINDOW_MS);
    fails.set(ip, list);
    if (list.length < MAX_FAILS) return 0;
    return Math.max(1, Math.ceil((FAIL_WINDOW_MS - (t - list[0])) / 1000));
  }
  const noteFail = (ip) => { const l = fails.get(ip) ?? []; l.push(now()); fails.set(ip, l); };

  // -> {ok:true, setCookie} | {ok:false, status, error, retryAfter?}
  function login(req, password) {
    const ip = remoteOf(req), wait = blockedFor(ip);
    if (wait) return { ok: false, status: 429, error: "too many failed logins", retryAfter: wait };
    if (!verifyPassword(password, getAdmin())) { noteFail(ip); return { ok: false, status: 401, error: "wrong password" }; }
    fails.delete(ip);
    const token = crypto.randomBytes(32).toString("hex");
    prune();
    sessions[sha256(token)] = now() + SESSION_MS;
    persist();
    return { ok: true, setCookie: cookieHeader(token, SESSION_MS / 1000, req) };
  }

  // -> Set-Cookie value that clears the cookie
  function logout(req) {
    const tok = tokenOf(req);
    if (tok && sessions[sha256(tok)] !== undefined) { delete sessions[sha256(tok)]; persist(); }
    return cookieHeader("", 0, req);
  }

  // -> {ok:true} | {ok:false, status, error}
  function changePassword(req, oldPw, newPw) {
    const ip = remoteOf(req), wait = blockedFor(ip);
    if (wait) return { ok: false, status: 429, error: "too many failed attempts", retryAfter: wait };
    if (typeof newPw !== "string" || newPw.length < MIN_PASSWORD || newPw.length > MAX_PASSWORD) return { ok: false, status: 400, error: `new password must be ${MIN_PASSWORD}..${MAX_PASSWORD} characters` };
    if (!verifyPassword(oldPw, getAdmin())) { noteFail(ip); return { ok: false, status: 403, error: "old password is wrong" }; }
    if (safeEqual(newPw, DEFAULT_PASSWORD)) return { ok: false, status: 400, error: "choose a password other than the default" };
    setAdmin(hashPassword(newPw));
    // keep the caller's session, drop the others
    const keep = tokenOf(req), keepKey = /^[0-9a-f]{64}$/.test(keep) ? sha256(keep) : null;
    const exp = keepKey ? sessions[keepKey] : undefined;
    sessions = exp !== undefined ? { [keepKey]: exp } : {};
    persist();
    return { ok: true };
  }

  return { isLoggedIn, login, logout, changePassword, isDefaultPassword: () => !validAdmin(getAdmin()), sessionCount: () => Object.keys(sessions).length };
}

// HTTP handlers for /api/login, /api/logout, /api/session, /api/password. ctx = {send(res, code, obj), readBody(req, limit), originOk(req)}.
// Returns false when the route is not ours. The caller has already done the host check.
export async function handleHttp(auth, req, res, url, ctx) {
  const routes = { "/api/login": "POST", "/api/logout": "POST", "/api/password": "POST", "/api/session": "GET" };
  if (routes[url] !== req.method) return false;
  const sendCookie = (code, obj, cookie, extra = {}) => {
    res.writeHead(code, { "Content-Type": "application/json", "Cache-Control": "no-store", ...(cookie ? { "Set-Cookie": cookie } : {}), ...extra });
    res.end(JSON.stringify(obj));
  };
  if (url === "/api/session") {
    if (!auth.isLoggedIn(req)) { ctx.send(res, 401, { error: "login required" }); return true; }
    ctx.send(res, 200, { ok: true, defaultPassword: auth.isDefaultPassword() });
    return true;
  }
  if (!ctx.originOk(req)) { ctx.send(res, 403, { error: "forbidden origin" }); return true; }
  if (url === "/api/logout") { sendCookie(200, { ok: true }, auth.logout(req)); return true; }
  if (url === "/api/password" && !auth.isLoggedIn(req)) { ctx.send(res, 401, { error: "login required" }); return true; }
  if (!/^application\/json\b/i.test(req.headers["content-type"] ?? "")) { ctx.send(res, 400, { error: "Content-Type must be application/json" }); return true; }
  let inp;
  try { inp = JSON.parse((await ctx.readBody(req, 10_000)) || "{}"); } catch { ctx.send(res, 400, { error: "invalid JSON" }); return true; }
  if (url === "/api/login") {
    const r = auth.login(req, inp?.password);
    if (!r.ok) sendCookie(r.status, { error: r.error, ...(r.retryAfter ? { retryAfter: r.retryAfter } : {}) }, null, r.retryAfter ? { "Retry-After": String(r.retryAfter) } : {});
    else sendCookie(200, { ok: true, defaultPassword: auth.isDefaultPassword() }, r.setCookie);
    return true;
  }
  const r = auth.changePassword(req, inp?.oldPassword, inp?.newPassword);
  if (!r.ok) sendCookie(r.status, { error: r.error }, null, r.retryAfter ? { "Retry-After": String(r.retryAfter) } : {});
  else sendCookie(200, { ok: true, defaultPassword: false }, null);
  return true;
}
