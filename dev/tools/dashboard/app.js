// Clausewitz HQ dashboard. Preact + htm (vendored, offline). All game text is rendered through Preact
// (auto-escaped); no dangerouslySetInnerHTML anywhere.
import { html, render, useState, useEffect, useRef, useMemo, useCallback } from "/vendor/standalone.module.js";

const WORLD = 4096, GRID = 512, STALE_MS = 30000; // WORLD = fallback extent until /api/world reports the real one
const DEF_EXT = { x0: 0, z0: 0, x1: WORLD, z1: WORLD };
// Faction order and colours come from the game: situation.side (0 = side A) and situation.identity (BLUFOR / OPFOR / INDFOR),
// plus war.sides. BLUFOR blue, OPFOR red, INDFOR green; other identity: side A blue / side B red / grey. Without those fields
// (older game build) the old behaviour applies: colours from the alphabetically sorted keys (first blue, second red, then a palette).
const PALETTE = ["#4aa3ff", "#ff5c5c", "#f0b429", "#3fcf8e", "#b57bff", "#ff8fd1", "#8793a6"];
const NEUTRAL = "#8793a6";
const IDENT_PAL = { BLUFOR: 0, OPFOR: 1, INDFOR: 3 };
let FACTIONS = []; // faction keys seen in the state, side A first (setFactions)
let SIDE = new Map(), IDENT = new Map(); // key -> side index / identity text (only when the game reported them)
function setFactions(st) {
  const set = new Set(Object.keys(st.factions ?? {}));
  SIDE = new Map(); IDENT = new Map();
  for (const e of Object.values(st.factions ?? {})) {
    const sit = e.situation ?? {};
    for (const b of Array.isArray(sit.bases) ? sit.bases : []) if (typeof b?.faction === "string") set.add(b.faction);
    for (const o of Array.isArray(sit.objectives) ? sit.objectives : []) if (typeof o?.owner === "string" && o.owner !== "none" && o.owner !== "") set.add(o.owner);
    if (Array.isArray(sit.war?.sides)) sit.war.sides.forEach((x, i) => { if (typeof x?.faction === "string") { set.add(x.faction); SIDE.set(x.faction, i); if (typeof x.identity === "string" && x.identity) IDENT.set(x.faction, x.identity); } });
  }
  for (const [f, e] of Object.entries(st.factions ?? {})) {
    if (num(e.side ?? e.situation?.side) && (e.side ?? e.situation.side) >= 0) SIDE.set(f, e.side ?? e.situation.side);
    const id = e.identity ?? e.situation?.identity;
    if (typeof id === "string" && id) IDENT.set(f, id);
  }
  FACTIONS = [...set].sort(facCmp);
}
// side A first, unknown side last, then alphabetical
const facCmp = (a, b) => (SIDE.has(a) ? SIDE.get(a) : 99) - (SIDE.has(b) ? SIDE.get(b) : 99) || (a < b ? -1 : a > b ? 1 : 0);
const sortedKeys = (o) => Object.keys(o ?? {}).sort(facCmp);
const facIdx = (f) => FACTIONS.indexOf(f);
// palette index of a faction; -1 = unknown faction
function palIdx(f) {
  const id = String(IDENT.get(f) ?? "").toUpperCase();
  if (id in IDENT_PAL) return IDENT_PAL[id];
  if (SIDE.has(f)) return SIDE.get(f) === 0 ? 0 : SIDE.get(f) === 1 ? 1 : 6;
  if (SIDE.size) return facIdx(f) < 0 ? -1 : 6; // game reports sides: a faction without a side is grey
  const i = facIdx(f); return i < 0 ? -1 : i % 6;
}
const colorOf = (f) => { const i = palIdx(f); return i < 0 ? NEUTRAL : PALETTE[i]; };
const enemyColor = (f) => { const o = FACTIONS.find((x) => x !== f); return o === undefined ? "#f0b429" : colorOf(o); };
const facClass = (f) => { const i = palIdx(f); return i < 0 ? "f-none" : "f-c" + i; };
const KIND_LABEL = { infantry: "INF", recon: "RECON", motorized: "MOT", light: "LIGHT", mech: "MECH", armor: "ARMOR", armored_recon: "A-RECON" };
const kindLabel = (k) => (typeof k === "string" && k ? KIND_LABEL[k] ?? k.toUpperCase().slice(0, 8) : "-");
const fmtTime = (sec) => (num(sec) ? `${Math.floor(sec / 60)}:${String(Math.floor(sec % 60)).padStart(2, "0")}` : "-");
// public picture (objectives, bases, war) from the freshest v2 situation of any faction
function publicPicture(st) {
  let objs = [], bases = [], ts = -1, viewer = null; // viewer = the faction whose point of view the objective progress is in
  for (const [fk, e] of Object.entries(st.factions ?? {})) {
    const sit = e.situation ?? {};
    if (Array.isArray(sit.objectives) && sit.objectives.length && (e.ts ?? 0) >= ts) { objs = sit.objectives; ts = e.ts ?? 0; viewer = fk; }
  }
  const seen = new Set();
  for (const e of Object.values(st.factions ?? {})) for (const b of Array.isArray(e.situation?.bases) ? e.situation.bases : []) {
    if (b && typeof b.faction === "string" && Array.isArray(b.pos) && num(b.pos[0]) && num(b.pos[1]) && !seen.has(b.faction)) { seen.add(b.faction); bases.push(b); }
  }
  return { objs: objs.filter((o) => o && Array.isArray(o.pos) && num(o.pos[0]) && num(o.pos[1])), bases, viewer };
}
const num = (v) => typeof v === "number" && Number.isFinite(v);
const okPos = (p) => Array.isArray(p) && p.length >= 3 && num(p[0]) && num(p[2]);
const gridTxt = (p) => (okPos(p) ? `${Math.round(p[0])}, ${Math.round(p[2])}` : "-");
const shortId = (id) => String(id).replace(/^CLW_/i, "").replace(/Squad_?/i, "");
const dash = (v) => (v === undefined || v === null || v === "" ? "-" : v);
// Activity (AI LOD): a unit is "dormant" when all its agents sit in the engine's max AI LOD (game field squad.dormant). Strong faction colour =
// active, the same colour at ~40 % alpha (plus a hollow-ish outline) = dormant. Missing field (older game build) = unknown, treated as active.
const hex2rgb = (h) => { const n = parseInt(String(h).slice(1), 16); return [(n >> 16) & 255, (n >> 8) & 255, n & 255]; };
const tint = (col, a) => { const [r, g, b] = hex2rgb(col); return `rgba(${r},${g},${b},${a})`; };
const DORM_A = 0.4;
const actOK = (act, dm) => act === "all" || (act === "dormant" ? dm : !dm);
const accent = (f) => ({ borderTop: `3px solid ${colorOf(f)}` });
const PARAM_LABEL = { timeLimitMin: "Time limit (min)", startTickets: "Start tickets", startSupplies: "Start supplies", incomeMultiplier: "Income multiplier", battleSize: "Battle size", ticketBleed: "Ticket bleed" };
const humanKey = (k) => PARAM_LABEL[k] ?? String(k).replace(/([A-Z])/g, " $1").toLowerCase().replace(/^./, (c) => c.toUpperCase());
const fmtVal = (v) => (typeof v === "boolean" ? (v ? "yes" : "no") : v !== null && typeof v === "object" ? JSON.stringify(v) : v === undefined || v === null || v === "" ? "-" : String(v));

// ---------- icons (inline SVG) ----------
const P = {
  logo: "M12 2 3 7v6c0 5 3.5 8 9 9 5.5-1 9-4 9-9V7z",
  save: "M5 3h11l3 3v15H5zM8 3v5h7V3M8 21v-7h8v7",
  plug: "M9 2v6M15 2v6M6 8h12v4a6 6 0 0 1-12 0zM12 18v4",
  crosshair: "M12 3v4M12 17v4M3 12h4M17 12h4M12 12m-5 0a5 5 0 1 0 10 0a5 5 0 1 0-10 0",
  reset: "M3 12a9 9 0 1 0 3-6.7L3 8M3 3v5h5",
};
const Icon = ({ n }) => html`<svg class="ic" viewBox="0 0 24 24" aria-hidden="true"><path d=${P[n]} /></svg>`;

// ---------- map symbols ----------
// Squad symbol path by unit kind (generic kinds; unknown kinds get the infantry circle). The caller fills / strokes the current path.
function kindShape(ctx, kind, x, y) {
  ctx.beginPath();
  switch (kind) {
    case "recon": ctx.moveTo(x, y - 8); ctx.lineTo(x + 6, y); ctx.lineTo(x, y + 8); ctx.lineTo(x - 6, y); ctx.closePath(); break; // narrow diamond
    case "armored_recon": ctx.moveTo(x, y - 9); ctx.lineTo(x + 9, y); ctx.lineTo(x, y + 9); ctx.lineTo(x - 9, y); ctx.closePath(); break; // big diamond
    case "motorized": ctx.rect(x - 6, y - 5, 12, 10); break; // box (truck)
    case "light": ctx.moveTo(x, y - 8); ctx.lineTo(x + 7, y + 6); ctx.lineTo(x - 7, y + 6); ctx.closePath(); break; // triangle
    case "mech": ctx.rect(x - 8, y - 7, 16, 14); break; // large box
    case "armor": ctx.moveTo(x - 10, y - 5); ctx.lineTo(x + 10, y - 5); ctx.lineTo(x + 10, y + 5); ctx.lineTo(x - 10, y + 5); ctx.closePath(); break; // wide hull
    default: ctx.arc(x, y, 7, 0, Math.PI * 2);
  }
}
let hatchCache = null; // diagonal hatch pattern for contested objectives
function hatchPattern(ctx) {
  if (hatchCache) return hatchCache;
  try {
    const c = document.createElement("canvas"); c.width = c.height = 8;
    const g = c.getContext("2d"); g.strokeStyle = "#fff"; g.lineWidth = 1.5; g.beginPath(); g.moveTo(0, 8); g.lineTo(8, 0); g.moveTo(-2, 2); g.lineTo(2, -2); g.moveTo(6, 10); g.lineTo(10, 6); g.stroke();
    hatchCache = ctx.createPattern(c, "repeat");
  } catch { hatchCache = null; }
  return hatchCache;
}

// ---------- data ----------
// Only the factions of the CURRENT war are shown (server-side `activeFactions`: same war epoch, posted within staleAfterSec of the newest post).
// showInactive keeps everything (debugging). Without activeFactions (older sidecar) nothing is hidden.
function deriveState(raw, showInactive) {
  const all = Object.keys(raw.factions ?? {});
  if (!Array.isArray(raw.activeFactions) || showInactive) return { ...raw, hidden: [] };
  const act = new Set(raw.activeFactions), pick = (o) => Object.fromEntries(Object.entries(o ?? {}).filter(([k]) => act.has(k)));
  return { ...raw, factions: pick(raw.factions), scheduler: pick(raw.scheduler), exchanges: (raw.exchanges ?? []).filter((x) => act.has(x.faction)), hidden: all.filter((k) => !act.has(k)) };
}
function useState2(showInactive) {
  const [raw, setRaw] = useState({ offline: true, factions: {}, exchanges: [], counters: {}, config: {} });
  const poll = useCallback(async () => {
    try {
      const r = await fetch("/api/state", { cache: "no-store" });
      const j = await r.json();
      setRaw({ ...j, offline: false, recvAt: Date.now() });
    } catch { setRaw((s) => ({ ...s, offline: true })); }
  }, []);
  useEffect(() => { poll(); const t = setInterval(poll, 2000); return () => clearInterval(t); }, [poll]);
  const st = useMemo(() => { const d = deriveState(raw, showInactive); setFactions(d); return d; }, [raw, showInactive]);
  return [st, poll];
}

function useToasts() {
  const [list, setList] = useState([]);
  const push = useCallback((text, kind = "ok") => {
    const id = Math.random();
    setList((l) => [...l, { id, text, kind }]);
    setTimeout(() => setList((l) => l.filter((t) => t.id !== id)), 4500);
  }, []);
  return [list, push];
}

// ---------- derived squad rows ----------
function buildRows(state, faction) {
  const now = state.now ?? Date.now(), rows = [];
  for (const f of Object.keys(state.factions ?? {})) {
    if (faction !== "all" && f !== faction) continue;
    const e = state.factions[f], sit = e.situation ?? {};
    const orders = new Map((e.orders ?? []).map((o) => [o.squad, o]));
    for (const s of Array.isArray(sit.squads) ? sit.squads : []) {
      if (!s || typeof s !== "object") continue;
      const o = orders.get(s.id);
      const obj = s.objective && typeof s.objective === "object" ? s.objective : o ? { command: o.command, pos: o.pos, target: o.target, reason: o.reason } : null;
      const pct = num(s.agents) && num(s.startAgents) && s.startAgents > 0 ? Math.round((100 * s.agents) / s.startAgents) : null;
      let speed = num(s.speed) ? s.speed : null, heading = num(s.heading) ? s.heading : null, derived = false;
      const pv = e.prevPos?.[s.id];
      if (speed === null && pv && okPos(s.pos) && okPos(pv.pos)) {
        const dt = num(sit.time) && num(pv.time) && sit.time !== pv.time ? sit.time - pv.time : (e.ts - pv.ts) / 1000;
        if (dt > 0) {
          const dx = s.pos[0] - pv.pos[0], dz = s.pos[2] - pv.pos[2];
          speed = Math.hypot(dx, dz) / dt; derived = true;
          if (heading === null && speed > 0.3) heading = (Math.atan2(dx, dz) * 180 / Math.PI + 360) % 360;
        }
      }
      const moving = speed !== null && speed > 0.3;
      const move = speed === null ? "-" : moving ? `${speed.toFixed(1)} m/s${heading !== null ? " " + Math.round(heading) + "°" : ""}${derived ? " ~" : ""}` : "stationary";
      const state_ = s.state ? String(s.state) : s.agents === 0 ? "dead" : moving ? "moving" : s.mode === "ATTACK" ? "engaged" : s.mode === "DEFEND" ? "defending" : "idle";
      const ots = e.orderTs?.[s.id];
      rows.push({
        key: f + "/" + s.id, id: String(s.id), faction: f, type: dash(s.type), agents: s.agents, size: num(s.agents) ? `${s.agents}/${s.startAgents}` : "-",
        pct, wounded: num(s.wounded) ? s.wounded : null, health: num(s.health) ? s.health : null, obj, state: state_, move, speed: speed ?? -1,
        human: !!s.humanLed, pos: s.pos, posTxt: gridTxt(s.pos), orderAge: ots ? Math.round((now - ots) / 1000) : null, mode: s.mode,
        dormant: typeof s.dormant === "boolean" ? s.dormant : null, reason: o?.reason ?? obj?.reason,
        kind: typeof s.kind === "string" ? s.kind : "", vehicle: s.vehicle && typeof s.vehicle === "object" ? s.vehicle : null,
        mission: s.mission && typeof s.mission === "object" && s.mission.state && s.mission.state !== "none" ? s.mission : null,
      });
    }
  }
  return rows;
}

// ---------- status strip ----------
// Per-faction HQ scheduler chip: live countdown between /api/state polls, tooltip with the scheduler details, "wake now" button.
function WakeChip({ name, s, recvAt, toast, refresh, paused, engine }) {
  const [, tick] = useState(0);
  useEffect(() => { const t = setInterval(() => tick((n) => n + 1), 1000); return () => clearInterval(t); }, []);
  const left = Math.max(0, Math.round(s.nextDueInSec - (Date.now() - (recvAt ?? Date.now())) / 1000));
  const wake = async () => {
    try {
      const { ok, j } = await postJson("/api/wake", { faction: name });
      toast(ok ? `${name} HQ will be consulted on the next poll` : "Wake failed: " + (j.error ?? "request failed"), ok ? "ok" : "err");
      refresh();
    } catch (e) { toast("Error: " + e.message, "err"); }
  };
  const trig = s.lastWakeTriggers?.length ? ` (${s.lastWakeTriggers.join("; ")})` : "";
  const tip = [
    `Interval: ${dash(s.intervalSec)} s (chosen ${dash(s.chosenSec)} s${s.chosenReason ? ": " + s.chosenReason : ""}), budget stretch x${(s.stretch ?? 1).toFixed(2)}`,
    `Last wake: ${s.lastWakeReason ?? "-"}${trig}`,
    `Gated polls (no LLM call): ${s.gated ?? 0}${s.failed ? "; last call failed, rule fallback active" : ""}`,
  ].join("\n");
  const idle = engine && engine !== "llm";
  const txt = idle ? "rules" : paused ? "paused" : s.manual ? "wake pending" : left > 0 ? `in ${left} s` : "due";
  return html`<div class=${"chip wake" + (s.failed && !idle && !paused ? " bad" : "")} title=${tip}><b class=${facClass(name)}>${name}</b> next HQ call <b>${txt}</b>
    <button class="mini" type="button" disabled=${idle || paused} onClick=${wake} title="Call the LLM on the next poll">wake now</button></div>`;
}

