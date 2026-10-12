// HQ war journal, simple first version (M4 round 3 WP5; design: dev/VISION.md section 1c, dev/notes/m4-round3-plan.md WP5).
// Pure functions + plain data, no I/O: the sidecar (llm_sidecar.mjs) owns the per-faction journals, the review call and the logging.
//
// Slots (caps below), ~150 tokens in the prompt:
//   intent  (model)  how we win, 1 x 120 chars
//   goals   (model)  3 x 40 chars, each open|done|failed
//   lessons (model)  3 x 60 chars
//   tries   (CODE)   per objective: attacks tried / failed / done / last result; shown only for targets with >= 1 fail, max 5 lines
// The model never counts. `tries` come from the ATTACK orders the sidecar sent plus the game's outcome events:
//   "outcome <squad> <ATTACK|DEFEND> <target|-> <done|failed>: <why> (after N s)"   (CLW_Orders.Finish)
//   "<squad> lost"                                                                   (CLW_RuleCommander.Sync)
//   "<id> <name> captured by <faction>"                                             (CLW_WarState)
// The three model slots are written only by a small review call every memory.reviewMin minutes: reviewSystem() + reviewUser() ->
// JSON -> validateReview() (truncate over-long strings, reject structurally invalid replies -> the caller keeps the old journal).

export const CAPS = { intent: 120, goals: 3, goalText: 40, lessons: 3, lessonText: 60, triesLines: 5, events: 12 };
export const GOAL_STATES = ["open", "done", "failed"];
const ATTACK_CLOSED_KEEP_SEC = 90;   // an outcome for a just-closed attack record is a duplicate of the event that closed it
const ATTACK_STALE_SEC = 1200;       // an attack record without any outcome this long is dropped silently
const isNum = (n) => typeof n === "number" && Number.isFinite(n);
const oneLine = (s) => String(s ?? "").replace(/\s+/g, " ").trim();
const clip = (s, n) => { const t = oneLine(s); return t.length > n ? t.slice(0, n - 1).trimEnd() + "." : t; };

export function emptyJournal() {
  return { intent: "", goals: [], lessons: [], tries: {}, attacks: {}, closed: {}, events: [],
    reviews: 0, rejected: 0, reviewedAt: 0, reviewWarTime: null, nextReviewAt: 0, reviewing: false, lastError: "",
    lastTime: null, lastPhase: "", lastTimeLeft: null, warId: "", active: false, resetAt: Date.now(), resetReason: "new" };
}

// ---------- war boundaries ----------
// Returns a reason string when this situation starts a new war for the journal (the caller then replaces it with a fresh one), else "".
// Rematch in place keeps the world clock running, so the phase going back to SETUP is the main signal; a game time that goes back,
// a war id that changes (war.id / warId, when the game sends one) and the time limit jumping up are the others.
export function newWarReason(j, sit) {
  const time = isNum(sit?.time) ? sit.time : null, phase = typeof sit?.war?.phase === "string" ? sit.war.phase : "running";
  const left = isNum(sit?.timeLeftSec) ? sit.timeLeftSec : null;
  const warId = sit?.war?.id !== undefined ? String(sit.war.id) : sit?.warId !== undefined ? String(sit.warId) : "";
  let why = "";
  if (j.lastTime !== null && time !== null && time < j.lastTime - 5) why = `game time went back (${Math.round(j.lastTime)} -> ${Math.round(time)})`;
  else if (warId && j.warId && warId !== j.warId) why = `war id changed (${j.warId} -> ${warId})`;
  else if (phase === "setup" && j.active) why = "game is back in the setup phase";
  else if (left !== null && j.lastTimeLeft !== null && left > j.lastTimeLeft + 30 && left > 0) why = `time limit restarted (${Math.round(j.lastTimeLeft)} -> ${Math.round(left)} s left)`;
  return why;
}
// Bookkeeping after every situation (also when no reset happened).
export function noteSituation(j, sit) {
  if (isNum(sit?.time)) j.lastTime = sit.time;
  if (isNum(sit?.timeLeftSec)) j.lastTimeLeft = sit.timeLeftSec;
  const phase = typeof sit?.war?.phase === "string" ? sit.war.phase : "running";
  j.lastPhase = phase;
  if (phase === "running") j.active = true;
  const warId = sit?.war?.id !== undefined ? String(sit.war.id) : sit?.warId !== undefined ? String(sit.warId) : "";
  if (warId) j.warId = warId;
}

