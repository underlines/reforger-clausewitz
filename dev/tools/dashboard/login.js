// Clausewitz HQ login page (public). Posts the password to /api/login; the sidecar answers with the clw_session cookie.
import { t } from "/i18n.js";

const f = document.getElementById("f"), pw = document.getElementById("pw"), err = document.getElementById("err");
f.addEventListener("submit", async (e) => {
  e.preventDefault();
  err.hidden = true;
  let r, j = {};
  try {
    r = await fetch("/api/login", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ password: pw.value }) });
    try { j = await r.json(); } catch { /* no body */ }
  } catch { err.textContent = t("login.offline"); err.hidden = false; return; }
  if (r.ok) { location.replace("/"); return; }
  err.textContent = r.status === 429 ? t("login.rateLimited", j.retryAfter ?? 60) : r.status === 401 ? t("login.wrong") : t("err.generic", j.error ?? r.status);
  err.hidden = false;
  pw.select();
});