// Global HQ controls: pause toggle (runtime) and engine selector (persisted config).
function HqControls({ st, toast, refresh }) {
  const engine = st.config?.engine ?? "llm", paused = !!st.paused;
  const togglePause = async () => {
    try {
      const { ok, j } = await postJson("/api/pause", { paused: !paused });
      if (!ok) toast("Pause failed: " + (j.error ?? "request failed"), "err");
      refresh();
    } catch (e) { toast("Error: " + e.message, "err"); }
  };
  const setEngine = async (e) => {
    try {
      const { ok, j } = await postJson("/api/config", { engine: e.target.value });
      if (!ok) toast("Engine not changed: " + (j.error ?? "request failed"), "err");
      refresh();
    } catch (err) { toast("Error: " + err.message, "err"); }
  };
  return html`<div class="chip hqctl">
    <button class=${"mini pausebtn" + (paused ? " on" : "")} type="button" onClick=${togglePause} title=${paused ? "Resume: the HQ is consulted on the next poll" : "Pause: no LLM calls, the game keeps its current orders"}>${paused ? "resume" : "pause"}</button>
    <label class="engsel" title="HQ engine: llm = LLM decides; rules = game rule commander, no LLM calls; model = trained decision model (not available yet)">engine
      <select value=${engine} onChange=${setEngine}>
        <option value="llm">llm</option><option value="rules">rules</option><option value="model" disabled>model (soon)</option></select></label></div>`;
}

function Strip({ st, toast, refresh }) {
  const c = st.counters ?? {}, cf = st.config ?? {}, bud = st.usage?.budget;
  const age = st.lastRequestTs ? ((st.now ?? Date.now()) - st.lastRequestTs) / 1000 : null;
  const stale = st.offline || age === null || age * 1000 > STALE_MS;
  const ageTxt = st.offline ? "sidecar unreachable" : age === null ? "no request yet" : `${age.toFixed(0)} s ago`;
  const prov = cf.provider ? (cf.provider === "fake" ? "fake" : `${cf.provider} / ${cf.model}`) : "?";
  return html`<div class="strip">
    <div class="chip">provider <b>${prov}</b></div>
    <div class=${"chip" + (cf.enabled === false ? " bad" : "")}>enabled <b>${cf.enabled === false ? "NO" : "yes"}</b></div>
    <div class="chip">requests <b>${c.requests ?? 0}</b></div>
    <div class="chip" title="Requests answered without a provider call (interval / unchanged)">skipped <b>${c.skipped ?? 0}</b></div>
    ${cf.enabled !== false && Object.keys(st.scheduler ?? {}).sort(facCmp).map((n) => html`<${WakeChip} key=${n} name=${n} s=${st.scheduler[n]} recvAt=${st.recvAt} toast=${toast} refresh=${refresh} paused=${!!st.paused} engine=${cf.engine} />`)}
    <${HqControls} st=${st} toast=${toast} refresh=${refresh} />
    ${(st.paused || (cf.engine && cf.engine !== "llm")) && html`<div class="chip hqnote">HQ engine: <b>${cf.engine && cf.engine !== "llm" ? cf.engine : "llm"}</b>${st.paused ? " (paused)" : ""}, no LLM calls</div>`}
    <div class=${"chip" + (c.errors ? " bad" : "")}>errors <b>${c.errors ?? 0}</b></div>
    <div class="chip">tokens in/out <b>${c.inputTokens ?? 0} / ${c.outputTokens ?? 0}</b></div>
    ${bud && bud.level !== "ok" && html`<div class=${"chip " + (bud.level === "warn" ? "warn" : "bad")} title="OpenRouter key budget (see Configuration)"><span class="dot"></span>${bud.level === "reached" ? "budget limit reached" : `budget ${bud.pct.toFixed(0)}% used`}</div>`}
    <div class=${"chip " + (stale ? "bad" : "ok")} title="Red when the game has not contacted the sidecar for 30 s"><span class="dot"></span>last request <b>${ageTxt}</b></div>
  </div>`;
}