// ---------- objectives ----------
function objMap(sit) {
  const m = new Map();
  for (const o of Array.isArray(sit?.objectives) ? sit.objectives : []) if (o && typeof o.id === "string") m.set(o.id, o);
  return m;
}
const xz = (p) => (Array.isArray(p) && p.length >= 2 && p.every(isNum) ? (p.length > 2 ? [p[0], p[2]] : [p[0], p[1]]) : null);
// The objective an ATTACK order aims at: its target id, else the objective whose area (radius + 150 m) contains the order position.
export function resolveTarget(order, sit) {
  const objs = objMap(sit);
  if (typeof order?.target === "string" && objs.has(order.target)) return objs.get(order.target);
  const p = xz(order?.pos);
  if (!p) return null;
  let best = null, bd = Infinity;
  for (const o of objs.values()) {
    const q = xz(o.pos);
    if (!q) continue;
    const d = Math.hypot(p[0] - q[0], p[1] - q[1]);
    if (d <= (isNum(o.radius) ? o.radius : 80) + 150 && d < bd) { best = o; bd = d; }
  }
  return best;
}
function triesFor(j, obj) {
  const e = (j.tries[obj.id] ??= { id: obj.id, name: obj.name ?? "", tried: 0, failed: 0, done: 0, last: "", lastAt: null });
  if (obj.name) e.name = obj.name;
  return e;
}

// ---------- tries (code) ----------
// Called with the orders the sidecar just sent (validated). A new ATTACK on an objective is one try; re-sending the same target to the same squad is not.
export function noteOrders(j, orders, sit, log = () => {}) {
  for (const o of Array.isArray(orders) ? orders : []) {
    if (o?.command !== "ATTACK" || typeof o.squad !== "string") continue;
    const t = resolveTarget(o, sit);
    if (!t) continue;
    const cur = j.attacks[o.squad];
    if (cur && cur.target === t.id) continue;
    if (cur) log(`attack of ${o.squad} on ${cur.target} replaced by ${t.id} (no result)`);
    j.attacks[o.squad] = { target: t.id, sentAt: isNum(sit?.time) ? sit.time : 0 };
    const e = triesFor(j, t);
    e.tried++;
    log(`try ATTACK ${t.id} by ${o.squad}: tried ${e.tried} failed ${e.failed}`);
  }
}
function closeAttack(j, squad, now) {
  const rec = j.attacks[squad];
  delete j.attacks[squad];
  if (rec) j.closed[squad] = { target: rec.target, at: now };
  return rec;
}
function pruneAttacks(j, now) {
  for (const [s, r] of Object.entries(j.attacks)) if (now - r.sentAt > ATTACK_STALE_SEC) delete j.attacks[s];
  for (const [s, r] of Object.entries(j.closed)) if (now - r.at > ATTACK_CLOSED_KEEP_SEC) delete j.closed[s];
}
function shortWhy(w) {
  return oneLine(w).replace(/\s*\(after \d+ s\)\s*$/, "").slice(0, 40);
}
function record(j, tid, sit, ok, why, log) {
  const obj = objMap(sit).get(tid) ?? { id: tid };
  const e = triesFor(j, obj);
  if (ok) e.done++; else e.failed++;
  e.last = ok ? "captured" : shortWhy(why) || "failed";
  e.lastAt = isNum(sit?.time) ? sit.time : null;
  log(`outcome ${tid}: ${ok ? "done" : "failed"} (${e.last}) -> tried ${e.tried} failed ${e.failed} done ${e.done}`);
}

