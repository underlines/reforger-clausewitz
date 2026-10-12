// Clausewitz HQ dashboard: HQ war journal (read-only view per faction) + the "memory" settings block of the Config form.
// Data: GET /api/state "journals" {faction: {enabled, intent, goals[{text,status}], lessons[], tries[], triesLines[], block, blockChars, reviews,
// rejected, reviewedAt, reviewWarTime, nextReviewAt, reviewing, lastError, resetAt, resetReason, recentEvents[]}} (sidecar, agent/journal.mjs).
// The journal is written by the sidecar only (review call every memory.reviewMin minutes + the game-counted attack record); nothing here edits it.
import { html } from "/vendor/standalone.module.js";

const num = (v) => typeof v === "number" && Number.isFinite(v);
const STATUS_CLS = { open: "info", done: "ok", failed: "bad" };
const fmtAgo = (now, ts) => {
  if (!num(ts)) return "never";
  const s = Math.max(0, Math.round((now - ts) / 1000));
  return s < 90 ? `${s} s ago` : `${Math.round(s / 60)} min ago`;
};
const fmtIn = (now, ts) => {
  if (!num(ts)) return "-";
  const s = Math.round((ts - now) / 1000);
  return s <= 0 ? "due" : s < 90 ? `in ${s} s` : `in ${Math.round(s / 60)} min`;
};

function JournalCard({ f, j, now, cls }) {
  const empty = !j.intent && !(j.goals ?? []).length && !(j.lessons ?? []).length;
  const tries = Array.isArray(j.tries) ? j.tries : [];
  return html`<section class="panel journal">
    <h2><span class=${cls}>${f}</span> <span class="sub">
      <span class=${"badge " + (j.enabled ? "ok" : "warn")}>${j.enabled ? "memory on" : "memory off"}</span>
      <span class="badge info">${j.reviews ?? 0} review(s)</span>
      ${j.rejected > 0 && html`<span class="badge warn" title=${j.lastError ?? ""}>${j.rejected} failed/rejected</span>`}
      ${j.reviewing && html`<span class="badge warn">reviewing...</span>`}
      last ${fmtAgo(now, j.reviewedAt)}, next ${j.enabled ? fmtIn(now, j.nextReviewAt) : "off"}; prompt block ${j.blockChars ?? 0} chars</span></h2>
    <div class="jbody">
      ${empty && html`<div class="empty">No review yet${j.enabled ? "; the first one runs when the war starts" : " (memory is switched off in Usage & Config)"}.</div>`}
      <dl class="kv jkv">
        <div class="wide"><dt>Intent</dt><dd>${j.intent || html`<span class="muted">-</span>`}</dd></div>
        <div class="wide"><dt>Goals</dt><dd>${(j.goals ?? []).length ? html`<ol class="jlist">${j.goals.map((g, i) => html`<li key=${i}><span class=${"badge " + (STATUS_CLS[g.status] ?? "info")}>${g.status}</span> ${g.text}</li>`)}</ol>` : html`<span class="muted">-</span>`}</dd></div>
        <div class="wide"><dt>Lessons</dt><dd>${(j.lessons ?? []).length ? html`<ul class="jlist">${j.lessons.map((l, i) => html`<li key=${i}>${l}</li>`)}</ul>` : html`<span class="muted">-</span>`}</dd></div>
      </dl>
      <h3 class="cfgh">Attack record <span class="muted">counted by the game and the sidecar, not by the model; the prompt shows only targets with a failure</span></h3>
      ${tries.length ? html`<div class="scroll"><table>
        <thead><tr><th>Objective</th><th>Tried</th><th>Failed</th><th>Done</th><th>Last result</th><th>War time</th></tr></thead><tbody>
        ${tries.map((t) => html`<tr key=${t.id} class=${t.failed > 0 ? "" : "muted"}><td><b>${t.id}</b> ${t.name ?? ""}</td><td>${t.tried}</td><td>${t.failed > 0 ? html`<span class="badge bad">${t.failed}</span>` : 0}</td><td>${t.done}</td><td class="wrap">${t.last || "-"}</td><td class="mono muted">${num(t.lastAt) ? `${Math.floor(t.lastAt / 60)}:${String(Math.floor(t.lastAt % 60)).padStart(2, "0")}` : "-"}</td></tr>`)}</tbody></table></div>`
        : html`<div class="empty">No attacks recorded yet.</div>`}
      ${(j.openAttacks ?? []).length > 0 && html`<small class="muted">${j.openAttacks.length} attack(s) in progress: ${j.openAttacks.map((a) => `${a.squad} -> ${a.target}`).join(", ")}</small>`}
      <details class="jdet"><summary>Prompt block as the HQ sees it (${j.blockChars ?? 0} chars)</summary><pre>${j.block || "(empty: nothing is injected yet)"}</pre></details>
      <small class="muted">Journal started: ${new Date(j.resetAt).toLocaleTimeString()} (${j.resetReason ?? "new"}).${j.lastError ? " Last problem: " + j.lastError : ""}</small>
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
    ${!names.length && html`<section class="panel"><h2>HQ journal <span class="sub">long-term memory of each commander</span></h2><div class="empty">No journal yet: it starts with the first situation of a running war (settings: Usage & Config, group "memory").</div></section>`}
  </main>`;
}

// Settings block for the Config form: setG(group, key, value); f.memory = {enabled, reviewMin}
export function MemorySettings({ f, setG }) {
  const m = f.memory ?? {};
  return html`<h3 class="cfgh">HQ memory (war journal)</h3>
    <div class="pair">
      <label class="switch"><input type="checkbox" checked=${m.enabled !== false} onChange=${(e) => setG("memory", "enabled", e.target.checked)} /><span>Keep a journal (intent, goals, lessons, failed attacks) and show it to the HQ</span></label>
      <label class="field"><span>Review every (min)</span><input type="number" min="1" max="60" value=${m.reviewMin ?? 5} onInput=${(e) => setG("memory", "reviewMin", e.target.value)} /></label>
    </div>
    <small class="muted">One small extra model call per faction every review interval writes the three journal slots; the attack record is counted by code. Off: no review calls and nothing is added to the prompts. The journal restarts with every war. View: Journal tab.</small>`;
}