// ---------- map (canvas renderer with pan/zoom, tooltips, layers) ----------
// act = all | active | dormant (unit filter); focus = { pos: [x, z], label, nonce } zooms the view onto a point and marks it; compact = small map (Missions).
function MapPanel({ st, faction, rows, sel, onSel, layers, world, tall, act = "all", focus = null, compact = false }) {
  const box = useRef(), cv = useRef();
  const view = useRef({ s: 0.1, ox: 0, oy: 0, fitted: false });
  const hits = useRef([]);
  const drag = useRef(null);
  const latest = useRef({});
  const [tip, setTip] = useState(null);
  latest.current = { st, faction, sel, layers, world, act, focus };
  const imgRef = useRef(null);
  const ext = world.extent ?? DEF_EXT;
  useEffect(() => { // map background: reload when the sidecar (re)resolves the image
    imgRef.current = null;
    if (world.map?.status !== "ready") return;
    const im = new Image();
    im.onload = () => { imgRef.current = im; };
    im.src = "/api/map.png?v=" + (world.map.ts ?? 0);
  }, [world.map?.status, world.map?.ts]);

  // Fit to the bounding box of all squads+contacts (padded, min span 400 m); whole world if there is no data.
  const fit = () => {
    const r = cv.current.getBoundingClientRect(), v = view.current;
    if (!r.width || !r.height) return;
    let x0 = Infinity, x1 = -Infinity, z0 = Infinity, z1 = -Infinity;
    for (const e of Object.values(latest.current.st.factions ?? {})) {
      const sit = e.situation ?? {};
      for (const o of [...(Array.isArray(sit.squads) ? sit.squads : []), ...(Array.isArray(sit.contacts) ? sit.contacts : [])]) {
        if (!o || !okPos(o.pos)) continue;
        x0 = Math.min(x0, o.pos[0]); x1 = Math.max(x1, o.pos[0]); z0 = Math.min(z0, o.pos[2]); z1 = Math.max(z1, o.pos[2]);
      }
    }
    const pic = publicPicture(latest.current.st);
    for (const o of [...pic.objs, ...pic.bases]) { x0 = Math.min(x0, o.pos[0]); x1 = Math.max(x1, o.pos[0]); z0 = Math.min(z0, o.pos[1]); z1 = Math.max(z1, o.pos[1]); }
    const hasData = x0 <= x1;
    const E = latest.current.world.extent ?? DEF_EXT, EW = E.x1 - E.x0, EH = E.z1 - E.z0;
    if (!hasData) { x0 = E.x0; x1 = E.x1; z0 = E.z0; z1 = E.z1; }
    const cx = (x0 + x1) / 2, cz = (z0 + z1) / 2;
    const wx = hasData ? Math.max(x1 - x0, 400) : EW, wz = hasData ? Math.max(z1 - z0, 400) : EH;
    v.s = Math.min(r.width / (wx * 1.3), r.height / (wz * 1.3));
    v.ox = r.width / 2 - cx * v.s; v.oy = r.height / 2 - (E.z1 - cz) * v.s;
    v.fitted = hasData; v.user = false;
  };

  const draw = useCallback((t) => {
    const c = cv.current;
    if (!c) return;
    const { st, faction, sel, layers, world, act, focus } = latest.current, v = view.current;
    const E = world.extent ?? DEF_EXT, EW = E.x1 - E.x0, EH = E.z1 - E.z0;
    const r = c.getBoundingClientRect(), dpr = window.devicePixelRatio || 1;
    if (!r.width) return;
    if (v.anim) { // eased pan/zoom towards a focus target
      const a = v.anim, p = Math.min(1, (performance.now() - a.t0) / 350), e = 1 - Math.pow(1 - p, 3);
      v.s = a.s0 + (a.s1 - a.s0) * e; v.ox = a.ox0 + (a.ox1 - a.ox0) * e; v.oy = a.oy0 + (a.oy1 - a.oy0) * e;
      if (p >= 1) v.anim = null;
    }
    if (c.width !== Math.round(r.width * dpr) || c.height !== Math.round(r.height * dpr)) { c.width = Math.round(r.width * dpr); c.height = Math.round(r.height * dpr); }
    if (!v.fitted) fit(); // refits every frame until the first squad/contact arrives, then once
    const ctx = c.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    const sx = (x) => v.ox + x * v.s, sy = (z) => v.oy + (E.z1 - z) * v.s;
    const bx = sx(E.x0), by = sy(E.z1), bw = EW * v.s, bh = EH * v.s; // world rectangle on screen
    ctx.fillStyle = "#0a0d12"; ctx.fillRect(0, 0, r.width, r.height);
    const g = ctx.createLinearGradient(bx, by, bx + bw, by + bh);
    g.addColorStop(0, "#131b27"); g.addColorStop(1, "#0f1620");
    ctx.fillStyle = g; ctx.fillRect(bx, by, bw, bh);
    const img = imgRef.current;
    if (layers.image && img) { // north up; layers.flip = image y axis inverted (calibration), world.map.flipY from the override json
      ctx.save();
      ctx.globalAlpha = num(layers.dim) ? layers.dim : 0.8;
      if (!!layers.flip !== !!world.map?.flipY) { ctx.translate(0, 2 * by + bh); ctx.scale(1, -1); }
      ctx.imageSmoothingQuality = "high"; ctx.drawImage(img, bx, by, bw, bh);
      ctx.restore();
    }
    if (layers.grid) {
      ctx.lineWidth = 1; ctx.font = "10px sans-serif";
      for (let k = Math.ceil(E.x0 / GRID) * GRID; k <= E.x1; k += GRID) {
        ctx.strokeStyle = k % 1024 === 0 ? "rgba(140,160,190,.22)" : "rgba(140,160,190,.10)";
        ctx.beginPath(); ctx.moveTo(sx(k), sy(E.z0)); ctx.lineTo(sx(k), sy(E.z1)); ctx.stroke();
        ctx.fillStyle = "#586377"; ctx.fillText(String(k), sx(k) + 3, sy(E.z0) - 4);
      }
      for (let k = Math.ceil(E.z0 / GRID) * GRID; k <= E.z1; k += GRID) {
        ctx.strokeStyle = k % 1024 === 0 ? "rgba(140,160,190,.22)" : "rgba(140,160,190,.10)";
        ctx.beginPath(); ctx.moveTo(sx(E.x0), sy(k)); ctx.lineTo(sx(E.x1), sy(k)); ctx.stroke();
        ctx.fillStyle = "#586377"; ctx.fillText(String(k), sx(E.x0) + 3, sy(k) - 3);
      }
    }
    ctx.strokeStyle = "rgba(140,160,190,.4)"; ctx.lineWidth = 1.5; ctx.strokeRect(bx, by, bw, bh);
    if (layers.roads) { // one stroked path per width bucket; roads outside the viewport are skipped
      ctx.lineCap = "round"; ctx.lineJoin = "round";
      for (const grp of world.roadGroups ?? []) {
        ctx.strokeStyle = img && layers.image ? "rgba(255,238,190,.55)" : "rgba(196,176,128,.5)";
        ctx.lineWidth = Math.min(6, Math.max(0.8, grp.w * v.s * 0.9));
        ctx.beginPath();
        for (const rd of grp.list) {
          if (sx(rd.x1) < 0 || sx(rd.x0) > r.width || sy(rd.z0) < 0 || sy(rd.z1) > r.height) continue;
          ctx.moveTo(sx(rd.p[0]), sy(rd.p[1]));
          for (let i = 2; i < rd.p.length; i += 2) ctx.lineTo(sx(rd.p[i]), sy(rd.p[i + 1]));
        }
        ctx.stroke();
      }
      ctx.lineCap = "butt"; ctx.lineJoin = "miter";
    }
    if (layers.buildings && world.buildings?.length && v.s >= 0.03) {
      // footprints: x = local width axis, y = local depth axis. Assumption: yaw is the Enforce heading (0 = +Z = north, 90 = +X = east),
      // so on this north-up canvas the rect is rotated clockwise by yaw (verify in game against a known building if it looks off).
      const bl = world.buildings, vx0 = (0 - v.ox) / v.s - 120, vx1 = (r.width - v.ox) / v.s + 120;
      const zTop = E.z1 - (0 - v.oy) / v.s + 120, zBot = E.z1 - (r.height - v.oy) / v.s - 120; // world z at screen top / bottom
      let lo = 0, hi = bl.length;
      while (lo < hi) { const m = (lo + hi) >> 1; if (bl[m].x < vx0) lo = m + 1; else hi = m; }
      const detail = v.s >= 0.6, onlyBig = v.s < 0.15;
      const fills = ["rgba(215,200,178,.50)", "rgba(226,206,170,.62)", "rgba(232,164,108,.70)"];
      const paths = [new Path2D(), new Path2D(), new Path2D()];
      for (let i = lo; i < bl.length; i++) {
        const b = bl[i];
        if (b.x > vx1) break;
        if (b.z > zTop || b.z < zBot || (onlyBig && b.cat < 1)) continue;
        const x = sx(b.x), y = sy(b.z);
        let hw = b.w * v.s / 2, hd = b.d * v.s / 2;
        const P = paths[b.cat] ?? paths[0];
        if (hw < 1.5 && hd < 1.5) { P.rect(x - 1, y - 1, 2, 2); continue; } // too small to show the shape: 2 px dot
        hw = Math.max(hw, 1); hd = Math.max(hd, 1);
        P.moveTo(x + (-hw) * b.c - (-hd) * b.s, y + (-hw) * b.s + (-hd) * b.c);
        P.lineTo(x + hw * b.c - (-hd) * b.s, y + hw * b.s + (-hd) * b.c);
        P.lineTo(x + hw * b.c - hd * b.s, y + hw * b.s + hd * b.c);
        P.lineTo(x + (-hw) * b.c - hd * b.s, y + (-hw) * b.s + hd * b.c);
        P.closePath();
      }
      for (let k = 0; k < 3; k++) { ctx.fillStyle = fills[k]; ctx.fill(paths[k]); }
      if (detail) { ctx.lineWidth = 0.75; ctx.strokeStyle = "rgba(24,18,12,.6)"; for (let k = 0; k < 3; k++) ctx.stroke(paths[k]); }
    }
    const used = []; // label boxes already taken; objects are drawn first so they always win over town names
    if (layers.objects) { // bases / airports / ports: symbol + bold label, never culled by the overlap check
      ctx.font = "bold 12px system-ui, sans-serif"; ctx.textBaseline = "middle"; ctx.lineJoin = "round";
      for (const ob of world.objects ?? []) {
        if (v.s < ob.minZoom) continue;
        const x = sx(ob.pos[0]), y = sy(ob.pos[1]);
        if (x < -60 || x > r.width + 60 || y < -20 || y > r.height + 20) continue;
        ctx.lineWidth = 1.5; ctx.strokeStyle = "rgba(8,11,16,.95)"; ctx.fillStyle = "#f2c94c";
        ctx.beginPath();
        if (ob.kind === "base") { ctx.moveTo(x, y - 7); ctx.lineTo(x + 7, y); ctx.lineTo(x, y + 7); ctx.lineTo(x - 7, y); ctx.closePath(); ctx.fill(); ctx.stroke(); }
        else if (ob.kind === "airport") {
          ctx.moveTo(x, y - 8); ctx.lineTo(x + 6, y + 3); ctx.lineTo(x - 6, y + 3); ctx.closePath(); ctx.fill(); ctx.stroke();
          ctx.fillRect(x - 8, y + 5, 16, 3); ctx.strokeRect(x - 8, y + 5, 16, 3);
        } else { ctx.arc(x, y, 6, 0, Math.PI * 2); ctx.stroke(); ctx.fillStyle = "#6ec1ff"; ctx.fill(); ctx.beginPath(); ctx.arc(x, y, 2, 0, Math.PI * 2); ctx.fillStyle = "#08111a"; ctx.fill(); }
        const w = ctx.measureText(ob.name).width;
        used.push([x - 8, y - 9, x + w + 14, y + 9]);
        ctx.lineWidth = 3.5; ctx.strokeStyle = "rgba(8,11,16,.9)"; ctx.strokeText(ob.name, x + 11, y);
        ctx.fillStyle = "#fff3c4"; ctx.fillText(ob.name, x + 11, y);
      }
      ctx.lineJoin = "miter";
    }
    if (layers.places) { // named places, important first; a label is skipped when it would overlap an earlier one
      ctx.font = "11px system-ui, sans-serif"; ctx.textBaseline = "middle"; ctx.lineJoin = "round";
      for (const pl of world.places ?? []) {
        if (v.s < pl.minZoom) continue;
        const x = sx(pl.pos[0]), y = sy(pl.pos[1]);
        if (x < -40 || x > r.width + 40 || y < -10 || y > r.height + 10) continue;
        const w = ctx.measureText(pl.name).width;
        if (used.some((u) => x < u[2] && x + w + 8 > u[0] && y - 7 < u[3] && y + 7 > u[1])) continue;
        used.push([x, y - 7, x + w + 8, y + 7]);
        ctx.fillStyle = "rgba(230,236,245,.85)"; ctx.beginPath(); ctx.arc(x, y, 2, 0, Math.PI * 2); ctx.fill();
        ctx.lineWidth = 3; ctx.strokeStyle = "rgba(8,11,16,.85)"; ctx.strokeText(pl.name, x + 6, y);
        ctx.fillText(pl.name, x + 6, y);
      }
      ctx.lineJoin = "miter";
    }
    const h = [];
    ctx.font = "11px system-ui, sans-serif"; ctx.textBaseline = "middle";
    const pic = publicPicture(st), objById = new Map(pic.objs.map((o) => [o.id, o]));
    if (layers.objectives) {
      const hatch = hatchPattern(ctx);
      for (const o of pic.objs) {
        const x = sx(o.pos[0]), y = sy(o.pos[1]);
        if (x < -200 || x > r.width + 200 || y < -200 || y > r.height + 200) continue;
        const col = o.owner && o.owner !== "none" ? colorOf(o.owner) : NEUTRAL, rad = Math.max(7, (num(o.radius) ? o.radius : 50) * v.s);
        const blink = o.contested ? 0.5 + 0.5 * Math.sin(t / 250) : 1;
        ctx.fillStyle = col; ctx.globalAlpha = 0.16; ctx.beginPath(); ctx.arc(x, y, rad, 0, Math.PI * 2); ctx.fill();
        if (o.contested && hatch) { ctx.save(); ctx.beginPath(); ctx.arc(x, y, rad, 0, Math.PI * 2); ctx.clip(); ctx.globalAlpha = 0.55; ctx.fillStyle = hatch; ctx.fillRect(x - rad, y - rad, rad * 2, rad * 2); ctx.restore(); }
        ctx.globalAlpha = o.contested ? 0.35 + 0.65 * blink : 0.95; ctx.strokeStyle = col; ctx.lineWidth = o.contested ? 3 : 2;
        ctx.beginPath(); ctx.arc(x, y, rad, 0, Math.PI * 2); ctx.stroke();
        if (num(o.progress) && o.progress !== 0) { // progress ring: |progress| % of the circle, drawn just inside the radius
          ctx.globalAlpha = 0.95; ctx.lineWidth = 4; ctx.strokeStyle = col;
          ctx.beginPath(); ctx.arc(x, y, Math.max(3, rad - 5), -Math.PI / 2, -Math.PI / 2 + Math.PI * 2 * Math.min(100, Math.abs(o.progress)) / 100); ctx.stroke();
        }
        ctx.globalAlpha = 1;
        ctx.fillStyle = col; ctx.beginPath(); ctx.arc(x, y, 3, 0, Math.PI * 2); ctx.fill();
        if (layers.labels) {
          ctx.font = "bold 12px system-ui, sans-serif"; ctx.lineJoin = "round"; ctx.lineWidth = 3.5; ctx.strokeStyle = "rgba(8,11,16,.9)";
          const txt = `${o.id}${num(o.value) && o.value > 1 ? " (v" + o.value + ")" : ""}${num(o.progress) && o.progress !== 0 ? " " + Math.round(o.progress) + "%" : ""}`;
          ctx.strokeText(txt, x + 6, y - rad - 8); ctx.fillStyle = "#fff"; ctx.fillText(txt, x + 6, y - rad - 8);
          if (v.s > 0.08 && o.name) { ctx.font = "10px system-ui, sans-serif"; ctx.strokeText(String(o.name), x + 6, y - rad + 5); ctx.fillStyle = "#dde3ec"; ctx.fillText(String(o.name), x + 6, y - rad + 5); }
          ctx.font = "11px system-ui, sans-serif"; ctx.lineJoin = "miter";
        }
        h.push({ x, y, r: Math.max(12, rad), title: `Objective ${o.id}${o.name ? " " + o.name : ""}`, lines: [`owner: ${o.owner ?? "?"}${o.contested ? " (CONTESTED)" : ""}`, `value ${dash(o.value)}, radius ${dash(o.radius)} m`, `capture progress ${dash(o.progress)}`, `at ${Math.round(o.pos[0])}, ${Math.round(o.pos[1])}`] });
      }
      for (const b of pic.bases) {
        const x = sx(b.pos[0]), y = sy(b.pos[1]), col = colorOf(b.faction);
        ctx.fillStyle = col; ctx.strokeStyle = "#0a0d12"; ctx.lineWidth = 2;
        ctx.beginPath(); ctx.rect(x - 8, y - 8, 16, 16); ctx.fill(); ctx.stroke();
        ctx.fillStyle = "#0a0d12"; ctx.font = "bold 10px system-ui, sans-serif"; ctx.textAlign = "center"; ctx.fillText("HQ", x, y + 1); ctx.textAlign = "start";
        if (layers.labels) { ctx.font = "bold 11px system-ui, sans-serif"; ctx.lineWidth = 3.5; ctx.lineJoin = "round"; ctx.strokeStyle = "rgba(8,11,16,.9)"; ctx.strokeText(b.faction + " base", x + 12, y); ctx.fillStyle = col; ctx.fillText(b.faction + " base", x + 12, y); ctx.lineJoin = "miter"; ctx.font = "11px system-ui, sans-serif"; }
        h.push({ x, y, r: 12, title: `${b.faction} main base`, lines: [`at ${Math.round(b.pos[0])}, ${Math.round(b.pos[1])}`] });
      }
    }
    const shown =Object.keys(st.factions ?? {}).filter((f) => faction === "all" || f === faction);
    for (const f of shown) {
      const e = st.factions[f], sit = e.situation ?? {};
      const squads = (Array.isArray(sit.squads) ? sit.squads : []).filter((s) => s && okPos(s.pos) && actOK(act, s.dormant === true));
      const byId = new Map(squads.map((s) => [s.id, s]));
      const byOrder = new Map((Array.isArray(e.orders) ? e.orders : []).map((o) => [o.squad, o]));
      if (layers.contacts) for (const ct of Array.isArray(sit.contacts) ? sit.contacts : []) {
        if (!okPos(ct.pos)) continue;
        const x = sx(ct.pos[0]), y = sy(ct.pos[2]), age = num(ct.ageSec) ? ct.ageSec : 0;
        ctx.globalAlpha = Math.max(0.25, 1 - age / 300);
        ctx.strokeStyle = enemyColor(f); ctx.lineWidth = 2;
        ctx.beginPath(); ctx.moveTo(x, y - 8); ctx.lineTo(x + 8, y); ctx.lineTo(x, y + 8); ctx.lineTo(x - 8, y); ctx.closePath(); ctx.stroke();
        if (layers.labels) { ctx.font = "10px system-ui, sans-serif"; ctx.fillStyle = enemyColor(f); ctx.fillText(String(ct.desc ?? "?").slice(0, 14), x + 11, y + 11); ctx.font = "11px system-ui, sans-serif"; }
        ctx.globalAlpha = 1;
        h.push({ x, y, r: 10, title: "Contact (seen by " + f + ")", lines: [String(ct.desc ?? "?"), `age ${age} s`, `at ${gridTxt(ct.pos)}`] });
      }
      // arrows: the sidecar's LLM orders, else the squad's current objective as reported by the game (rule commander, human leader)
      const arrows = [];
      for (const s of squads) {
        const lo = byOrder.get(s.id), ob = s.objective;
        if (lo) arrows.push(lo);
        else if (ob && ob.command && (Array.isArray(ob.pos) || ob.target)) arrows.push({ squad: s.id, command: ob.command, pos: ob.pos, target: ob.target, reason: ob.reason });
      }
      if (layers.orders) for (const o0 of arrows) {
        const s = byId.get(o0.squad);
        const tg = o0.target ? objById.get(o0.target) : null; // order on an objective id: aim at the objective centre
        const o = tg ? { ...o0, pos: tg.pos } : o0;
        if (!s || !Array.isArray(o.pos) || !num(o.pos[0]) || !num(o.pos[1])) continue;
        const x0 = sx(s.pos[0]), y0 = sy(s.pos[2]), x1 = sx(o.pos[0]), y1 = sy(o.pos[1]);
        const col = colorOf(f), a = Math.atan2(y1 - y0, x1 - x0);
        ctx.strokeStyle = col; ctx.fillStyle = col; ctx.lineWidth = 2; ctx.globalAlpha = .85;
        ctx.setLineDash(o.command === "MOVE" || o.command === "FALL_BACK" ? [6, 5] : o.command === "RECON" ? [2, 4] : []);
        ctx.beginPath(); ctx.moveTo(x0, y0); ctx.lineTo(x1, y1); ctx.stroke(); ctx.setLineDash([]);
        ctx.beginPath(); ctx.moveTo(x1, y1); ctx.lineTo(x1 - 10 * Math.cos(a - .4), y1 - 10 * Math.sin(a - .4)); ctx.lineTo(x1 - 10 * Math.cos(a + .4), y1 - 10 * Math.sin(a + .4)); ctx.closePath(); ctx.fill();
        ctx.globalAlpha = 1;
        if (layers.labels) { ctx.font = "9px system-ui, sans-serif"; ctx.fillStyle = "rgba(221,227,236,.75)"; ctx.fillText(o.command + (o.target ? " " + o.target : ""), x1 + 8, y1 + 12); ctx.font = "11px system-ui, sans-serif"; }
        h.push({ x: x1, y: y1, r: 8, title: `${o.command} order${o.target ? " -> " + o.target : ""}`, lines: [String(o.squad), String(o.reason ?? "")] });
      }
      if (layers.squads) for (const s of squads) {
        const x = sx(s.pos[0]), y = sy(s.pos[2]), key = f + "/" + s.id, col = colorOf(f), dm = s.dormant === true;
        if (s.humanLed) {
          const ph = ((t / 1200) % 1);
          ctx.strokeStyle = "#fff"; ctx.lineWidth = 2; ctx.globalAlpha = 1 - ph;
          ctx.beginPath(); ctx.arc(x, y, 9 + ph * 14, 0, Math.PI * 2); ctx.stroke(); ctx.globalAlpha = 1;
        }
        if (sel === key) { ctx.strokeStyle = "#ffe36e"; ctx.lineWidth = 3; ctx.beginPath(); ctx.arc(x, y, 14, 0, Math.PI * 2); ctx.stroke(); }
        if (num(s.heading) && num(s.speed) && s.speed > 0.3) { // movement vector: compass heading (0 = north), length grows with speed
          const hr = s.heading * Math.PI / 180, len = 14 + Math.min(s.speed, 6) * 5, ex = x + Math.sin(hr) * len, ey = y - Math.cos(hr) * len;
          ctx.strokeStyle = "#fff"; ctx.fillStyle = "#fff"; ctx.lineWidth = 2; ctx.globalAlpha = .9;
          ctx.beginPath(); ctx.moveTo(x, y); ctx.lineTo(ex, ey); ctx.stroke();
          ctx.beginPath(); ctx.moveTo(ex, ey); ctx.lineTo(ex - 7 * Math.sin(hr - .45), ey + 7 * Math.cos(hr - .45)); ctx.lineTo(ex - 7 * Math.sin(hr + .45), ey + 7 * Math.cos(hr + .45)); ctx.closePath(); ctx.fill();
          ctx.globalAlpha = 1;
        }
        ctx.fillStyle = dm ? tint(col, DORM_A) : col; ctx.shadowColor = col; ctx.shadowBlur = dm ? 0 : 10;
        kindShape(ctx, s.kind, x, y); ctx.fill(); ctx.shadowBlur = 0;
        if (dm) { ctx.strokeStyle = tint(col, 0.85); ctx.lineWidth = 1.5; ctx.stroke(); } // dormant: pale fill, thin outline
        if (s.humanLed) { ctx.strokeStyle = "#fff"; ctx.lineWidth = 2; ctx.stroke(); }
        const veh = s.vehicle && typeof s.vehicle === "object" ? s.vehicle : null;
        if (veh && veh.mounted === false) { ctx.strokeStyle = "#0a0d12"; ctx.lineWidth = 2; ctx.beginPath(); ctx.moveTo(x - 4, y); ctx.lineTo(x + 4, y); ctx.stroke(); } // dismounted: bar across the symbol
        if (layers.labels) {
          ctx.fillStyle = dm ? "rgba(221,227,236,.6)" : "#dde3ec";
          ctx.font = "600 11px system-ui, sans-serif"; ctx.fillText(shortId(s.id) + (s.humanLed ? " ★" : ""), x + 9, y - 11);
          if (s.kind || veh) { ctx.font = "9px system-ui, sans-serif"; ctx.fillStyle = dm ? "rgba(221,227,236,.45)" : "rgba(221,227,236,.7)"; ctx.fillText([kindLabel(s.kind), veh?.type, veh ? (veh.mounted ? "mounted" : "dismounted") : null].filter(Boolean).join(" "), x + 9, y + 13); }
          ctx.font = "11px system-ui, sans-serif";
        }
        const oc = s.objective?.command ?? (byOrder.get(s.id)?.command ?? "-"), ot = s.objective?.target ?? byOrder.get(s.id)?.target;
        h.push({ x, y, r: 11, key, title: String(s.id), lines: [`${f}, ${s.agents}/${s.startAgents} agents, ${s.mode ?? s.state ?? "-"}`, `activity: ${typeof s.dormant !== "boolean" ? "unknown (game does not report it)" : dm ? "dormant (engine max AI LOD, barely simulated)" : "active (kept awake)"}`, ...(s.kind ? [`kind ${s.kind}${s.type ? ": " + s.type : ""}`] : []),
          ...(veh ? [`vehicle ${dash(veh.type)}, ${veh.mounted ? "mounted" : "dismounted"}${veh.state ? ", " + veh.state : ""}${num(veh.health) ? ", hp " + Math.round(veh.health * 100) + "%" : ""}`] : []),
          `objective: ${oc}${ot ? " " + ot : ""}`, s.humanLed ? "human-led" + (s.mission?.state && s.mission.state !== "none" ? `, mission ${s.mission.state} ${s.mission.kind ?? ""} ${s.mission.target ?? ""}` : "") : "AI-led", `at ${gridTxt(s.pos)}`] });
      }
    }
    if (focus && Array.isArray(focus.pos)) { // focus marker (Missions): pulsing dashed ring + label
      const x = sx(focus.pos[0]), y = sy(focus.pos[1]), pr = 20 + 3 * Math.sin(t / 220);
      ctx.strokeStyle = "#ffe36e"; ctx.lineWidth = 2.5; ctx.setLineDash([5, 4]); ctx.beginPath(); ctx.arc(x, y, pr, 0, Math.PI * 2); ctx.stroke(); ctx.setLineDash([]);
      if (focus.label) { ctx.font = "bold 12px system-ui, sans-serif"; ctx.lineJoin = "round"; ctx.lineWidth = 3.5; ctx.strokeStyle = "rgba(8,11,16,.9)"; ctx.strokeText(focus.label, x + pr + 6, y); ctx.fillStyle = "#ffe36e"; ctx.fillText(focus.label, x + pr + 6, y); ctx.lineJoin = "miter"; }
    }
    hits.current = h;
  }, []);

  useEffect(() => { // focus request: ease the view onto the target
    const c = cv.current;
    if (!focus || !Array.isArray(focus.pos) || !c) return;
    const v = view.current, r = c.getBoundingClientRect(), E = latest.current.world.extent ?? DEF_EXT;
    if (!r.width) return;
    const s1 = focus.zoom ?? Math.max(0.3, Math.min(1.2, r.width / 1500));
    v.anim = { t0: performance.now(), s0: v.s, ox0: v.ox, oy0: v.oy, s1, ox1: r.width / 2 - focus.pos[0] * s1, oy1: r.height / 2 - (E.z1 - focus.pos[1]) * s1 };
    v.fitted = true; v.user = true;
  }, [focus?.nonce]);

  // animation loop (pulsing rings need continuous redraw)
  useEffect(() => {
    let raf;
    const loop = (t) => { draw(t); raf = requestAnimationFrame(loop); };
    raf = requestAnimationFrame(loop);
    return () => cancelAnimationFrame(raf);
  }, [draw]);

  const pick = (mx, my, onlySquad) => {
    let best = null, bd = Infinity;
    for (const h of hits.current) {
      if (onlySquad && !h.key) continue;
      const d = Math.hypot(h.x - mx, h.y - my);
      if (d < h.r && d < bd) { bd = d; best = h; }
    }
    return best;
  };
  const local = (e) => { const r = cv.current.getBoundingClientRect(); return [e.clientX - r.left, e.clientY - r.top]; };

  // keep the view sensible when the panel is resized (unless the user has already panned/zoomed)
  useEffect(() => {
    const ro = new ResizeObserver(() => { if (!view.current.user) fit(); });
    ro.observe(box.current);
    return () => ro.disconnect();
  }, []);

  // wheel needs a non-passive listener
  useEffect(() => {
    const c = cv.current;
    const onWheel = (e) => {
      e.preventDefault();
      const v = view.current, [mx, my] = local(e); v.anim = null;
      const ns = Math.min(5, Math.max(0.02, v.s * (e.deltaY < 0 ? 1.2 : 1 / 1.2))), q = ns / v.s;
      v.ox = mx - (mx - v.ox) * q; v.oy = my - (my - v.oy) * q; v.s = ns; v.user = true;
    };
    c.addEventListener("wheel", onWheel, { passive: false });
    return () => c.removeEventListener("wheel", onWheel);
  }, []);

  const onDown = (e) => { view.current.anim = null; drag.current = { x: e.clientX, y: e.clientY, moved: 0 }; cv.current.classList.add("grabbing"); };
  const onMove = (e) => {
    const d = drag.current, v = view.current;
    if (d) {
      d.moved += Math.abs(e.clientX - d.x) + Math.abs(e.clientY - d.y);
      v.user = true; v.ox += e.clientX - d.x; v.oy += e.clientY - d.y; d.x = e.clientX; d.y = e.clientY; setTip(null); return;
    }
    const [mx, my] = local(e), h = pick(mx, my, false);
    setTip(h ? { x: mx, y: my, h } : null);
  };
  const onUp = (e) => {
    const d = drag.current; drag.current = null; cv.current.classList.remove("grabbing");
    if (d && d.moved < 4) { const [mx, my] = local(e), h = pick(mx, my, true); onSel(h ? h.key : null); }
  };

  return html`<section class=${"panel" + (tall ? " tall" : "") + (compact ? " compact" : "")}>
    <h2>${compact ? "Mission map" : "Map"} <span class="sub">${Math.round(ext.x1 - ext.x0)} x ${Math.round(ext.z1 - ext.z0)} m, x east / z north${world.map?.status === "resolving" ? ", loading map image..." : ""}</span></h2>
    <div class="mapbox" ref=${box}>
      <canvas ref=${cv} onMouseDown=${onDown} onMouseMove=${onMove} onMouseUp=${onUp}
        onMouseLeave=${() => { setTip(null); drag.current = null; cv.current.classList.remove("grabbing"); }}
        onDblClick=${() => fit()}></canvas>
      <button class="mapbtn" onClick=${() => fit()} title="Reset view"><${Icon} n="reset" /> Fit</button>
      <div class="maphint">wheel zoom, drag pan, click a squad to select, hover for details</div>
      ${tip && html`<div class="tip" style=${{ left: Math.min(tip.x + 14, (box.current?.clientWidth ?? 600) - 200) + "px", top: tip.y + 14 + "px" }}>
        <b>${tip.h.title}</b>${tip.h.lines.map((l) => html`<div class="muted">${l}</div>`)}</div>`}
    </div>
  </section>`;
}