// Feeds the faction's new game events (strings since the last POST) into the journal. prevSit = the previous situation of this faction (positions).
export function ingestEvents(j, events, sit, prevSit = null, log = () => {}) {
  const now = isNum(sit?.time) ? sit.time : 0, me = sit?.faction;
  for (const raw of Array.isArray(events) ? events : []) {
    if (typeof raw !== "string" || !raw) continue;
    const text = oneLine(raw);
    j.events.push(text.slice(0, 140));
    if (j.events.length > CAPS.events) j.events.splice(0, j.events.length - CAPS.events);
    let m = /^outcome (\S+) (ATTACK|DEFEND|MOVE|RECON|FALL_BACK) (\S+) (done|failed)(?:: (.*))?$/.exec(text);
    if (m) {
      const [, squad, cmd, target, status, why] = m;
      if (cmd !== "ATTACK") continue;
      const rec = closeAttack(j, squad, now);
      const tid = rec?.target ?? (target !== "-" ? target : "");
      if (!tid) continue;
      if (!rec) {
        const c = j.closed[squad];
        // already counted via "<squad> lost" / "captured" a moment ago
        if (c && c.target === tid && now - c.at < ATTACK_CLOSED_KEEP_SEC && !rec) { j.closed[squad] = { target: tid, at: now }; continue; }
        const e = triesFor(j, objMap(sit).get(tid) ?? { id: tid });
        e.tried++; // an attack the sidecar did not send (rule commander, bought squad): counted when it ends
        j.closed[squad] = { target: tid, at: now };
      }
      record(j, tid, sit, status === "done", why ?? "", log);
      continue;
    }
    m = /^(\S+) lost$/.exec(text);
    if (m) {
      const squad = m[1], rec = closeAttack(j, squad, now);
      if (!rec) continue;
      const obj = objMap(sit).get(rec.target), prev = (Array.isArray(prevSit?.squads) ? prevSit.squads : []).find((s) => s?.id === squad);
      const a = xz(prev?.pos), b = xz(obj?.pos);
      const d = a && b ? Math.round(Math.hypot(a[0] - b[0], a[1] - b[1])) : null;
      record(j, rec.target, sit, false, d !== null && d <= 600 ? "squad lost near target" : "squad lost en route", log);
      continue;
    }
    m = /^(\S+)(?: .*)? captured by (\S+)$/.exec(text);
    if (m && m[2] === me) {
      const tid = m[1];
      for (const [squad, rec] of Object.entries(j.attacks)) if (rec.target === tid) { closeAttack(j, squad, now); record(j, tid, sit, true, "", log); }
    }
  }
  pruneAttacks(j, now);
}

// Display lines of the code-kept record: targets with >= 1 fail, worst first, at most CAPS.triesLines. A target we took and still hold is left out.
export function triesLines(j, sit) {
  const objs = objMap(sit), now = isNum(sit?.time) ? sit.time : null;
  const rows = Object.values(j.tries).filter((e) => e.failed >= 1 && !(e.last === "captured" && objs.get(e.id)?.owner === sit?.faction));
  rows.sort((a, b) => b.failed - a.failed || (b.lastAt ?? 0) - (a.lastAt ?? 0));
  return rows.slice(0, CAPS.triesLines).map((e) => {
    const ago = now !== null && isNum(e.lastAt) ? `, ${Math.max(0, Math.round((now - e.lastAt) / 60))} min ago` : "";
    return `${e.id}${e.name ? " " + e.name : ""}: ${e.tried} attacks, ${e.failed} failed, last: ${e.last || "-"}${ago}`;
  });
}

// ---------- prompt block ----------
// The one HQ JOURNAL block of the commander / HQ chat prompt. "" when the journal is still empty.
export function renderBlock(j, sit) {
  const tries = triesLines(j, sit);
  if (!j.intent && !j.goals.length && !j.lessons.length && !tries.length) return "";
  const lines = ["HQ JOURNAL (your own notes; the attack record is counted by the game)"];
  if (j.intent) lines.push("Intent: " + j.intent);
  if (j.goals.length) lines.push("Goals: " + j.goals.map((g, i) => `${i + 1}. [${g.status}] ${g.text}`).join("; "));
  if (j.lessons.length) lines.push("Lessons: " + j.lessons.map((l) => "- " + l).join(" "));
  if (tries.length) lines.push("Failed attacks:\n" + tries.map((t) => "- " + t).join("\n"));
  return lines.join("\n");
}

