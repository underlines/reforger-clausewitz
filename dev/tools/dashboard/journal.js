// Clausewitz HQ dashboard: HQ war journal (read-only view per faction) + the "memory" settings block of the Config form.
// Data: GET /api/state "journals" {faction: {enabled, intent, goals[{text,status}], lessons[], tries[], triesLines[], block, blockChars, reviews,
// rejected, reviewedAt, reviewWarTime, nextReviewAt, reviewing, lastError, resetAt, resetReason, recentEvents[]}} (sidecar, agent/journal.mjs).
// The journal is written by the sidecar only (review call every memory.reviewMin minutes + the game-counted attack record); nothing here edits it.
import { html } from "/vendor/standalone.module.js";
import { t, tk } from "/i18n.js"; // dashboard dictionaries (dashboard/i18n/*.json)

const num = (v) => typeof v === "number" && Number.isFinite(v);
const STATUS_CLS = { open: "info", done: "ok", failed: "bad" };
const fmtAgo = (now, ts) => {
  if (!num(ts)) return t("time.never");
  const s = Math.max(0, Math.round((now - ts) / 1000));
  return s < 90 ? t("time.agoS", s) : t("time.agoMin", Math.round(s / 60));
};
const fmtIn = (now, ts) => {
  if (!num(ts)) return "-";
  const s = Math.round((ts - now) / 1000);
  return s <= 0 ? t("wake.due") : s < 90 ? t("wake.in", s) : t("time.inMin", Math.round(s / 60));
};

function JournalCard({ f, j, now, cls }) {
  const empty = !j.intent && !(j.goals ?? []).length && !(j.lessons ?? []).length;
  const tries = Array.isArray(j.tries) ? j.tries : [];
  return html`<section class="panel journal">
    <h2><span class=${cls}>${f}</span> <span class="sub">
      <span class=${"badge " + (j.enabled ? "ok" : "warn")}>${j.enabled ? t("j.memOn") : t("j.memOff")}</span>
      <span class="badge info">${t("j.reviews", j.reviews ?? 0)}</span>
      ${j.rejected > 0 && html`<span class="badge warn" title=${j.lastError ?? ""}>${t("j.rejected", j.rejected)}</span>`}
      ${j.reviewing && html`<span class="badge warn">${t("j.reviewing")}</span>`}
      ${t("j.lastNext", fmtAgo(now, j.reviewedAt), j.enabled ? fmtIn(now, j.nextReviewAt) : t("common.off"), j.blockChars ?? 0)}</span></h2>
    <div class="jbody">
      ${empty && html`<div class="empty">${j.enabled ? t("j.noReviewOn") : t("j.noReviewOff")}</div>`}
      <dl class="kv jkv">
        <div class="wide"><dt>${t("j.intent")}</dt><dd>${j.intent || html`<span class="muted">-</span>`}</dd></div>
        <div class="wide"><dt>${t("j.goals")}</dt><dd>${(j.goals ?? []).length ? html`<ol class="jlist">${j.goals.map((g, i) => html`<li key=${i}><span class=${"badge " + (STATUS_CLS[g.status] ?? "info")}>${tk("j.status", g.status)}</span> ${g.text}</li>`)}</ol>` : html`<span class="muted">-</span>`}</dd></div>
        <div class="wide"><dt>${t("j.lessons")}</dt><dd>${(j.lessons ?? []).length ? html`<ul class="jlist">${j.lessons.map((l, i) => html`<li key=${i}>${l}</li>`)}</ul>` : html`<span class="muted">-</span>`}</dd></div>
      </dl>
      <h3 class="cfgh">${t("j.attackRecord")} <span class="muted">${t("j.attackNote")}</span></h3>
      ${tries.length ? html`<div class="scroll"><table>
        <thead><tr><th>${t("col.objective")}</th><th>${t("j.tried")}</th><th>${t("j.failed")}</th><th>${t("j.done")}</th><th>${t("j.last")}</th><th>${t("j.warTime")}</th></tr></thead><tbody>
        ${tries.map((x) => html`<tr key=${x.id} class=${x.failed > 0 ? "" : "muted"}><td><b>${x.id}</b> ${x.name ?? ""}</td><td>${x.tried}</td><td>${x.failed > 0 ? html`<span class="badge bad">${x.failed}</span>` : 0}</td><td>${x.done}</td><td class="wrap">${x.last || "-"}</td><td class="mono muted">${num(x.lastAt) ? `${Math.floor(x.lastAt / 60)}:${String(Math.floor(x.lastAt % 60)).padStart(2, "0")}` : "-"}</td></tr>`)}</tbody></table></div>`
        : html`<div class="empty">${t("j.noAttacks")}</div>`}
      ${(j.openAttacks ?? []).length > 0 && html`<small class="muted">${t("j.openAttacks", j.openAttacks.length, j.openAttacks.map((a) => `${a.squad} -> ${a.target}`).join(", "))}</small>`}
      <details class="jdet"><summary>${t("j.block", j.blockChars ?? 0)}</summary><pre>${j.block || t("j.blockEmpty")}</pre></details>
      <small class="muted">${t("j.started", new Date(j.resetAt).toLocaleTimeString(), j.resetReason ?? "new")}${j.lastError ? " " + t("j.lastProblem", j.lastError) : ""}</small>
    </div>
  </section>`;
}

// faction = selected faction name or "all"; facClass(f) = the dashboard's faction colour class
export function JournalView({ st, faction, facClass }) {
  const all = st.journals ?? {};
  const names = Object.keys(all).filter((f) => (faction === "all" || f === faction) && (st.factions?.[f] || faction !== "all")).sort();
  const now = st.now ?? Date.now();
  return html`<main class="view">
    ${names.map((f) => html`<${JournalCard} key=${f} f=${f} j=${all[f]} now=${now} cls=${facClass ? facClass(f) : ""} />`)}
    ${!names.length && html`<section class="panel"><h2>${t("j.title")} <span class="sub">${t("j.sub")}</span></h2><div class="empty">${t("j.none")}</div></section>`}
  </main>`;
}

// Settings block for the Config form: setG(group, key, value); f.memory = {enabled, reviewMin}
export function MemorySettings({ f, setG }) {
  const m = f.memory ?? {};
  return html`<h3 class="cfgh">${t("j.memHeading")}</h3>
    <div class="pair">
      <label class="switch"><input type="checkbox" checked=${m.enabled !== false} onChange=${(e) => setG("memory", "enabled", e.target.checked)} /><span>${t("j.memKeep")}</span></label>
      <label class="field"><span>${t("j.memEvery")}</span><input type="number" min="1" max="60" value=${m.reviewMin ?? 5} onInput=${(e) => setG("memory", "reviewMin", e.target.value)} /></label>
    </div>
    <small class="muted">${t("j.memNote")}</small>`;
}