// ---------- shared faction-vs-faction components ----------
// Compare: attributes as rows, factions as side-by-side columns (same table everywhere a faction comparison is shown).
// rows: { section } | { label, tip, merge, max, cell: (faction) => { txt, v, max, stack, dim } }
//  - v draws a thin bar under the value on a scale shared by all factions in the row (cell.max overrides, e.g. a cap)
//  - stack = [[count, alpha], ...] draws a segmented bar in the faction colour (active strong / dormant pale)
//  - merge: identical string values collapse into one spanning cell (rules that are the same for both sides)
function CmpCell({ c, mx, f }) {
  const col = colorOf(f), tot = c.stack ? c.stack.reduce((n, s) => n + s[0], 0) : 0;
  return html`<div class="cc"><span class=${"cv" + (c.dim ? " dim" : "")}>${c.txt ?? "-"}</span>
    ${c.stack && tot > 0 ? html`<span class="mbar">${c.stack.filter((s) => s[0] > 0).map((s, i) => html`<i key=${i} style=${{ width: (100 * s[0] / tot) + "%", background: tint(col, s[1]) }}></i>`)}</span>`
      : num(c.v) && mx > 0 ? html`<span class="mbar"><i style=${{ width: Math.min(100, 100 * c.v / mx) + "%", background: col }}></i></span>` : ""}</div>`;
}
function Compare({ names, rows }) {
  const n = names.length;
  return html`<table class="cmp"><thead><tr><th></th>${names.map((f) => html`<th key=${f} class="fh" style=${{ borderTopColor: colorOf(f) }}><b class=${facClass(f)}>${f}</b></th>`)}</tr></thead><tbody>
    ${rows.map((r, i) => {
      if (r.section) return html`<tr class="sec" key=${i}><th colspan=${n + 1}>${r.section}</th></tr>`;
      const cells = names.map(r.cell), mx = r.max ?? Math.max(0, ...cells.map((c) => (num(c.v) ? c.v : 0)));
      const merged = r.merge && n > 1 && cells.every((c) => typeof c.txt === "string" && c.txt === cells[0].txt);
      return html`<tr key=${i}><td class="rl" title=${r.tip ?? ""}>${r.label}</td>${merged ? html`<td colspan=${n} class="mg">${cells[0].txt}</td>`
        : cells.map((c, j) => html`<td key=${j}><${CmpCell} c=${c} mx=${c.max ?? mx} f=${names[j]} /></td>`)}</tr>`;
    })}</tbody></table>`;
}
// unit-activity filter (all / active / dormant), shared by the Map and Forces views; counts come from the unfiltered rows
function ActFilter({ act, setAct, rows }) {
  const dm = rows.filter((r) => r.dormant === true).length, ac = rows.filter((r) => r.dormant !== true).length, known = rows.some((r) => r.dormant !== null);
  const opt = [["all", "all", rows.length], ["active", "active", ac], ["dormant", "dormant", dm]];
  return html`<div class="seg actf" title=${known ? "Dormant = every agent of the unit sits in the engine's max AI LOD (barely simulated); active = kept awake by activity management" : "This game build does not report unit activity (no dormant field)"}>
    ${opt.map(([k, l, n]) => html`<button key=${k} class=${act === k ? "on" : ""} disabled=${!known && k !== "all"} onClick=${() => setAct(k)}>${l} <span class="muted">${n}</span></button>`)}</div>`;
}
// key: strong swatch = active, pale swatch = dormant, per faction
function ActLegend({ names, rows }) {
  if (!rows.some((r) => r.dormant !== null)) return null;
  return html`<div class="actleg">${names.map((f) => html`<span key=${f}><i class="sw" style=${{ background: colorOf(f) }}></i><i class="sw" style=${{ background: tint(colorOf(f), DORM_A), boxShadow: `inset 0 0 0 1px ${tint(colorOf(f), 0.85)}` }}></i> <b class=${facClass(f)}>${f}</b></span>`)}
    <span class="muted">strong = active, pale = dormant</span></div>`;
}

// ---------- squads table ----------
const COLS = [["id", "Squad"], ["faction", "Faction"], ["kind", "Kind"], ["strength", "Strength"], ["objective", "Objective"], ["state", "State"], ["position", "Position x, z"]];
const SORTKEY = { id: (r) => r.id, faction: (r) => r.faction, kind: (r) => r.kind, strength: (r) => r.pct ?? -1,
  objective: (r) => r.obj?.command ?? "", state: (r) => r.state, position: (r) => (okPos(r.pos) ? r.pos[0] : -1) };
const stateBadge = (s) => ({ dead: "bad", engaged: "bad", moving: "info", defending: "ok" }[s] ?? "");
const DormBadge = ({ r }) => (r.dormant === true ? html`<span class="badge dorm" title="Dormant: engine max AI LOD">dormant</span>` : "");

// Compact table; the secondary fields (type, vehicle, wounded, movement, leader, order age) open as a detail row on the selected squad.
function SquadsTable({ rows, sel, onSel }) {
  const [sort, setSort] = useState({ key: "faction", dir: 1 });
  const sorted = useMemo(() => {
    const f = SORTKEY[sort.key] ?? SORTKEY.id;
    return [...rows].sort((a, b) => (f(a) > f(b) ? 1 : f(a) < f(b) ? -1 : a.id > b.id ? 1 : -1) * sort.dir);
  }, [rows, sort]);
  useEffect(() => { if (sel) document.querySelector(`tr[data-key="${CSS.escape(sel)}"]`)?.scrollIntoView({ block: "nearest" }); }, [sel]);
  return html`<section class="panel squads">
    <h2>Squads <span class="sub">${rows.length} shown, click header to sort, click a row for details</span></h2>
    <div class="scroll"><table>
      <thead><tr>${COLS.map(([k, t]) => html`<th class="sortable" onClick=${() => setSort((s) => ({ key: k, dir: s.key === k ? -s.dir : 1 }))}>${t}${sort.key === k ? (sort.dir > 0 ? " ▲" : " ▼") : ""}</th>`)}</tr></thead>
      <tbody>${sorted.map((r) => {
        const col = r.pct === null ? "#5d697c" : r.pct >= 66 ? "var(--ok)" : r.pct >= 33 ? "var(--warn)" : "var(--bad)", o = r.obj, on = sel === r.key;
        const main = html`<tr key=${r.key} data-key=${r.key} class=${"sqrow" + (on ? " sel" : "") + (r.dormant === true ? " dormrow" : "")} onClick=${() => onSel(on ? null : r.key)}>
          <td style=${{ borderLeft: "3px solid " + colorOf(r.faction) }}><b>${r.id}</b>${r.human ? html` <span class="badge human">human</span>` : ""}</td><td class=${facClass(r.faction)}>${r.faction}</td>
          <td>${r.kind ? html`<span class=${"badge kind k-" + r.kind}>${kindLabel(r.kind)}</span>` : "-"}</td>
          <td>${r.pct === null ? "-" : html`<span class="bar"><i style=${{ width: Math.min(100, r.pct) + "%", background: col }}></i></span><span class="mono">${r.size}</span> <span class="muted">${r.pct}%</span>`}</td>
          <td class="wrap">${o ? html`<span class=${"cmd " + o.command}>${o.command}</span> ${o.target ? html`<b>${o.target}</b>` : ""}${!o.target && Array.isArray(o.pos) && num(o.pos[0]) ? html`<span class="mono">${Math.round(o.pos[0])}, ${Math.round(o.pos[1])}</span>` : ""}${o.status ? html` <span class="muted">${o.status}</span>` : ""}` : "-"}</td>
          <td><span class=${"badge " + stateBadge(r.state)}>${r.state}</span> <${DormBadge} r=${r} /></td><td class="mono">${r.posTxt}</td></tr>`;
        if (!on) return main;
        return [main, html`<tr key=${r.key + "#d"} class="detail"><td colspan=${COLS.length}><dl class="kv">
          <div><dt>Type</dt><dd>${r.type}</dd></div>
          <div><dt>Vehicle</dt><dd>${r.vehicle ? html`${dash(r.vehicle.type)} <span class=${"badge " + (r.vehicle.mounted ? "info" : "warn")}>${r.vehicle.mounted ? "mounted" : "dismounted"}</span>${r.vehicle.state ? html` <span class="muted">${r.vehicle.state}</span>` : ""}${num(r.vehicle.health) ? html` <span class="muted">hp ${Math.round(r.vehicle.health * 100)}%</span>` : ""}` : "-"}</dd></div>
          <div><dt>Wounded</dt><dd>${r.wounded === null ? "-" : r.wounded}${r.health !== null ? html` <span class="muted">avg hp ${Math.round(r.health * 100)}%</span>` : ""}</dd></div>
          <div><dt>Movement</dt><dd class="mono">${r.move}</dd></div>
          <div><dt>Leader</dt><dd>${r.human ? html`human${r.mission ? html` <span class="badge warn">${r.mission.state} ${r.mission.kind ?? ""} ${r.mission.target ?? ""}</span>` : ""}` : "AI"}</dd></div>
          <div><dt>Activity</dt><dd>${r.dormant === true ? "dormant (engine max AI LOD)" : r.dormant === false ? "active (kept awake)" : "-"}</dd></div>
          <div><dt>Last order</dt><dd>${r.orderAge === null ? "-" : r.orderAge + " s ago"}</dd></div>
          <div class="wide"><dt>Reason</dt><dd class="muted">${dash(r.reason)}</dd></div></dl></td></tr>`];
      })}</tbody></table>
      ${!rows.length && html`<div class="empty">No squads to show.</div>`}</div>
  </section>`;
}

// ---------- game mode + live status (Overview): attributes as rows, factions as columns ----------
function GameModePanel({ st, names }) {
  const sit = (f) => st.factions[f]?.situation ?? {}, war = (f) => sit(f).war ?? null;
  if (!names.some((f) => war(f))) return null;
  const mode = names.map((f) => war(f)?.mode).find((m) => m && (m.name || m.id)), phase = names.map((f) => war(f)?.phase).find((p) => p);
  const keys = [...new Set(names.flatMap((f) => Object.keys(war(f)?.mode?.params ?? {})))];
  const cap = (v, c) => (num(c) && c > 0 ? { txt: html`${dash(v)}<span class="muted"> / ${c}</span>`, v: num(v) ? v : 0, max: c } : { txt: dash(v) });
  const rows = [
    { section: "Rules" },
    ...keys.map((k) => ({ label: humanKey(k), tip: k, merge: true, cell: (f) => ({ txt: fmtVal(war(f)?.mode?.params?.[k]) }) })),
    { label: "Time left", merge: true, cell: (f) => ({ txt: num(sit(f).timeLeftSec) && sit(f).timeLeftSec >= 0 ? fmtTime(sit(f).timeLeftSec) : "no limit" }) },
    { section: "Live" },
    { label: "Tickets", tip: "0 = defeat; -1 = not used by this mode", cell: (f) => { const t = war(f)?.tickets; return num(t) && t >= 0 ? { txt: String(t), v: t } : { txt: "n/a", dim: true }; } },
    { label: "Supplies", tip: "Purchasing power, income per minute", cell: (f) => { const w = war(f); return w ? { txt: html`${dash(w.supplies)}${num(w.income) ? html`<span class="muted"> +${w.income}/min</span>` : ""}`, v: num(w.supplies) ? w.supplies : 0 } : { txt: "-" }; } },
    { label: "AI soldiers", tip: "Live AI / cap", cell: (f) => cap(war(f)?.liveAI, war(f)?.aiCap) },
    { label: "Vehicles", tip: "Live vehicles / cap", cell: (f) => cap(war(f)?.vehicles, war(f)?.vehicleCap) },
  ];
  return html`<section class="panel"><h2>Game mode <span class="sub">${mode ? html`<b>${mode.name || mode.id}</b> ` : ""}${phase ? html`<span class=${"badge " + (phase === "running" ? "ok" : phase === "setup" ? "warn" : "info")}>${phase}</span>` : ""}</span></h2>
    <div class="scroll"><${Compare} names=${names} rows=${rows} /></div></section>`;
}

// ---------- per-faction SITREP (generated elsewhere: st.factions[f].sitrep = { text, sections:[{title, body}], generatedAt, source, warTimeSec }) ----------
function SitrepCard({ f, e, now }) {
  const s = e?.sitrep && typeof e.sitrep === "object" ? e.sitrep : null, secs = Array.isArray(s?.sections) ? s.sections.filter((x) => x && (x.title || x.body)) : [];
  const has = s && (s.text || secs.length), age = num(s?.generatedAt) ? Math.max(0, Math.round(((now ?? Date.now()) - s.generatedAt) / 1000)) : null;
  return html`<section class="panel sitrep" style=${accent(f)}><h2><span class=${facClass(f)}>${f}</span> SITREP
    ${has && html`<span class="sub">${s.source ? html`<span class=${"badge " + (s.source === "llm" ? "info" : "")} title=${s.source === "llm" ? "written by the LLM" : "generated by rules"}>${s.source}</span> ` : ""}${num(s.warTimeSec) ? "war time " + fmtTime(s.warTimeSec) + ", " : ""}${age === null ? "" : age < 90 ? age + " s ago" : Math.round(age / 60) + " min ago"}${age !== null && age > 600 ? html` <span class="badge warn">stale</span>` : ""}</span>`}</h2>
    ${has ? html`<div class="sitbody">${s.text && html`<p class="sittext">${s.text}</p>`}
      ${secs.length > 0 && html`<dl class="sitsecs">${secs.map((x, i) => html`<div key=${i}><dt>${dash(x.title)}</dt><dd>${x.body}</dd></div>`)}</dl>`}</div>`
      : html`<div class="empty sitempty">No SITREP yet. The HQ report appears here when the SITREP generator has produced one.</div>`}</section>`;
}