export const JOURNAL_RULE = "HQ JOURNAL: below the situation you get your own short notes (intent, goals, lessons) and the game-counted record of failed attacks. Stick to your intent; do not repeat an attack that failed twice without changing the approach (flank, armour, FOB, more force, other target). You do not write the journal; a separate review does.";
export const FLANK_PROMPT = "Consider flanks: a FOB or a route off the direct axis can beat a frontal push. Each objective has side left|centre|right of the line from your HQ to the enemy HQ (seen from your HQ, facing the enemy).";
export const SQUAD_NAME_PROMPT = "Squads are ordered by id; name is for display and radio only.";

// ---------- review call ----------
export function reviewSystem() {
  return `You keep the war journal of one faction's HQ (tactical war simulation). Update it from the facts you are given. Reply with ONLY one JSON object, no prose, no code fences:
{"intent":"<how we win, max ${CAPS.intent} chars>","goals":[{"text":"<max ${CAPS.goalText} chars>","status":"open|done|failed"}],"lessons":["<max ${CAPS.lessonText} chars>"]}
Rules: at most ${CAPS.goals} goals and ${CAPS.lessons} lessons. Update, do not restart: keep what is still right, mark goals done or failed only from facts (objective owners, the attack record), replace finished goals by new ones. A lesson is a concrete fact you can act on ("frontal on C failed 2x: try a flank"), only from the attack record or events. Use objective letters or names, never coordinates. Do not write the attack record yourself.`;
}
// Short situation summary for the review call (kept to a few hundred tokens).
export function summarySituation(sit, j) {
  const w = sit?.war ?? {};
  const out = [`time ${Math.round(sit?.time ?? 0)} s${isNum(sit?.timeLeftSec) && sit.timeLeftSec >= 0 ? `, ${Math.round(sit.timeLeftSec)} s left` : ""}; tickets ${w.tickets ?? "?"} vs ${w.enemyTickets ?? "?"}; supplies ${w.supplies ?? "?"}`];
  const objs = (Array.isArray(sit?.objectives) ? sit.objectives : []).filter((o) => o && typeof o.id === "string").slice(0, 12);
  if (objs.length) out.push("objectives: " + objs.map((o) => `${o.id} ${o.name ?? ""} ${o.owner === sit.faction ? "ours" : o.owner === "none" || !o.owner ? "neutral" : "enemy"}${o.contested ? " contested" : ""}${o.side ? " " + o.side : ""}`.replace(/\s+/g, " ")).join("; "));
  const squads = (Array.isArray(sit?.squads) ? sit.squads : []).filter((s) => s && !s.humanLed);
  const byTarget = new Map();
  for (const s of squads) { const k = s.objective ? `${s.objective.command} ${s.objective.target ?? "pos"}` : "no order"; byTarget.set(k, (byTarget.get(k) ?? 0) + 1); }
  out.push(`own squads ${squads.length}: ` + ([...byTarget.entries()].map(([k, n]) => `${n}x ${k}`).join(", ") || "none"));
  out.push(`known enemy contacts ${Array.isArray(sit?.contacts) ? sit.contacts.length : 0}`);
  const fobs = Array.isArray(sit?.fobs) ? sit.fobs : [];
  if (fobs.length) out.push("FOBs: " + fobs.map((f) => `${f.id} ${f.state}`).join(", "));
  if (j.events.length) out.push("recent events: " + j.events.slice(-8).join(" | "));
  return out.join("\n");
}
export function reviewUser(sit, j) {
  const cur = { intent: j.intent, goals: j.goals, lessons: j.lessons };
  const tries = triesLines(j, sit);
  return `SITUATION\n${summarySituation(sit, j)}\n\nATTACK RECORD (counted by the game)\n${tries.length ? tries.join("\n") : "no failed attacks"}\n\nCURRENT JOURNAL\n${JSON.stringify(cur)}\n\nWrite the updated journal JSON.`;
}
// A fixed journal for the fake provider (deterministic, no network).
export function fakeReview() {
  return { intent: "Fake HQ: hold what we own, take the nearest enemy objective", goals: [{ text: "Take the nearest enemy objective", status: "open" }, { text: "Keep tickets ahead of the enemy", status: "open" }], lessons: ["Do not repeat a twice-failed attack unchanged"] };
}

