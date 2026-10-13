// Shared by the HTTP tests: log in to a test sidecar (default admin password) and make every global fetch() to that base URL
// carry the session cookie. Game endpoints ignore the cookie, so patching all requests to the base is harmless.
export const DEFAULT_PASSWORD = "clausewitz";
export async function loginFetch(base, password = DEFAULT_PASSWORD) {
  const orig = globalThis.fetch;
  const r = await orig(base + "/api/login", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ password }) });
  if (!r.ok) throw new Error(`test login failed: HTTP ${r.status}`);
  const cookie = (r.headers.getSetCookie?.()[0] ?? "").split(";")[0];
  globalThis.fetch = (url, opts = {}) => {
    if (!String(url).startsWith(base)) return orig(url, opts);
    return orig(url, { ...opts, headers: { ...(opts.headers ?? {}), Cookie: cookie } });
  };
  return cookie;
}