// ---------- force comparison (Overview and Forces): same table, Forces adds rows ----------
function forceStats(st, rows, f) {
  const mine = rows.filter((r) => r.faction === f), sit = st.factions[f]?.situation ?? {}, kinds = new Map();
  for (const r of mine) kinds.set(r.kind || "?", (kinds.get(r.kind || "?") ?? 0) + 1);
  const alive = mine.reduce((n, r) => n + (num(r.agents) ? r.agents : 0), 0);
  const start = mine.reduce((n, r) => n + (num(r.agents) ? (num(r.pct) && r.pct > 0 ? Math.round(r.agents * 100 / r.pct) : r.agents) : 0), 0);
  const dorm = mine.filter((r) => r.dormant === true).length;
  return { mine, kinds, alive, start: Math.max(start, alive), dorm, act: mine.length - dorm, known: mine.some((r) => r.dormant !== null), contacts: Array.isArray(sit.contacts) ? sit.contacts.length : 0,
    vehicles: mine.filter((r) => r.vehicle).length, human: mine.filter((r) => r.human).length, wounded: mine.reduce((n, r) => n + (num(r.wounded) ? r.wounded : 0), 0),
    engaged: mine.filter((r) => r.state === "engaged").length, moving: mine.filter((r) => r.state === "moving").length };
}
function ForcesCompare({ st, rows, names, full, title = "Forces" }) {
  if (!names.length) return null;
  const S = new Map(names.map((f) => [f, forceStats(st, rows, f)]));
  const g = (f) => S.get(f);
  const out = [
    { label: "Squads", tip: "Commandable units. Bar: strong = active, pale = dormant", cell: (f) => ({ txt: String(g(f).mine.length), stack: g(f).known ? [[g(f).act, 1], [g(f).dorm, DORM_A]] : null, v: g(f).mine.length }) },
    ...(rows.some((r) => r.dormant !== null) ? [{ label: "Active / dormant", tip: "Dormant = all agents in the engine's max AI LOD", cell: (f) => (g(f).known ? { txt: html`${g(f).act} <span class="muted">/</span> ${g(f).dorm}` } : { txt: "-", dim: true }) }] : []),
    { label: "Soldiers", tip: "Alive / at start. Bar: share still alive", cell: (f) => ({ txt: html`${g(f).alive}${g(f).start > g(f).alive ? html`<span class="muted"> / ${g(f).start}</span>` : ""}`, v: g(f).alive, max: g(f).start || 1 }) },
    { label: "Vehicle units", cell: (f) => ({ txt: String(g(f).vehicles) }) },
    { label: "Human-led", cell: (f) => ({ txt: String(g(f).human) }) },
    { label: "Contacts known", cell: (f) => ({ txt: String(g(f).contacts) }) },
    { label: "By kind", cell: (f) => ({ txt: html`<span class="kinds">${g(f).kinds.size ? [...g(f).kinds.entries()].map(([k, n]) => html`<span class=${"badge kind k-" + k} key=${k}>${kindLabel(k)} ${n}</span>`) : "-"}</span>` }) },
    ...(full ? [
      { label: "Engaged", cell: (f) => ({ txt: String(g(f).engaged) }) },
      { label: "Moving", cell: (f) => ({ txt: String(g(f).moving) }) },
      { label: "Wounded", cell: (f) => ({ txt: String(g(f).wounded) }) },
    ] : []),
    { label: "Updated", cell: (f) => ({ txt: ago(st, st.factions[f]?.ts), dim: true }) },
  ];
  return html`<section class="panel"><h2>${title} <span class="sub">per side</span></h2><div class="scroll"><${Compare} names=${names} rows=${out} /></div></section>`;
}

// ---------- objectives + recent events ----------
function ObjectivesPanel({ st }) {
  const { objs, viewer } = publicPicture(st);
  if (!objs.length) return null;
  // progress is signed from the viewer's side (+100 = viewer's, -100 = enemy's): show who it favours, in that faction's colour
  const other = FACTIONS.find((x) => x !== viewer);
  const prog = (o) => { if (!num(o.progress) || o.progress === 0) return html`<span class="dim">0 %</span>`; const to = o.progress > 0 ? viewer : other, c = to ? colorOf(to) : NEUTRAL;
    return html`<span class="pbar" title=${"toward " + (to ?? "?")}><i style=${{ width: Math.min(100, Math.abs(o.progress)) + "%", background: c }}></i></span><span class="mono">${Math.abs(o.progress)} %</span>`; };
  return html`<section class="panel side"><h2>Objectives <span class="sub">${objs.length} points</span></h2><div class="scroll"><table>
    <thead><tr><th>Id</th><th>Name</th><th>Value</th><th>Owner</th><th>Progress</th><th>Position x, z</th></tr></thead><tbody>
    ${objs.map((o) => html`<tr key=${o.id}><td><b>${o.id}</b></td><td>${dash(o.name)}</td><td>${dash(o.value)}</td>
      <td>${o.owner && o.owner !== "none" ? html`<b class=${facClass(o.owner)}>${o.owner}</b>` : html`<span class="dim">neutral</span>`}${o.contested ? html` <span class="badge warn">contested</span>` : ""}</td>
      <td>${prog(o)}</td><td class="mono">${Math.round(o.pos[0])}, ${Math.round(o.pos[1])}</td></tr>`)}
    </tbody></table></div></section>`;
}
function EventsPanel({ st, faction }) {
  const list = [];
  for (const f of Object.keys(st.factions ?? {})) if (faction === "all" || f === faction) for (const e of st.factions[f].eventLog ?? []) list.push({ ...e, faction: f });
  list.sort((a, b) => b.ts - a.ts);
  if (!list.length) return null;
  return html`<section class="panel side"><h2>Recent events <span class="sub">newest first</span></h2><div class="scroll"><table><tbody>
    ${list.slice(0, 30).map((e, i) => html`<tr key=${i}><td class="mono muted">${new Date(e.ts).toLocaleTimeString()}</td><td class=${facClass(e.faction)}>${e.faction}</td><td class="wrap">${e.text}</td></tr>`)}
    </tbody></table></div></section>`;
}

// ---------- sitrep ----------
function Sitrep({ st, faction }) {
  const names = sortedKeys(st.factions).filter((f) => faction === "all" || f === faction);
  return html`<section class="panel side"><h2>Known contacts <span class="sub">per HQ</span></h2><div class="scroll">
    ${names.map((f) => {
      const e = st.factions[f], sit = e.situation ?? {}, ct = Array.isArray(sit.contacts) ? sit.contacts : [];
      return html`<div class="facblock"><div class="hd"><b class=${facClass(f)}>${f}</b>
        <span class="muted">time ${dash(sit.time)} s, ${Array.isArray(sit.squads) ? sit.squads.length : 0} squads, updated ${(((st.now ?? Date.now()) - e.ts) / 1000).toFixed(0)} s ago</span></div>
        <table><thead><tr><th>Contact</th><th>Position x, z</th><th>Age</th></tr></thead><tbody>
        ${ct.length ? ct.map((c) => html`<tr><td>${dash(c.desc)}${c.kind && c.kind !== "unknown" ? html` <span class=${"badge " + (c.kind === "vehicle" ? "bad" : "info")}>${c.kind}</span>` : ""}</td><td class="mono">${gridTxt(c.pos)}</td><td>${dash(c.ageSec)} s</td></tr>`) : html`<tr><td colspan="3" class="dim">no contacts</td></tr>`}
        </tbody></table></div>`;
    })}
    ${!names.length && html`<div class="empty">No data yet.</div>`}</div></section>`;
}

// ---------- exchange log ----------
const J = (o) => JSON.stringify(o, null, 2);
function Log({ st, faction }) {
  const ex = (st.exchanges ?? []).filter((x) => faction === "all" || x.faction === faction);
  const noLlm = st.paused || (st.config?.engine && st.config.engine !== "llm");
  return html`<section class="panel log"><h2>Orders / log <span class="sub">newest first, last ${st.exchanges?.length ?? 0}</span>
    ${noLlm && html`<span class="badge warn">HQ engine: ${st.config?.engine && st.config.engine !== "llm" ? st.config.engine : "llm"}${st.paused ? " (paused)" : ""}, no LLM calls</span>`}</h2><div class="scroll">
    ${ex.map((x) => html`<details class="ex" key=${x.ts + x.faction}><summary>
      <span class="mono muted">${new Date(x.ts).toLocaleTimeString()}</span><b class=${facClass(x.faction)}>${x.faction}</b>
      <span class="muted">${x.provider}${x.model ? " / " + x.model : ""}</span>
      ${x.ok && String(x.reason ?? "").startsWith("skipped") ? html`<span class="badge info">${x.reason}</span>` : x.ok ? html`<span class=${"badge " + (x.orders.length ? "ok" : "warn")}>${x.orders.length ? "ok" : "ok, no orders"}</span>` : html`<span class="badge bad">fallback: ${x.reason}</span>`}
      ${x.dropped?.length ? html`<span class="badge warn">${x.dropped.length} dropped</span>` : ""}
      ${x.wake && html`<span class=${"badge " + (x.wake.reason === "trigger" ? "warn" : "info")} title=${x.wake.triggers?.join("\n") ?? ""}>wake: ${x.wake.reason}${x.wake.triggers?.length ? ": " + x.wake.triggers.slice(0, 2).join("; ") + (x.wake.triggers.length > 2 ? ` +${x.wake.triggers.length - 2}` : "") : ""}</span>`}
      ${x.nextCheck && html`<span class="muted" title=${x.nextCheck.reason}>next ${x.nextCheck.sec} s${x.nextCheck.reason ? ": " + x.nextCheck.reason : ""}</span>`}
      <span>${x.orders.length} orders</span>
      ${x.buy?.length ? html`<span class="badge ok" title=${x.buy.map((b) => `${b.count}x ${b.type}`).join(", ")}>buy ${x.buy.map((b) => `${b.count}x ${b.type}`).join(", ")}</span>` : ""}
      ${x.missions?.length ? html`<span class="badge info">${x.missions.reduce((n, m) => n + m.offers.length, 0)} mission offers</span>` : ""}
      ${x.keep ? html`<span class="badge info">keep</span>` : ""}<span class="muted">${x.latencyMs} ms${x.usage ? ` , ${x.usage.input}/${x.usage.output} tok` : ""}${num(x.usage?.cost) ? ` , $${x.usage.cost.toFixed(4)}` : ""}</span></summary>
      <div class="body"><h3>Orders</h3><pre>${J(x.orders)}</pre>
      ${x.buy?.length ? html`<h3>Purchases</h3><pre>${J(x.buy)}</pre>` : ""}
      ${x.missions?.length ? html`<h3>Mission offers (human-led squads)</h3><pre>${J(x.missions)}</pre>` : ""}
      ${x.request?.events?.length ? html`<h3>Events in this request</h3><pre>${x.request.events.join("\n")}</pre>` : ""}
      ${x.dropped?.length ? html`<h3>Dropped (with reason)</h3><pre>${J(x.dropped)}</pre>` : ""}
      ${x.rawReply != null ? html`<h3>Raw LLM reply</h3><pre>${x.rawReply}</pre>` : ""}
      <h3>Request</h3><pre>${J(x.request)}</pre></div></details>`)}
    ${!ex.length && html`<div class="empty">No exchanges yet.</div>`}</div></section>`;
}

// ---------- world: extent, map image status, roads, places (from the sidecar; the game posts them) ----------
const PLACE_RANK = (t) => (/city/.test(t) ? [0, 0] : /town|airport|port|base/.test(t) ? [1, 0.03] : /village/.test(t) ? [2, 0.08] : [3, 0.15]);
function prepVector(vec) {
  const groups = new Map();
  for (const r of Array.isArray(vec.roads) ? vec.roads : []) {
    if (!r || !Array.isArray(r.p) || r.p.length < 4) continue;
    let x0 = Infinity, x1 = -Infinity, z0 = Infinity, z1 = -Infinity;
    for (let i = 0; i < r.p.length; i += 2) { x0 = Math.min(x0, r.p[i]); x1 = Math.max(x1, r.p[i]); z0 = Math.min(z0, r.p[i + 1]); z1 = Math.max(z1, r.p[i + 1]); }
    const w = Math.max(2, Math.round(r.w || 0));
    if (!groups.has(w)) groups.set(w, { w, list: [] });
    groups.get(w).list.push({ p: r.p, x0, x1, z0, z1 });
  }
  const all = (Array.isArray(vec.locations) ? vec.locations : []).filter((l) => l && Array.isArray(l.pos)).map((l) => {
    const type = String(l.type ?? "");
    // kind from the game; entries cached before it existed fall back to the descriptor type name
    let kind = typeof l.kind === "string" && l.kind ? l.kind : /airport/.test(type) ? "airport" : /port/.test(type) ? "port" : /base/.test(type) ? "base" : "other";
    if (kind === "other") { // generic descriptors ("name_generic"): classify by the display name
      const nm = String(l.name ?? "").toLowerCase();
      kind = /air(field|port|strip)|runway/.test(nm) ? "airport" : /harbou?r|\bport\b|\bdock/.test(nm) ? "port" : /military|\bbase\b|barracks|\bcamp\b|outpost/.test(nm) ? "base" : "other";
    }
    const [rank, minZoom] = PLACE_RANK(type);
    return { name: String(l.name), pos: l.pos, rank, minZoom, kind, faction: String(l.faction ?? "") };
  }).sort((a, b) => a.rank - b.rank);
  const OBJ_ZOOM = { airport: 0, base: 0.02, port: 0.03 };
  const objects = all.filter((l) => l.kind in OBJ_ZOOM).map((l) => ({ ...l, minZoom: OBJ_ZOOM[l.kind] }));
  const places = all.filter((l) => !(l.kind in OBJ_ZOOM));
  // buildings [cx, cz, w, d, yawDeg, cat], sorted by cx so drawing can binary-search the viewport's x range
  const buildings = (Array.isArray(vec.buildings) ? vec.buildings : []).filter((b) => Array.isArray(b) && b.length >= 5).map((b) => ({ x: b[0], z: b[1], w: b[2], d: b[3], c: Math.cos(b[4] * Math.PI / 180), s: Math.sin(b[4] * Math.PI / 180), cat: b[5] | 0 }));
  buildings.sort((a, b) => a.x - b.x);
  return { roadGroups: [...groups.values()].sort((a, b) => a.w - b.w), places, objects, buildings };
}
function useWorld() {
  const [w, setW] = useState({ extent: null, map: { status: "none" }, roadGroups: [], places: [], objects: [], buildings: [] });
  useEffect(() => {
    let alive = true, vkey = "";
    const load = async () => {
      try {
        const j = await (await fetch("/api/world", { cache: "no-store" })).json();
        let vec = null;
        const key = `${j.worldKey}/${j.vector?.roads}/${j.vector?.locations}/${j.vector?.buildings}`;
        if (key !== vkey) { vec = prepVector(await (await fetch("/api/vector", { cache: "no-store" })).json()); vkey = key; }
        if (alive) setW((p) => ({ ...p, extent: j.extent, map: j.map, ...(vec ?? {}) }));
      } catch { /* sidecar offline: keep the last known world */ }
    };
    load();
    const t = setInterval(load, 5000);
    return () => { alive = false; clearInterval(t); };
  }, []);
  return w;
}

// ---------- views ----------
const LAYER_KEY = "clw.mapLayers";
const LAYER_DEFAULTS = { image: true, roads: true, buildings: true, objects: true, places: true, grid: true, objectives: true, squads: true, contacts: true, orders: true, labels: true, flip: false, dim: 0.8 };
function loadLayers() {
  try { return { ...LAYER_DEFAULTS, ...JSON.parse(localStorage.getItem(LAYER_KEY) ?? "{}") }; } catch { return { ...LAYER_DEFAULTS }; }
}
const ago = (st, ts) => (ts ? `${Math.max(0, Math.round(((st.now ?? Date.now()) - ts) / 1000))} s ago` : "-");