// Parses + validates a review reply (text or object). -> {value:{intent,goals,lessons}, notes:[...]} | {error}
// Over-long strings and surplus list entries are truncated / dropped (listed in notes); structural problems reject the whole review.
export function validateReview(input) {
  let obj = input;
  if (typeof input === "string") {
    const a = input.indexOf("{"), b = input.lastIndexOf("}");
    if (a < 0 || b < a) return { error: "no JSON object in the reply" };
    try { obj = JSON.parse(input.slice(a, b + 1)); } catch (e) { return { error: "invalid JSON: " + e.message }; }
  }
  if (!obj || typeof obj !== "object" || Array.isArray(obj)) return { error: "reply is not a JSON object" };
  const notes = [];
  if (typeof obj.intent !== "string") return { error: "intent must be a string" };
  if (!Array.isArray(obj.goals)) return { error: "goals must be an array" };
  if (!Array.isArray(obj.lessons)) return { error: "lessons must be an array" };
  const intent = oneLine(obj.intent);
  if (intent.length > CAPS.intent) notes.push(`intent truncated ${intent.length} -> ${CAPS.intent}`);
  const goals = [];
  for (const g of obj.goals) {
    const text = typeof g === "string" ? g : g && typeof g === "object" ? g.text : undefined;
    const status = typeof g === "string" ? "open" : g?.status === undefined ? "open" : g?.status;
    if (typeof text !== "string") return { error: "a goal needs a text string" };
    if (!GOAL_STATES.includes(status)) return { error: `bad goal status ${JSON.stringify(status)}` };
    if (!oneLine(text)) continue;
    if (goals.length >= CAPS.goals) { notes.push("more than 3 goals: extra dropped"); break; }
    if (oneLine(text).length > CAPS.goalText) notes.push(`goal truncated ${oneLine(text).length} -> ${CAPS.goalText}`);
    goals.push({ text: clip(text, CAPS.goalText), status });
  }
  const lessons = [];
  for (const l of obj.lessons) {
    if (typeof l !== "string") return { error: "a lesson must be a string" };
    if (!oneLine(l)) continue;
    if (lessons.length >= CAPS.lessons) { notes.push("more than 3 lessons: extra dropped"); break; }
    if (oneLine(l).length > CAPS.lessonText) notes.push(`lesson truncated ${oneLine(l).length} -> ${CAPS.lessonText}`);
    lessons.push(clip(l, CAPS.lessonText));
  }
  if (!intent && !goals.length && !lessons.length) return { error: "review is empty" };
  return { value: { intent: clip(intent, CAPS.intent), goals, lessons }, notes };
}

// Dashboard / GET /api/state view of one journal (no internal bookkeeping maps).
export function view(j, sit, enabled) {
  const block = renderBlock(j, sit);
  return { enabled, intent: j.intent, goals: j.goals, lessons: j.lessons, tries: Object.values(j.tries).sort((a, b) => b.failed - a.failed), triesLines: triesLines(j, sit),
    openAttacks: Object.entries(j.attacks).map(([squad, r]) => ({ squad, target: r.target, sentAt: r.sentAt })),
    block, blockChars: block.length, reviews: j.reviews, rejected: j.rejected, reviewedAt: j.reviewedAt || null, reviewWarTime: j.reviewWarTime, nextReviewAt: j.nextReviewAt || null,
    reviewing: j.reviewing, lastError: j.lastError, resetAt: j.resetAt, resetReason: j.resetReason, recentEvents: j.events };
}