// Faction selector + "show inactive" toggle; shared by all views (except Usage & Config).
function FactionBar({ names, faction, setFaction, hidden, showInactive, setShowInactive }) {
  return html`<div class="toolbar subbar">
    <div class="seg">${["all", ...names].map((n) => html`<button key=${n} class=${faction === n ? "on" : ""} onClick=${() => setFaction(n)}>${n}</button>`)}</div>
    <label class="toggles inact" title="Factions that stopped posting (setup-phase factions before the host picked others, or a previous war) are hidden. Show them for debugging.">
      <span><input type="checkbox" checked=${showInactive} onChange=${(e) => setShowInactive(e.target.checked)} /> show inactive factions${hidden.length ? ` (${hidden.length} hidden: ${hidden.join(", ")})` : ""}</span></label>
  </div>`;
}

// ---- Overview ----
function OwnerBar({ st }) {
  const { objs } = publicPicture(st);
  if (!objs.length) return null;
  const wt = (o) => (num(o.value) && o.value > 0 ? o.value : 1), tot = objs.reduce((n, o) => n + wt(o), 0), by = new Map(), hatch = new Map();
  for (const o of objs) {
    const k = o.owner && o.owner !== "none" ? o.owner : "";
    by.set(k, (by.get(k) ?? 0) + wt(o));
    if (o.contested) hatch.set(k, (hatch.get(k) ?? 0) + wt(o));
  }
  // side A | neutral | side B (| further factions): both sides keep their slot even when they hold nothing
  const facs = [...new Set([...Object.keys(st.factions ?? {}), ...by.keys()].filter((k) => k))].sort(facCmp);
  const keys = facs.length ? [facs[0], "", ...facs.slice(1)] : [""];
  const contested = objs.filter((o) => o.contested).length;
  return html`<section class="panel"><h2>Objective control <span class="sub">${objs.length} points, weighted by value${contested ? `, ${contested} contested` : ""}</span></h2><div class="cfgbody">
    <div class="ownbar">${keys.map((k) => html`<i key=${k} style=${{ width: (100 * (by.get(k) ?? 0) / tot) + "%", background: k ? colorOf(k) : NEUTRAL }} title=${(k || "neutral") + ": " + (by.get(k) ?? 0) + " / " + tot + (hatch.get(k) ? ", contested " + hatch.get(k) : "")}>${hatch.get(k) ? html`<b class="hatch" style=${{ width: (100 * hatch.get(k) / by.get(k)) + "%" }}></b>` : ""}</i>`)}</div>
    <div class="ownleg">${keys.map((k) => html`<span key=${k}><b class=${k ? facClass(k) : "f-none"}>${k || "neutral"}</b> <span class="muted">${by.get(k) ?? 0} / ${tot}</span></span>`)}</div></div></section>`;
}
function Overview({ st, faction, rows }) {
  const names = sortedKeys(st.factions).filter((f) => faction === "all" || f === faction);
  return html`<main class="view">
    <${OwnerBar} st=${st} />
    <div class="grid2 gap">
      <${GameModePanel} st=${st} names=${names} />
      <${ForcesCompare} st=${st} rows=${rows} names=${names} />
    </div>
    <div class="grid2 gap">${names.map((f) => html`<${SitrepCard} key=${f} f=${f} e=${st.factions[f]} now=${st.now} />`)}</div>
    <div class="grid2 gap"><${ObjectivesPanel} st=${st} /><${EventsPanel} st=${st} faction=${faction} /></div>
    ${!Object.keys(st.factions ?? {}).length && html`<div class="empty">No data yet: waiting for the game to post a situation.</div>`}</main>`;
}

// ---- Map ----
function SelCard({ rows, sel }) {
  const r = sel ? rows.find((x) => x.key === sel) : null;
  if (!r) return html`<div class="muted selcard">Click a squad on the map to see its details.</div>`;
  const o = r.obj;
  return html`<div class="selcard"><b class=${facClass(r.faction)}>${r.id}</b> ${r.kind ? html`<span class=${"badge kind k-" + r.kind}>${kindLabel(r.kind)}</span>` : ""} <span class=${"badge " + stateBadge(r.state)}>${r.state}</span> <${DormBadge} r=${r} />${r.dormant === false ? html`<span class="badge actv">active</span>` : ""}
    <span class="muted">${r.size} soldiers${r.pct !== null ? ", " + r.pct + "%" : ""}${r.vehicle ? ", " + dash(r.vehicle.type) + (r.vehicle.mounted ? " (mounted)" : " (dismounted)") : ""}, ${r.move}, at ${r.posTxt}</span>
    ${o ? html` <span class=${"cmd " + o.command}>${o.command}</span> ${o.target ?? ""} <span class="muted">${o.reason ?? ""}</span>` : ""}${r.human ? html` <span class="badge human">human</span>` : ""}</div>`;
}
function MapView({ st, faction, rows, sel, onSel, layers, setLayers, world, act, setAct }) {
  const names = sortedKeys(st.factions).filter((f) => faction === "all" || f === faction);
  const tog = (k) => html`<label key=${k}><input type="checkbox" checked=${layers[k]} onChange=${() => setLayers((l) => ({ ...l, [k]: !l[k] }))} /> ${k}</label>`;
  return html`<main class="view">
    <div class="toolbar">
      <${ActFilter} act=${act} setAct=${setAct} rows=${rows} /><${ActLegend} names=${names} rows=${rows} />
      <div class="toggles nomargin">${["grid", "objectives", "squads", "contacts", "orders", "labels"].map(tog)}</div>
      <div class="toggles" title="Map background: toggles, image opacity; flip = image y axis inverted (calibration)">
        ${["image", "roads", "buildings", "objects", "places", "flip"].map(tog)}
        <span class="muted" title="Objects key: diamond = base, triangle over a bar = airport, ring = port. Buildings: grey = house, large = bigger than 250 m2, orange = military / industrial">◆ base ▲ airport ◎ port</span>
        <label>dim <input type="range" min="0.1" max="1" step="0.05" value=${layers.dim} onInput=${(e) => setLayers((l) => ({ ...l, dim: Number(e.target.value) }))} style=${{ width: "80px", verticalAlign: "middle" }} /></label>
      </div>
    </div>
    <${MapPanel} st=${st} faction=${faction} rows=${rows} sel=${sel} onSel=${onSel} layers=${layers} world=${world} tall=${true} act=${act} />
    <${SelCard} rows=${rows} sel=${sel} /></main>`;
}

// ---- Forces ----
function CatalogPanel({ st, f }) {
  const e = st.factions[f], sit = e.situation ?? {}, sup = sit.war?.supplies;
  const cat = (Array.isArray(sit.catalog) ? sit.catalog : []).filter((c) => c && typeof c.type === "string").sort((a, b) => (num(a.cost) ? a.cost : 1e9) - (num(b.cost) ? b.cost : 1e9));
  return html`<section class="panel" style=${accent(f)}><h2><span class=${facClass(f)}>${f}</span> buy list <span class="sub">supplies ${dash(sup)}${num(sit.war?.income) ? ` (+${sit.war.income}/min)` : ""}</span></h2><div class="scroll"><table>
    <thead><tr><th>Type</th><th>Kind</th><th>Cost</th><th>Affordable</th><th>Description</th></tr></thead><tbody>
    ${cat.map((c) => html`<tr key=${c.type}><td><b>${c.type}</b></td><td>${c.kind ? html`<span class=${"badge kind k-" + c.kind}>${kindLabel(c.kind)}</span>` : "-"}</td><td class="mono">${dash(c.cost)}</td>
      <td>${num(c.cost) && num(sup) ? html`<span class=${"badge " + (c.cost <= sup ? "ok" : "bad")}>${c.cost <= sup ? "yes" : "short " + (c.cost - sup)}</span>` : "-"}</td><td class="wrap muted">${dash(c.desc)}</td></tr>`)}
    </tbody></table>${!cat.length && html`<div class="empty">No catalog reported.</div>`}</div></section>`;
}
function PurchasesPanel({ st, faction }) {
  const list = [];
  for (const x of st.exchanges ?? []) if ((faction === "all" || x.faction === faction) && x.buy?.length) for (const b of x.buy) list.push({ ts: x.ts, faction: x.faction, ...b });
  return html`<section class="panel"><h2>Purchases <span class="sub">decided by the HQ, this sidecar session</span></h2><div class="scroll"><table>
    <thead><tr><th>Time</th><th>Faction</th><th>Order</th><th>Reason</th></tr></thead><tbody>
    ${list.slice(0, 60).map((b, i) => html`<tr key=${i}><td class="mono muted">${new Date(b.ts).toLocaleTimeString()}</td><td class=${facClass(b.faction)}>${b.faction}</td><td><b>${b.count}x ${b.type}</b></td><td class="wrap muted">${dash(b.reason)}</td></tr>`)}
    </tbody></table>${!list.length && html`<div class="empty">No purchases yet.</div>`}</div></section>`;
}
function ForcesView({ st, faction, rows, sel, onSel, act, setAct }) {
  const names = sortedKeys(st.factions).filter((f) => faction === "all" || f === faction);
  const mine = rows.filter((r) => faction === "all" || r.faction === faction), vis = mine.filter((r) => actOK(act, r.dormant === true));
  return html`<main class="view forces">
    <${ForcesCompare} st=${st} rows=${mine} names=${names} full=${true} />
    <div class="toolbar"><${ActFilter} act=${act} setAct=${setAct} rows=${mine} /><${ActLegend} names=${names} rows=${mine} /></div>
    <${SquadsTable} rows=${vis} sel=${sel} onSel=${onSel} />
    <div class="grid2 gap">${names.map((f) => html`<${CatalogPanel} key=${f} st=${st} f=${f} />`)}</div>
    <${PurchasesPanel} st=${st} faction=${faction} /></main>`;
}

// ---- HQ / LLM ----
function Decision({ st, f }) {
  const exs = (st.exchanges ?? []).filter((x) => x.faction === f);
  const x = exs.find((e) => !(e.ok && String(e.reason ?? "").startsWith("skipped"))) ?? exs[0];
  const sc = st.scheduler?.[f];
  const e = st.factions[f];
  const dropLabel = (d) => { const k = Object.keys(d).find((q) => q !== "reason"); return k ? `${k}: ${J(d[k])}` : ""; };
  return html`<section class="panel" style=${accent(f)}><h2><span class=${facClass(f)}>${f}</span> HQ decision <span class="sub">${x ? new Date(x.ts).toLocaleTimeString() : "no exchange yet"}</span></h2><div class="cfgbody">
    ${sc && html`<div class="muted" title=${(sc.lastWakeTriggers ?? []).join("\n")}>Scheduler: next call ${sc.manual ? "pending (wake)" : sc.nextDueInSec > 0 ? "in " + sc.nextDueInSec + " s" : "due"}, interval ${dash(sc.intervalSec)} s (chosen ${dash(sc.chosenSec)} s${sc.chosenReason ? ": " + sc.chosenReason : ""}), budget stretch x${(sc.stretch ?? 1).toFixed(2)},
      last wake ${sc.lastWakeReason ?? "-"}${sc.lastWakeTriggers?.length ? " (" + sc.lastWakeTriggers.join("; ") + ")" : ""}, last LLM call ${sc.lastLlmAgoSec === null || sc.lastLlmAgoSec === undefined ? "never" : sc.lastLlmAgoSec + " s ago"}, gated polls ${sc.gated ?? 0}${sc.failed ? ", last call FAILED (rule fallback)" : ""}</div>`}
    ${!x ? html`<div class="empty">No exchange for this faction yet.</div>` : html`
      <div class="hdr">${x.ok ? html`<span class=${"badge " + (x.orders.length || x.buy?.length || x.missions?.length ? "ok" : "warn")}>ok</span>` : html`<span class="badge bad">fallback: ${x.reason}</span>`}
        <span class="muted">${x.provider}${x.model ? " / " + x.model : ""}, ${x.latencyMs} ms${x.usage ? `, ${x.usage.input}/${x.usage.output} tok` : ""}${num(x.usage?.cost) ? `, $${x.usage.cost.toFixed(4)}` : ""}</span>
        ${x.wake && html`<span class="badge info" title=${x.wake.triggers?.join("\n") ?? ""}>wake: ${x.wake.reason}</span>`}
        ${x.nextCheck && html`<span class="muted">next check ${x.nextCheck.sec} s${x.nextCheck.reason ? ": " + x.nextCheck.reason : ""}</span>`}${x.keep ? html`<span class="badge info">keep</span>` : ""}</div>
      <h3 class="cfgh">Orders applied (${x.orders.length})</h3>
      ${x.orders.length ? html`<table><thead><tr><th>Squad</th><th>Command</th><th>Target</th><th>Reason</th></tr></thead><tbody>${x.orders.map((o, i) => html`<tr key=${i}><td><b>${o.squad}</b></td><td><span class=${"cmd " + o.command}>${o.command}</span></td>
        <td class="mono">${o.target ?? gridTxt(Array.isArray(o.pos) ? [o.pos[0], 0, o.pos[1]] : null)}</td><td class="wrap muted">${dash(o.reason)}</td></tr>`)}</tbody></table>` : html`<div class="dim">none</div>`}
      ${x.buy?.length ? html`<h3 class="cfgh">Buy (${x.buy.length})</h3><table><tbody>${x.buy.map((b, i) => html`<tr key=${i}><td><b>${b.count}x ${b.type}</b></td><td class="wrap muted">${dash(b.reason)}</td></tr>`)}</tbody></table>` : ""}
      ${x.missions?.length ? html`<h3 class="cfgh">Mission offers</h3><table><tbody>${x.missions.flatMap((m) => m.offers.map((o, i) => html`<tr key=${m.squad + i}><td><b>${m.squad}</b></td><td>${o.kind}</td><td class="mono">${o.target ?? gridTxt(Array.isArray(o.pos) ? [o.pos[0], 0, o.pos[1]] : null)}</td><td class="wrap muted">${dash(o.brief)}</td></tr>`))}</tbody></table>` : ""}
      ${x.dropped?.length ? html`<h3 class="cfgh">Dropped (${x.dropped.length})</h3><table><tbody>${x.dropped.map((d, i) => html`<tr key=${i}><td class="wrap mono">${dropLabel(d)}</td><td class="wrap"><span class="badge warn">${d.reason}</span></td></tr>`)}</tbody></table>` : ""}
      ${x.request?.events?.length ? html`<details><summary class="muted">Events in this request (${x.request.events.length})</summary><pre>${x.request.events.join("\n")}</pre></details>` : ""}
      <details><summary class="muted">Raw LLM reply</summary><pre>${x.rawReply != null ? x.rawReply : "(no raw reply: rules / fake / skipped)"}</pre></details>`}
    ${e.orders?.length ? html`<div class="muted">Standing orders in the game: ${e.orders.length}</div>` : ""}
  </div></section>`;
}
function HqView({ st, faction }) {
  const names = sortedKeys(st.factions).filter((f) => faction === "all" || f === faction);
  return html`<main class="view">
    <div class="grid2">${names.map((f) => html`<${Decision} key=${f} st=${st} f=${f} />`)}</div>
    <div class="grid" style=${{ marginTop: "12px" }}><div class="col"><${Log} st=${st} faction=${faction} /></div><div class="col"><${Sitrep} st=${st} faction=${faction} /></div></div></main>`;
}

// ---- Missions (human squad leaders) ----
function MissionsView({ st, faction, rows, sel, onSel, layers, world, act }) {
  const [focus, setFocus] = useState(null);
  const humans = rows.filter((r) => r.human), { objs } = publicPicture(st), objById = new Map(objs.map((o) => [o.id, o]));
  const offers = [];
  for (const f of Object.keys(st.factions ?? {})) if (faction === "all" || f === faction) for (const m of st.factions[f].missions ?? []) for (const o of m.offers ?? []) offers.push({ faction: f, squad: m.squad, ...o });
  const hist = [];
  for (const x of st.exchanges ?? []) if ((faction === "all" || x.faction === faction) && x.missions?.length) for (const m of x.missions) for (const o of m.offers ?? []) hist.push({ ts: x.ts, faction: x.faction, squad: m.squad, ...o });
  const evs = [];
  for (const f of Object.keys(st.factions ?? {})) if (faction === "all" || f === faction) for (const e of st.factions[f].eventLog ?? []) if (/mission/i.test(e.text)) evs.push({ ...e, faction: f });
  evs.sort((a, b) => b.ts - a.ts);
  const noHum = Object.keys(st.factions ?? {}).filter((f) => (faction === "all" || f === faction) && !rows.some((r) => r.human && r.faction === f));
  const why = noHum.length ? ` HQ only offers missions to player-led squads; none in ${noHum.join(", ")}.` : "";
  const stBadge = (s) => (s === "accepted" ? "ok" : s === "offered" ? "warn" : s === "completed" ? "info" : "");
  // where an offer / mission points: objective id -> objective centre, else explicit [x, z], else (fallback) the squad
  const aim = (o, squadKey) => {
    const ob = o.target ? objById.get(o.target) : null;
    if (ob) return { pos: ob.pos, label: ob.id + (ob.name ? " " + ob.name : "") };
    if (Array.isArray(o.pos) && num(o.pos[0]) && num(o.pos[1])) return { pos: [o.pos[0], o.pos[1]], label: "target" };
    const r = rows.find((x) => x.key === squadKey);
    return r && okPos(r.pos) ? { pos: [r.pos[0], r.pos[2]], label: r.id } : null;
  };
  const go = (key, a, squadKey) => { if (!a) return; if (squadKey) onSel(squadKey); setFocus({ key, pos: a.pos, label: a.label, nonce: Date.now() }); };
  const mapLayers = { ...layers, grid: false, buildings: false, labels: true };
  const tgt = (o) => o.target ?? gridTxt(Array.isArray(o.pos) ? [o.pos[0], 0, o.pos[1]] : null);
  return html`<main class="view">
    <div class="grid2">
      <section class="panel"><h2>Human-led squads <span class="sub">${humans.length}, click a row to show it on the map</span></h2><div class="scroll"><table>
        <thead><tr><th>Squad</th><th>Faction</th><th>Mission</th><th>Kind</th><th>Target</th><th>State</th></tr></thead><tbody>
        ${humans.map((r) => { const key = "h:" + r.key;
          return html`<tr key=${r.key} class=${"sqrow" + (focus?.key === key ? " sel" : "")} onClick=${() => go(key, aim(r.mission ?? {}, r.key), r.key)}><td style=${{ borderLeft: "3px solid " + colorOf(r.faction) }}><b>${r.id}</b></td><td class=${facClass(r.faction)}>${r.faction}</td>
          <td>${r.mission ? html`<span class=${"badge " + stBadge(r.mission.state)}>${r.mission.state}</span>` : html`<span class="dim">none</span>`}</td><td>${dash(r.mission?.kind)}</td><td>${dash(r.mission?.target)}</td>
          <td><span class=${"badge " + stateBadge(r.state)}>${r.state}</span></td></tr>`; })}</tbody></table>
        ${!humans.length && html`<div class="empty">No human-led squads right now.</div>`}</div></section>
      <section class="panel"><h2>Current offers <span class="sub">latest HQ reply, click to show the target</span></h2><div class="scroll"><table>
        <thead><tr><th>Faction</th><th>Squad</th><th>Kind</th><th>Target</th><th>Brief</th></tr></thead><tbody>
        ${offers.map((o, i) => { const key = "o:" + i; return html`<tr key=${i} class=${"sqrow" + (focus?.key === key ? " sel" : "")} onClick=${() => go(key, aim(o, o.faction + "/" + o.squad), o.faction + "/" + o.squad)}><td style=${{ borderLeft: "3px solid " + colorOf(o.faction) }} class=${facClass(o.faction)}>${o.faction}</td><td><b>${o.squad}</b></td><td>${o.kind}</td><td class="mono">${tgt(o)}</td><td class="wrap muted">${dash(o.brief)}</td></tr>`; })}</tbody></table>
        ${!offers.length && html`<div class="empty">No LLM offers pending (without them the game plans offers itself).${why}</div>`}</div></section>
    </div>
    <${MapPanel} st=${st} faction=${faction} rows=${rows} sel=${sel} onSel=${onSel} layers=${mapLayers} world=${world} act=${act} focus=${focus} compact=${true} />
    <div class="grid2" style=${{ marginTop: "12px" }}>
      <section class="panel"><h2>Mission events <span class="sub">offered / accepted / completed / failed</span></h2><div class="scroll"><table><tbody>
        ${evs.slice(0, 40).map((e, i) => html`<tr key=${i}><td class="mono muted">${new Date(e.ts).toLocaleTimeString()}</td><td class=${facClass(e.faction)}>${e.faction}</td><td class="wrap">${e.text}</td></tr>`)}</tbody></table>
        ${!evs.length && html`<div class="empty">No mission events yet.${why}</div>`}</div></section>
      <section class="panel"><h2>Offer history <span class="sub">from HQ replies this session, click to show the target</span></h2><div class="scroll"><table>
        <thead><tr><th>Time</th><th>Faction</th><th>Squad</th><th>Kind</th><th>Target</th><th>Brief</th></tr></thead><tbody>
        ${hist.slice(0, 60).map((o, i) => { const key = "x:" + i; return html`<tr key=${i} class=${"sqrow" + (focus?.key === key ? " sel" : "")} onClick=${() => go(key, aim(o, o.faction + "/" + o.squad), null)}><td class="mono muted">${new Date(o.ts).toLocaleTimeString()}</td><td class=${facClass(o.faction)}>${o.faction}</td><td><b>${o.squad}</b></td><td>${o.kind}</td><td class="mono">${tgt(o)}</td><td class="wrap muted">${dash(o.brief)}</td></tr>`; })}</tbody></table>
        ${!hist.length && html`<div class="empty">No offers yet.${why}</div>`}</div></section>
    </div></main>`;
}

// ---- debug log: the game's CLW_Debug lines (not shown in game chat by default), newest first ----
function DebugView({ st }) {
  const [q, setQ] = useState("");
  const all = Array.isArray(st.debugLog) ? st.debugLog : [];
  const needle = q.trim().toLowerCase();
  const rows = (needle ? all.filter((d) => String(d.msg).toLowerCase().includes(needle)) : all).slice(0, 300);
  const clock = (sec) => { const s = Math.max(0, Math.round(sec)), p = (v) => String(v).padStart(2, "0"); return `${p(Math.floor(s / 3600))}:${p(Math.floor((s % 3600) / 60))}:${p(s % 60)}`; };
  return html`<main class="view"><section class="panel"><h2>Debug log <span class="sub">game debug lines, newest first, ${rows.length} of ${all.length}</span></h2>
    <div class="cfgbody"><label class="field"><span>Filter</span><input type="text" value=${q} placeholder="e.g. mission, spawn, captured" onInput=${(e) => setQ(e.target.value)} /></label></div>
    <div class="scroll"><table><tbody>
      ${rows.map((d, i) => html`<tr key=${i}><td class="mono muted">${new Date(d.ts).toLocaleTimeString()}</td><td class="mono muted">${clock(d.t)}</td><td class="wrap">${d.msg}</td></tr>`)}</tbody></table>
      ${!rows.length && html`<div class="empty">${all.length ? "No line matches the filter." : "No debug lines yet. They arrive with the game's situation posts while a war runs."}</div>`}</div></section></main>`;
}

// ---- action manifest (protocol v3): what each faction's game offers HQ in this war mode, read-only ----
function ManifestTable({ title, rows }) {
  return html`<h3 class="cfgh">${title} <span class="muted">(${rows.length})</span></h3>
    ${rows.length ? html`<div class="scroll"><table><thead><tr><th>Name</th><th>Reply key</th><th>Authority</th><th>Requires</th><th>Parameters</th><th>Description</th></tr></thead><tbody>
      ${rows.map((e) => html`<tr key=${e.name}><td><b>${e.name}</b></td><td class="mono">${e.channel}</td><td>${e.authority}</td><td class="mono">${(e.requires ?? []).join(", ") || "-"}</td>
        <td class="mono wrap">${(e.params ?? []).map((p) => p.name + (p.required ? "*" : "") + ":" + p.type).join(", ")}</td><td class="wrap muted">${e.desc}</td></tr>`)}</tbody></table></div>` : html`<div class="empty">none</div>`}`;
}
function ActionsView({ st }) {
  const ms = st.manifests ?? { factions: {}, builtin: null };
  const names = Object.keys(ms.factions ?? {}).sort();
  return html`<main class="view">
    ${names.map((n) => { const m = ms.factions[n]; return html`<section class="panel" key=${n}><h2>${n} <span class="sub">manifest ${m.hash}, mode ${m.mode || "?"}, received ${new Date(m.receivedAt).toLocaleTimeString()}</span></h2>
      <div class="cfgbody"><div class="muted">Capabilities on: <b>${(m.capabilities ?? []).join(", ") || "none"}</b></div>
      <${ManifestTable} title="Intents (strategic tick)" rows=${m.intents ?? []} /><${ManifestTable} title="Primitives (agent turns)" rows=${m.primitives ?? []} /></div></section>`; })}
    ${!names.length && ms.builtin && html`<section class="panel"><h2>Built-in manifest <span class="sub">no game manifest received yet; the sidecar uses this one</span></h2><div class="cfgbody"><${ManifestTable} title="Intents" rows=${ms.builtin.intents ?? []} /></div></section>`}
  </main>`;
}

// ---- tabs ----
const TABS = [["overview", "Overview"], ["map", "Map"], ["forces", "Forces"], ["hq", "HQ / LLM"], ["missions", "Missions"], ["actions", "Actions"], ["debug", "Debug log"], ["usage", "Usage & Config"]];
const TAB_KEY = "clw.tab", INACT_KEY = "clw.showInactive";
const tabOf = (s) => { const k = String(s ?? "").replace(/^#/, ""), t = k === "config" ? "usage" : k; return TABS.some(([id]) => id === t) ? t : null; };
function loadTab() {
  let saved = null;
  try { saved = localStorage.getItem(TAB_KEY); } catch { /* storage unavailable */ }
  return tabOf(location.hash) ?? tabOf(saved) ?? "overview";
}
function loadInactive() { try { return localStorage.getItem(INACT_KEY) === "1"; } catch { return false; } }

// ---------- configuration view ----------
async function postJson(url, body) {
  const r = await fetch(url, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
  let j = {}; try { j = await r.json(); } catch { /* ignore */ }
  return { ok: r.ok, j };
}
// Map image / vector data status and game dir (sidecar config keys gameDir, mapMaxSize).
function MapCard({ toast }) {
  const [w, setW] = useState(null);
  const [gameDir, setGameDir] = useState(null);
  const [maxSize, setMaxSize] = useState(2048);
  const load = useCallback(async () => {
    try { setW(await (await fetch("/api/world", { cache: "no-store" })).json()); } catch { /* offline */ }
  }, []);
  useEffect(() => {
    load(); const t = setInterval(load, 3000);
    fetch("/api/config", { cache: "no-store" }).then((r) => r.json()).then((c) => { setGameDir(c.gameDir ?? ""); setMaxSize(c.mapMaxSize ?? 2048); }).catch(() => {});
    return () => clearInterval(t);
  }, [load]);
  const act = async (url, body, okMsg) => {
    try { const { ok, j } = await postJson(url, body); toast(ok ? okMsg : "Failed: " + (j.error ?? "request failed"), ok ? "ok" : "err"); load(); }
    catch (err) { toast("Error: " + err.message, "err"); }
  };
  const m = w?.map ?? {}, v = w?.vector ?? {};
  return html`<section class="panel" style=${{ marginTop: "12px" }}><h2>Map <span class="sub">background image and roads / places for the HQ map</span></h2>
    <div class="cfgbody">
      <div class="muted">World <b>${w?.worldKey || "(game has not reported yet)"}</b>${w?.info?.mapPrefab ? " / " + w.info.mapPrefab : ""}</div>
      <div>Image: <b>${m.status ?? "none"}</b>${m.source ? html` <span class="muted">${m.source}, ${m.width}x${m.height}</span>` : ""}${m.error ? html` <span class="muted">${m.error}</span>` : ""}</div>
      <div>Roads: <b>${v.roads ?? "not received"}</b>, places: <b>${v.locations ?? "not received"}</b>, buildings: <b>${v.buildings ?? "not received"}</b></div>
      ${v.roads == null || v.locations == null || v.buildings == null ? html`<div class="dim">Waiting for the game: it resends within about 60 s (the game must be running).</div>` : null}
      <div class="muted">Detected game dir: ${w?.detectedGameDir || "none"}</div>
      <label class="field"><span>Game dir (empty = auto-detect)</span><input type="text" value=${gameDir ?? ""} placeholder="e.g. E:/Games/steam/steamapps/common/Arma Reforger" onInput=${(e) => setGameDir(e.target.value)} /></label>
      <label class="field"><span>Max image size (px)</span><input type="number" min="256" max="8192" value=${maxSize} onInput=${(e) => setMaxSize(e.target.value)} /></label>
      <div class="btns">
        <button class="btn" type="button" onClick=${() => act("/api/config", { gameDir: gameDir ?? "", mapMaxSize: Number(maxSize) }, "Map settings saved")}><${Icon} n="save" /> Save</button>
        <button class="btn ghost" type="button" onClick=${() => act("/api/map/reload", {}, "Re-resolving the map image")}><${Icon} n="reset" /> Re-resolve image</button>
        <button class="btn ghost" type="button" onClick=${() => act("/api/vector/reset", {}, "Vector data cleared; the game resends within 60 s")}><${Icon} n="reset" /> Resend roads</button></div>
      <small class="muted">The image is read at runtime from your own game install and is never committed. Modded terrains: put dev/tools/maps/NAME.png and NAME.json next to the sidecar.</small>
    </div></section>`;
}
function Config({ st, toast, refresh }) {
  const [f, setF] = useState(null);
  const [keyMeta, setKeyMeta] = useState({ set: false, last4: "" });
  const [busy, setBusy] = useState(false);
  const [test, setTest] = useState(null);
  const load = useCallback(async () => {
    try {
      const c = await (await fetch("/api/config", { cache: "no-store" })).json();
      setF({ provider: c.provider, model: c.model, baseUrl: c.baseUrl, apiKey: "", timeoutSec: c.timeoutSec, maxTokens: c.maxTokens, enabled: !!c.enabled, engine: c.engine ?? "llm", minIntervalSec: c.minIntervalSec ?? 0, skipUnchanged: c.skipUnchanged !== false, stopAtLimit: c.stopAtLimit !== false, wakeup: { ...c.wakeup }, triggers: { ...c.triggers } });
      setKeyMeta({ set: c.apiKeySet, last4: c.apiKeyLast4 });
    } catch (e) { toast("Could not load config: " + e.message, "err"); }
  }, []);
  useEffect(() => { load(); }, [load]);
  if (!f) return html`<main><div class="empty">Loading...</div></main>`;
  const set = (k, v) => setF((o) => ({ ...o, [k]: v }));
  const setG = (g, k, v) => setF((o) => ({ ...o, [g]: { ...o[g], [k]: v } }));
  const numG = (g) => Object.fromEntries(Object.entries(f[g]).map(([k, v]) => [k, typeof v === "boolean" ? v : Number(v)]));
  // number input / toggle bound to a group setting
  const gnum = (g, k, label, min, max) => html`<label class="field"><span>${label}</span><input type="number" min=${min} max=${max} value=${f[g][k]} onInput=${(e) => setG(g, k, e.target.value)} /></label>`;
  const gsw = (g, k, label) => html`<label class="switch"><input type="checkbox" checked=${f[g][k]} onChange=${(e) => setG(g, k, e.target.checked)} /><span>${label}</span></label>`;
  const body = () => {
    const b = { provider: f.provider, model: f.model, baseUrl: f.baseUrl, timeoutSec: Number(f.timeoutSec), maxTokens: Number(f.maxTokens), enabled: f.enabled, engine: f.engine, minIntervalSec: Number(f.minIntervalSec), skipUnchanged: f.skipUnchanged, stopAtLimit: f.stopAtLimit, wakeup: numG("wakeup"), triggers: numG("triggers") };
    if (f.apiKey) b.apiKey = f.apiKey;
    return b;
  };
  const save = async (e) => {
    e.preventDefault(); setBusy(true);
    try {
      const { ok, j } = await postJson("/api/config", body());
      if (!ok) toast("Not saved: " + (j.error ?? "request failed"), "err");
      else { toast(j.saved ? "Settings saved" : "Applied, but sidecar-config.json could not be written", j.saved ? "ok" : "err"); await load(); refresh(); }
    } catch (err) { toast("Error: " + err.message, "err"); }
    setBusy(false);
  };
  const doTest = async () => {
    setBusy(true); setTest({ pending: true });
    try {
      const { ok, j } = await postJson("/api/test", body());
      setTest(ok ? j : { ok: false, error: j.error ?? "request failed" });
      toast(ok && j.ok ? `Connection OK (${j.latencyMs} ms)` : "Test failed: " + (ok ? j.error : j.error), ok && j.ok ? "ok" : "err");
    } catch (err) { setTest({ ok: false, error: err.message }); toast("Error: " + err.message, "err"); }
    setBusy(false);
  };
  return html`<main class="view"><div class="cfgwrap">${st.usage?.enabled && html`<${UsageCard} u=${st.usage} now=${st.now} toast=${toast} refresh=${refresh} />`}<section class="panel" style=${{ marginTop: "12px" }}><h2>Configuration <span class="sub">saved to dev/tools/sidecar-config.json</span></h2>
    <form class="cfgbody" onSubmit=${save} autocomplete="off">
      <label class="field"><span>Provider</span><select value=${f.provider} onChange=${(e) => set("provider", e.target.value)}>
        <option value="fake">fake (rule-based stub, no network)</option>
        <option value="anthropic">anthropic</option>
        <option value="openai-compatible">openai-compatible (Ollama, LM Studio, ...)</option>
        <option value="openrouter">openrouter (with usage / budget monitoring)</option></select></label>
      <label class="field"><span>HQ engine</span><select value=${f.engine} onChange=${(e) => set("engine", e.target.value)}>
        <option value="llm">llm (LLM provider decides)</option>
        <option value="rules">rules (game rule commander, no LLM calls)</option>
        <option value="model" disabled>model (soon)</option></select>
        <small>Main switch. rules: the sidecar answers with no orders so the game's own rule commander drives; the HQ scheduler is idle. model: a future trained decision model.</small></label>
      <label class="field"><span>Model</span><input type="text" value=${f.model} onInput=${(e) => set("model", e.target.value)} /></label>
      <label class="field"><span>Base URL</span><input type="text" value=${f.baseUrl} placeholder=${f.provider === "openrouter" ? "optional, default https://openrouter.ai/api/v1" : "e.g. http://127.0.0.1:11434/v1  (openai-compatible; /chat/completions is appended)"} onInput=${(e) => set("baseUrl", e.target.value)} /></label>
      <label class="field"><span>API key</span><input type="password" autocomplete="new-password" value=${f.apiKey} placeholder=${keyMeta.set ? `stored key ending ...${keyMeta.last4} (leave empty to keep)` : "not set"} onInput=${(e) => set("apiKey", e.target.value)} />
        <small>Write-only: the sidecar never sends the key back.</small></label>
      <div class="pair">
        <label class="field"><span>Timeout (s)</span><input type="number" min="1" max="300" value=${f.timeoutSec} onInput=${(e) => set("timeoutSec", e.target.value)} /></label>
        <label class="field"><span>Max tokens</span><input type="number" min="16" max="32000" value=${f.maxTokens} onInput=${(e) => set("maxTokens", e.target.value)} /></label></div>
      <div class="pair">
        <label class="field"><span>Min interval per faction (s, 0 = every request)</span><input type="number" min="0" max="600" value=${f.minIntervalSec} onInput=${(e) => set("minIntervalSec", e.target.value)} /></label>
        <label class="switch"><input type="checkbox" checked=${f.skipUnchanged} onChange=${(e) => set("skipUnchanged", e.target.checked)} /><span>Skip unchanged situations</span></label></div>
      <small class="muted">Batch APIs (24 h latency) don't fit a live war, so we cut call frequency instead. Skipped requests answer "keep current orders". The min interval is the hard floor between any two LLM calls (also for wakeups and triggers); unchanged situations are skipped at scheduled wakeups.</small>
      <h3 class="cfgh">Wakeup</h3>
      ${gsw("wakeup", "dynamic", "Let the LLM choose its next wakeup (next_check_sec)")}
      <div class="pair">${gnum("wakeup", "defaultSec", "Default interval (s)", 5, 3600)}${gnum("wakeup", "minSec", "Shortest interval (s)", 5, 3600)}</div>
      <div class="pair">${gnum("wakeup", "maxSec", "Longest interval (s)", 5, 7200)}${gsw("wakeup", "budgetStretch", "Stretch intervals when the budget pace overshoots")}</div>
      <small class="muted">The game polls every 10 s; the HQ is only consulted at the scheduled time or when a trigger fires. With dynamic off the interval is always the default. Budget stretch multiplies the interval by projected/limit (max x3) when the OpenRouter pace estimate projects overshooting the key limit.</small>
      <h3 class="cfgh">Triggers</h3>
      ${gsw("triggers", "enabled", "Wake the HQ early on events")}
      <div class="pair">${gsw("triggers", "newContact", "New contact (more than 150 m from every known one)")}${gsw("triggers", "squadLost", "Squad lost (0 alive or gone)")}</div>
      <div class="pair">${gsw("triggers", "objectiveReached", "Objective reached (within 50 m, or squad idle)")}${gsw("triggers", "squadChange", "Squad appeared / player-led changed")}</div>
      <div class="pair">${gnum("triggers", "contactNearSquadM", "Contact near a squad (m, 0 = off)", 0, 5000)}${gnum("triggers", "casualties", "Casualties since last call (agents, 0 = off)", 0, 1000)}</div>
      <div class="pair">${gnum("triggers", "strengthDropPct", "Squad strength drop (% of start, 0 = off)", 0, 100)}${gnum("triggers", "cooldownSec", "Cooldown between trigger calls (s)", 0, 600)}</div>
      <div class="pair">${gsw("triggers", "objectiveChanged", "Objective changed owner (v2)")}${gsw("triggers", "unitSpawned", "New unit spawned (v2)")}</div>
      <div class="pair">${gsw("triggers", "missionEvent", "Mission offered / accepted / done (v2)")}${gnum("triggers", "ticketsDropPct", "Own tickets dropped since last call (%, 0 = off, v2)", 0, 100)}</div>
      <small class="muted">Compared with the situation at the last LLM call. Triggered calls also respect the min interval above. Use "wake now" in the status strip to force a call.</small>
      ${f.provider === "openrouter" && html`<label class="switch"><input type="checkbox" checked=${f.stopAtLimit} onChange=${(e) => set("stopAtLimit", e.target.checked)} />
        <span>Stop provider calls at the key's spending limit <span class="muted">(the game then runs on its rule commander; LLM resumes automatically when budget is available again)</span></span></label>`}
      <label class="switch"><input type="checkbox" checked=${f.enabled} onChange=${(e) => set("enabled", e.target.checked)} />
        <span>Enabled <span class="muted">(off: no LLM calls, the game runs on its rule commander; the HQ engine selector above is the main switch)</span></span></label>
      <div class="btns"><button class="btn" type="submit" disabled=${busy}><${Icon} n="save" /> Save</button>
        <button class="btn ghost" type="button" disabled=${busy} onClick=${doTest}><${Icon} n="plug" /> Test connection</button></div>
      ${test && html`<pre>${test.pending ? "testing with the form values (nothing is saved)..." : J(test)}</pre>`}
    </form></section>
    <${MapCard} toast=${toast} />
    </div></main>`;
}

// ---------- OpenRouter usage card ----------
const usd = (v) => (num(v) ? "$" + (Math.abs(v) < 10 ? v.toFixed(2) : v.toFixed(1)) : "-");
const usd4 = (v) => (num(v) ? "$" + v.toFixed(4) : "-");
const PERIOD = { daily: ["usage_daily", "Today"], weekly: ["usage_weekly", "This week"], monthly: ["usage_monthly", "This month"] };
function UsageCard({ u, now, toast, refresh }) {
  const [busy, setBusy] = useState(false);
  if (!u?.enabled) return null;
  const k = u.key, d = k?.data, s = u.session ?? {}, b = u.budget;
  const doRefresh = async () => {
    setBusy(true);
    try { const { ok, j } = await postJson("/api/usage/refresh", {}); toast(ok && j.ok ? "Usage refreshed" : "Refresh failed: " + (j.usage?.key?.error ?? j.error ?? "request failed"), ok && j.ok ? "ok" : "err"); refresh(); }
    catch (err) { toast("Error: " + err.message, "err"); }
    setBusy(false);
  };
  // bar: period usage vs limit, or all-time usage vs limit
  let bar = null, numbers = null;
  if (d && num(d.limit) && d.limit > 0) {
    const per = PERIOD[d.limit_reset], used = per ? d[per[0]] : d.usage;
    const est = (used ?? 0) + (k.costSincePoll ?? 0), pct = Math.min(100, (100 * est) / d.limit);
    bar = { pct, label: `${per ? per[1] : "Total"} ${usd(est)} / ${usd(d.limit)} (${per ? "resets " + d.limit_reset : "no reset"})` };
  } else if (d) {
    numbers = [["Today", d.usage_daily], ["Week", d.usage_weekly], ["Month", d.usage_monthly], ["Total", d.usage]];
  }
  const pc = u.pace, dur = (ms) => { const m = Math.max(0, Math.round(ms / 60000)), dd = Math.floor(m / 1440), hh = Math.floor((m % 1440) / 60); return dd ? `${dd}d ${hh}h` : hh ? `${hh}h ${m % 60}m` : `${m}m`; };
  let chip = null, mark = null;
  if (pc?.kind === "period" && d) {
    mark = Math.min(100, (100 * pc.expected) / d.limit);
    const rs = ` · resets in ${dur(pc.resetsInMs)}`;
    if (pc.projected === null) chip = { cls: "", txt: "not enough data yet" + rs };
    else {
      const r = pc.projected / d.limit, cls = r > 1 ? "bad" : r >= 0.9 ? "warn" : "ok";
      chip = { cls, txt: pc.delta > 0 ? `▲ +${usd(pc.delta)} over by reset (proj. ${usd(pc.projected)} of ${usd(d.limit)})${rs}` : `▼ ${usd(-pc.delta)} under (proj. ${usd(pc.projected)} of ${usd(d.limit)})${rs}` };
    }
  } else if (pc?.kind === "rate") {
    chip = { cls: "", txt: pc.ratePerHour === null ? "not enough data yet" : `${usd(pc.ratePerHour)}/h${pc.hoursToLimit ? `, limit reached in ~${pc.hoursToLimit < 10 ? pc.hoursToLimit.toFixed(1) : Math.round(pc.hoursToLimit)} h` : ""}` };
  }
  const col = !bar ? "" : bar.pct >= 95 ? "var(--bad)" : bar.pct >= 80 ? "var(--warn)" : "var(--ok)";
  const models = Object.entries(s.byModel ?? {}).sort((a, c) => c[1].cost - a[1].cost);
  const facs = Object.entries(s.byFaction ?? {}).sort((a, c) => c[1].cost - a[1].cost);
  const polled = k?.ts ? `${Math.round(((now ?? Date.now()) - k.ts) / 1000)} s ago` : "never";
  return html`<section class="panel"><h2>Usage <span class="sub">OpenRouter${d?.label ? " key " + d.label : ""}</span></h2>
    <div class="cfgbody">
      ${b && b.level !== "ok" && html`<div class=${"banner " + (b.level === "warn" ? "warn" : "bad")}>${b.level === "reached"
        ? `Spending limit reached (${usd(b.limit)}).${u.stopAtLimit ? " Provider calls are stopped; the game uses its rule commander until budget is available again." : ""}`
        : `${b.pct.toFixed(0)}% of the key's spending limit used (${usd(b.remaining)} left).${b.pace?.delta > 0 ? ` At this pace you will overshoot by ${usd(b.pace.delta)} by reset.` : ""}`}</div>`}
      ${k?.error && html`<div class="banner bad">Budget poll failed: ${k.error}</div>`}
      ${!k?.keySet && html`<div class="muted">No API key stored: budget polling is off.</div>`}
      ${!bar && chip && html`<div class=${"upace " + chip.cls}>${chip.txt}</div>`}
      ${bar && html`<div><div class="ubar"><i style=${{ width: bar.pct + "%", background: col }}></i>${mark !== null && html`<span class="umark" style=${{ left: mark + "%" }} title="on pace marker"></span>`}</div><div class="ulabel">${bar.label}${chip && html` <span class=${"upace " + chip.cls}>${chip.txt}</span>`}${num(d.limit_remaining) ? html` <span class="muted">, ${usd(d.limit_remaining - (k.costSincePoll ?? 0))} left</span>` : ""}</div></div>`}
      ${numbers && html`<div><div class="unums">${numbers.map(([l, v]) => html`<div><span class="muted">${l}</span><b>${usd(v)}</b></div>`)}</div><div class="muted">no spending limit set</div></div>`}
      <div class="unums">
        <div><span class="muted">Session cost</span><b>${usd4(s.cost)}</b></div>
        <div><span class="muted">Requests</span><b>${s.requests ?? 0}</b></div>
        <div><span class="muted">Tokens in / out</span><b>${s.tokensIn ?? 0} / ${s.tokensOut ?? 0}</b></div></div>
      ${models.length > 0 && html`<table><thead><tr><th>Model</th><th>Requests</th><th>Cost</th></tr></thead><tbody>${models.map(([m, e]) => html`<tr><td>${m}</td><td>${e.requests}</td><td class="mono">${usd4(e.cost)}</td></tr>`)}</tbody></table>`}
      ${facs.length > 0 && html`<table><thead><tr><th>Faction (unit)</th><th>Requests</th><th>Cost</th></tr></thead><tbody>${facs.map(([m, e]) => html`<tr><td>${m}</td><td>${e.requests}</td><td class="mono">${usd4(e.cost)}</td></tr>`)}</tbody></table>`}
      <div class="btns"><button class="btn ghost" type="button" disabled=${busy || !k?.keySet} onClick=${doRefresh}><${Icon} n="reset" /> Refresh</button>
        <span class="muted" style=${{ alignSelf: "center" }}>last poll ${polled}; session figures reset when the sidecar restarts</span></div>
      <small class="muted">Use a dedicated OpenRouter key with a hard spending limit.</small>
    </div></section>`;
}

// ---------- app ----------
function App() {
  const [showInactive, setShowInactive] = useState(loadInactive);
  const [st, refresh] = useState2(showInactive);
  const [tab, setTab] = useState(loadTab);
  const [faction, setFaction] = useState("all");
  const [sel, setSel] = useState(null);
  const [act, setAct] = useState("all");
  const [layers, setLayers] = useState(loadLayers);
  const [toasts, toast] = useToasts();
  const world = useWorld();
  useEffect(() => { const h = () => { const t = tabOf(location.hash); if (t) setTab(t); }; window.addEventListener("hashchange", h); return () => window.removeEventListener("hashchange", h); }, []);
  useEffect(() => { try { localStorage.setItem(TAB_KEY, tab); } catch { /* storage unavailable */ } }, [tab]);
  useEffect(() => { try { localStorage.setItem(INACT_KEY, showInactive ? "1" : "0"); } catch { /* storage unavailable */ } }, [showInactive]);
  useEffect(() => { try { localStorage.setItem(LAYER_KEY, JSON.stringify(layers)); } catch { /* storage unavailable */ } }, [layers]);
  const names = sortedKeys(st.factions);
  const fac = faction === "all" || names.includes(faction) ? faction : "all"; // a selected faction that became inactive falls back to all
  const rows = useMemo(() => buildRows(st, fac), [st, fac]);
  const go = (id) => (e) => { e.preventDefault(); setTab(id); try { history.replaceState(null, "", "#" + id); } catch { location.hash = id; } };
  const common = { st, faction: fac, rows, sel, onSel: setSel, act, setAct };
  return html`<div>
    <header class="topbar">
      <div class="brand"><span class="logo"><${Icon} n="logo" /></span>Clausewitz HQ</div>
      <nav>${TABS.map(([id, label]) => html`<a key=${id} href=${"#" + id} class=${tab === id ? "active" : ""} onClick=${go(id)}>${label}</a>`)}</nav>
      <${Strip} st=${st} toast=${toast} refresh=${refresh} />
    </header>
    ${tab !== "usage" && tab !== "debug" && tab !== "actions" && html`<div class="subwrap"><${FactionBar} names=${names} faction=${fac} setFaction=${setFaction} hidden=${st.hidden ?? []} showInactive=${showInactive} setShowInactive=${setShowInactive} /></div>`}
    ${tab === "overview" && html`<${Overview} ...${common} />`}
    ${tab === "map" && html`<${MapView} ...${common} layers=${layers} setLayers=${setLayers} world=${world} />`}
    ${tab === "forces" && html`<${ForcesView} ...${common} />`}
    ${tab === "hq" && html`<${HqView} ...${common} />`}
    ${tab === "missions" && html`<${MissionsView} ...${common} layers=${layers} world=${world} />`}
    ${tab === "actions" && html`<${ActionsView} st=${st} />`}
    ${tab === "debug" && html`<${DebugView} st=${st} />`}
    ${tab === "usage" && html`<${Config} st=${st} toast=${toast} refresh=${refresh} />`}
    <div class="toasts">${toasts.map((t) => html`<div class=${"toast " + t.kind} key=${t.id}>${t.text}</div>`)}</div>
  </div>`;
}
render(html`<${App} />`, document.getElementById("app"));
