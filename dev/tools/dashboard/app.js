// Clausewitz HQ dashboard. Preact + htm (vendored, offline). All game text is rendered through Preact
// (auto-escaped); no dangerouslySetInnerHTML anywhere.
import { html, render, useState, useEffect, useRef, useMemo, useCallback } from "/vendor/standalone.module.js";
import { JournalView, MemorySettings } from "/journal.js"; // HQ war journal view + memory settings (WP5, dashboard/journal.js)
import { LoadoutsView, AccountCard } from "/loadouts.js"; // Loadouts tab + admin account card (dashboard/loadouts.js)
import { t, tk, has, LANGS, getLang, setLang, onLang, fmtClock } from "/i18n.js"; // EN/DE dictionaries (dashboard/i18n/*.json); locals named t / tk below were renamed

// every API call needs the login session: a 401 (expired / cleared cookie, restarted sidecar with a new password) goes back to the login page
const rawFetch = window.fetch.bind(window);
window.fetch = async (...args) => {
  const r = await rawFetch(...args);
  if (r.status === 401 && !String(args[0]).startsWith("/api/login")) location.href = "/login";
  return r;
};

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
const humanKey = (k) => (has("param." + k) ? t("param." + k) : String(k).replace(/([A-Z])/g, " $1").toLowerCase().replace(/^./, (c) => c.toUpperCase()));
const fmtVal = (v) => (typeof v === "boolean" ? (v ? t("val.yes") : t("val.no")) : v !== null && typeof v === "object" ? JSON.stringify(v) : v === undefined || v === null || v === "" ? "-" : String(v));

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
  useEffect(() => { poll(); const iv = setInterval(poll, 2000); return () => clearInterval(iv); }, [poll]);
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
      const move = speed === null ? "-" : moving ? `${speed.toFixed(1)} m/s${heading !== null ? " " + Math.round(heading) + "°" : ""}${derived ? " ~" : ""}` : t("squad.stationary");
      const state_ = s.state ? String(s.state) : s.agents === 0 ? "dead" : moving ? "moving" : s.mode === "ATTACK" ? "engaged" : s.mode === "DEFEND" ? "defending" : "idle";
      const ots = e.orderTs?.[s.id];
      rows.push({
        key: f + "/" + s.id, id: String(s.id), name: typeof s.name === "string" && s.name ? s.name : "", faction: f, type: dash(s.type), agents: s.agents, size: num(s.agents) ? `${s.agents}/${s.startAgents}` : "-",
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
      toast(ok ? t("wake.ok", name) : t("wake.failed", j.error ?? t("err.request")), ok ? "ok" : "err");
      refresh();
    } catch (e) { toast(t("err.generic", e.message), "err"); }
  };
  const trig = s.lastWakeTriggers?.length ? ` (${s.lastWakeTriggers.join("; ")})` : "";
  const tip = [
    t("wake.tipInterval", dash(s.intervalSec), dash(s.chosenSec), s.chosenReason ? ": " + s.chosenReason : "", (s.stretch ?? 1).toFixed(2)),
    t("wake.tipLast", s.lastWakeReason ?? "-", trig),
    t("wake.tipGated", s.gated ?? 0, s.failed ? t("wake.tipFailed") : ""),
  ].join("\n");
  const idle = engine && engine !== "llm";
  const txt = idle ? t("wake.rules") : paused ? t("wake.paused") : s.manual ? t("wake.pending") : left > 0 ? t("wake.in", left) : t("wake.due");
  return html`<div class=${"chip wake" + (s.failed && !idle && !paused ? " bad" : "")} title=${tip}><b class=${facClass(name)}>${name}</b> ${t("wake.next")} <b>${txt}</b>
    <button class="mini" type="button" disabled=${idle || paused} onClick=${wake} title=${t("wake.btnTip")}>${t("wake.btn")}</button></div>`;
}

// Global HQ controls: pause toggle (runtime) and engine selector (persisted config).
function HqControls({ st, toast, refresh }) {
  const engine = st.config?.engine ?? "llm", paused = !!st.paused;
  const togglePause = async () => {
    try {
      const { ok, j } = await postJson("/api/pause", { paused: !paused });
      if (!ok) toast(t("hq.pauseFailed", j.error ?? t("err.request")), "err");
      refresh();
    } catch (e) { toast(t("err.generic", e.message), "err"); }
  };
  const setEngine = async (e) => {
    try {
      const { ok, j } = await postJson("/api/config", { engine: e.target.value });
      if (!ok) toast(t("hq.engineFailed", j.error ?? t("err.request")), "err");
      refresh();
    } catch (err) { toast(t("err.generic", err.message), "err"); }
  };
  return html`<div class="chip hqctl">
    <button class=${"mini pausebtn" + (paused ? " on" : "")} type="button" onClick=${togglePause} title=${paused ? t("hq.resumeTip") : t("hq.pauseTip")}>${paused ? t("hq.resume") : t("hq.pause")}</button>
    <label class="engsel" title=${t("hq.engineTip")}>${t("hq.engine")}
      <select value=${engine} onChange=${setEngine}>
        <option value="llm">llm</option><option value="rules">rules</option><option value="model" disabled>${t("hq.modelSoon")}</option></select></label></div>`;
}

function Strip({ st, toast, refresh }) {
  const c = st.counters ?? {}, cf = st.config ?? {}, bud = st.usage?.budget;
  const age = st.lastRequestTs ? ((st.now ?? Date.now()) - st.lastRequestTs) / 1000 : null;
  const stale = st.offline || age === null || age * 1000 > STALE_MS;
  const ageTxt = st.offline ? t("strip.unreachable") : age === null ? t("strip.noRequest") : t("time.agoS", age.toFixed(0));
  const prov = cf.provider ? (cf.provider === "fake" ? "fake" : `${cf.provider} / ${cf.model}`) : "?";
  return html`<div class="strip">
    <div class="chip">${t("strip.provider")} <b>${prov}</b></div>
    <div class=${"chip" + (cf.enabled === false ? " bad" : "")}>${t("strip.enabled")} <b>${cf.enabled === false ? t("strip.no") : t("val.yes")}</b></div>
    <div class="chip">${t("strip.requests")} <b>${c.requests ?? 0}</b></div>
    <div class="chip" title=${t("strip.skippedTip")}>${t("strip.skipped")} <b>${c.skipped ?? 0}</b></div>
    ${cf.enabled !== false && Object.keys(st.scheduler ?? {}).sort(facCmp).map((n) => html`<${WakeChip} key=${n} name=${n} s=${st.scheduler[n]} recvAt=${st.recvAt} toast=${toast} refresh=${refresh} paused=${!!st.paused} engine=${cf.engine} />`)}
    <${HqControls} st=${st} toast=${toast} refresh=${refresh} />
    ${(st.paused || (cf.engine && cf.engine !== "llm")) && html`<div class="chip hqnote">${t("strip.hqEngine")} <b>${cf.engine && cf.engine !== "llm" ? cf.engine : "llm"}</b>${st.paused ? " " + t("strip.pausedParen") : ""}, ${t("strip.noLlm")}</div>`}
    ${(c.modelFallbacks?.strong || c.modelFallbacks?.weak) ? html`<div class="chip warn" title=${t("fb.chipTip", c.modelFallbacks.strong ?? 0, c.modelFallbacks.weak ?? 0)}>${t("fb.chip")} <b>${(c.modelFallbacks.strong ?? 0) + (c.modelFallbacks.weak ?? 0)}</b></div>` : ""}
    ${Object.keys(st.modelCooldowns ?? {}).length > 0 && html`<div class="chip warn" title=${t("fb.coolTip")}>${t("fb.cooling")} <b>${Object.entries(st.modelCooldowns).map(([k, s]) => k.split("|").slice(1).join("|") + " " + s + "s").join(", ")}</b></div>`}
    <div class=${"chip" + (c.errors ? " bad" : "")}>${t("strip.errors")} <b>${c.errors ?? 0}</b></div>
    <div class="chip">${t("strip.tokens")} <b>${c.inputTokens ?? 0} / ${c.outputTokens ?? 0}</b></div>
    ${bud && bud.level !== "ok" && html`<div class=${"chip " + (bud.level === "warn" ? "warn" : "bad")} title=${t("strip.budgetTip")}><span class="dot"></span>${bud.level === "reached" ? t("strip.budgetReached") : t("strip.budgetPct", bud.pct.toFixed(0))}</div>`}
    <div class=${"chip " + (stale ? "bad" : "ok")} title=${t("strip.staleTip")}><span class="dot"></span>${t("strip.lastReq")} <b>${ageTxt}</b></div>
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

  const draw = useCallback((ts) => {
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
    if (!v.fitted && !v.user) fit(); // refits every frame until the first squad/contact arrives (or the user pans/zooms), then once
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
        const blink = o.contested ? 0.5 + 0.5 * Math.sin(ts / 250) : 1;
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
        h.push({ x, y, r: Math.max(12, rad), title: t("map.objective", o.id + (o.name ? " " + o.name : "")), lines: [t("map.owner", o.owner ?? "?", o.contested ? " " + t("map.contestedCaps") : ""), t("map.valueRadius", dash(o.value), dash(o.radius)), t("map.progress", dash(o.progress)), t("map.at2", Math.round(o.pos[0]), Math.round(o.pos[1]))] });
      }
      for (const b of pic.bases) {
        const x = sx(b.pos[0]), y = sy(b.pos[1]), col = colorOf(b.faction);
        ctx.fillStyle = col; ctx.strokeStyle = "#0a0d12"; ctx.lineWidth = 2;
        ctx.beginPath(); ctx.rect(x - 8, y - 8, 16, 16); ctx.fill(); ctx.stroke();
        ctx.fillStyle = "#0a0d12"; ctx.font = "bold 10px system-ui, sans-serif"; ctx.textAlign = "center"; ctx.fillText("HQ", x, y + 1); ctx.textAlign = "start";
        if (layers.labels) { ctx.font = "bold 11px system-ui, sans-serif"; ctx.lineWidth = 3.5; ctx.lineJoin = "round"; ctx.strokeStyle = "rgba(8,11,16,.9)"; ctx.strokeText(t("map.baseLabel", b.faction), x + 12, y); ctx.fillStyle = col; ctx.fillText(t("map.baseLabel", b.faction), x + 12, y); ctx.lineJoin = "miter"; ctx.font = "11px system-ui, sans-serif"; }
        h.push({ x, y, r: 12, title: t("map.mainBase", b.faction), lines: [t("map.at2", Math.round(b.pos[0]), Math.round(b.pos[1]))] });
      }
      // WP3: task arrows (dashed, unit -> destination): FOB builder / escort -> site, convoy truck -> destination base
      const taskArrow = (x0, y0, x1, y1, col) => {
        const a = Math.atan2(y1 - y0, x1 - x0);
        ctx.strokeStyle = col; ctx.fillStyle = col; ctx.lineWidth = 2; ctx.globalAlpha = .8; ctx.setLineDash([7, 4]);
        ctx.beginPath(); ctx.moveTo(x0, y0); ctx.lineTo(x1, y1); ctx.stroke(); ctx.setLineDash([]);
        ctx.beginPath(); ctx.moveTo(x1, y1); ctx.lineTo(x1 - 11 * Math.cos(a - .4), y1 - 11 * Math.sin(a - .4)); ctx.lineTo(x1 - 11 * Math.cos(a + .4), y1 - 11 * Math.sin(a + .4)); ctx.closePath(); ctx.fill(); ctx.globalAlpha = 1;
      };
      const destOf = (fe, baseId) => { // world [x,z] of a base id of a faction: HQ (situation.bases) or one of its FOBs
        const sit0 = fe?.situation ?? {};
        const fb = (Array.isArray(sit0.fobs) ? sit0.fobs : []).find((f0) => f0 && f0.id === baseId);
        if (fb && Array.isArray(fb.pos)) return fb.pos;
        const hq = (Array.isArray(sit0.bases) ? sit0.bases : []).find((b0) => b0 && b0.faction === sit0.faction && Array.isArray(b0.pos));
        return baseId === "HQ" && hq ? hq.pos : null;
      };
      const nameOfUnit = (fe, id) => { const q = (Array.isArray(fe?.situation?.squads) ? fe.situation.squads : []).find((s0) => s0 && s0.id === id); return q?.name || id; };
      // Convoys (M3 phase L): supply trucks between bases, a small square with the cargo; hollow while loading, red when lost
      for (const [fk, fe] of Object.entries(st.factions ?? {})) for (const c of Array.isArray(fe.situation?.convoys) ? fe.situation.convoys : []) {
        if (!c || !Array.isArray(c.pos) || !num(c.pos[0]) || !num(c.pos[1]) || c.state === "failed") continue;
        const x = sx(c.pos[0]), y = sy(c.pos[1]), col = c.state === "lost" ? "#ff5a4d" : colorOf(fk), moving = c.state === "en_route";
        ctx.strokeStyle = col; ctx.fillStyle = col; ctx.lineWidth = 2; ctx.beginPath(); ctx.rect(x - 5, y - 5, 10, 10); if (moving) ctx.fill(); ctx.stroke();
        if (layers.labels) { const txt = `${c.id} ${num(c.cargo) ? c.cargo : 0}`; ctx.font = "bold 10px system-ui, sans-serif"; ctx.lineWidth = 3; ctx.lineJoin = "round"; ctx.strokeStyle = "rgba(8,11,16,.9)"; ctx.strokeText(txt, x + 9, y + 3); ctx.fillStyle = col; ctx.fillText(txt, x + 9, y + 3); ctx.lineJoin = "miter"; ctx.font = "11px system-ui, sans-serif"; }
        const cd = c.state === "lost" ? null : destOf(fe, c.to);
        if (cd && num(cd[0]) && num(cd[1]) && (c.state === "en_route" || c.state === "loading")) taskArrow(x, y, sx(cd[0]), sy(cd[1]), col);
        h.push({ x, y, r: 10, title: t("map.convoy", fk, c.id), lines: [`${c.from} -> ${c.to}, ${c.state}`, t("map.cargo", dash(c.cargo))] });
      }
      // FOBs (M3 phase F): each faction's own situation lists its FOBs; a diamond, hollow while planned / moving / building
      for (const [fk, fe] of Object.entries(st.factions ?? {})) for (const f of Array.isArray(fe.situation?.fobs) ? fe.situation.fobs : []) {
        if (!f || !Array.isArray(f.pos) || !num(f.pos[0]) || !num(f.pos[1]) || f.state === "lost") continue;
        const x = sx(f.pos[0]), y = sy(f.pos[1]), col = colorOf(fk), up = f.state === "active";
        // base icon like the HQ (square with text): filled when standing, hollow while planned / moving / building (never an objective)
        ctx.globalAlpha = f.underAttack ? 0.5 + 0.5 * Math.sin(ts / 200) : 1; ctx.strokeStyle = col; ctx.fillStyle = up ? col : "rgba(8,11,16,.85)"; ctx.lineWidth = 2.5;
        ctx.beginPath(); ctx.rect(x - 9, y - 9, 18, 18); ctx.fill(); ctx.stroke();
        ctx.fillStyle = up ? "#0a0d12" : col; ctx.font = "bold 9px system-ui, sans-serif"; ctx.textAlign = "center"; ctx.fillText("FOB", x, y + 1); ctx.textAlign = "start"; ctx.globalAlpha = 1;
        const fobTasks = Array.isArray(fe.situation?.fobTasks) ? fe.situation.fobTasks.filter((ft) => ft && ft.fob === f.id && Array.isArray(ft.pos)) : [];
        if (!up) for (const ft of fobTasks) taskArrow(sx(ft.pos[0]), sy(ft.pos[1]), x, y, col);
        if (!up && !fobTasks.length && f.builder) { const bq = (Array.isArray(fe.situation?.squads) ? fe.situation.squads : []).find((s0) => s0 && s0.id === f.builder && okPos(s0.pos)); if (bq) taskArrow(sx(bq.pos[0]), sy(bq.pos[2]), x, y, col); }
        if (layers.labels) { const txt = `${f.id}${up ? "" : " " + (f.state === "building" ? t("map.fobBuilding", Math.round(f.build ?? 0)) : t("map.fobPlanned"))}${num(f.stock) ? " [" + Math.round(f.stock) + "]" : ""}`; ctx.font = "bold 11px system-ui, sans-serif"; ctx.lineWidth = 3.5; ctx.lineJoin = "round"; ctx.strokeStyle = "rgba(8,11,16,.9)"; ctx.strokeText(txt, x + 14, y); ctx.fillStyle = col; ctx.fillText(txt, x + 14, y); ctx.lineJoin = "miter"; ctx.font = "11px system-ui, sans-serif"; }
        h.push({ x, y, r: 12, title: t("map.fobTitle", fk, f.id + (f.name ? " " + f.name : "")), lines: [...(num(f.stock) ? [t("map.fobStock", Math.round(f.stock))] : []), t("map.fobState", f.state, f.state === "building" ? " " + Math.round(f.build ?? 0) + "%" : "", dash(f.tier), f.underAttack ? " (" + t("map.underAttack") + ")" : ""), t("map.services", Array.isArray(f.services) && f.services.length ? f.services.join(", ") : "-"), t("map.at2", Math.round(f.pos[0]), Math.round(f.pos[1]))] });
      }
    }
    if (layers.structures) { // base / FOB structures (arsenal, depot, ...): situation.structures[] of each faction, a small lettered square
      const GLYPH = { command_post: "C", arsenal: "A", vehicle_depot: "V", supply: "S", repair: "R" };
      for (const [fk, fe] of Object.entries(st.factions ?? {})) {
        if (faction !== "all" && fk !== faction) continue;
        for (const s of Array.isArray(fe.situation?.structures) ? fe.situation.structures : []) {
          if (!s || !Array.isArray(s.pos) || !num(s.pos[0]) || !num(s.pos[1])) continue;
          const x = sx(s.pos[0]), y = sy(s.pos[1]), col = colorOf(fk);
          if (x < -50 || x > r.width + 50 || y < -50 || y > r.height + 50) continue;
          ctx.fillStyle = "rgba(8,11,16,.85)"; ctx.strokeStyle = col; ctx.lineWidth = 2;
          ctx.beginPath(); ctx.rect(x - 6, y - 6, 12, 12); ctx.fill(); ctx.stroke();
          ctx.fillStyle = col; ctx.font = "bold 9px system-ui, sans-serif"; ctx.textAlign = "center"; ctx.fillText(GLYPH[s.kind] ?? "?", x, y + 3); ctx.textAlign = "start";
          if (layers.labels && v.s > 0.1) { const txt = String(s.label ?? s.kind); ctx.font = "10px system-ui, sans-serif"; ctx.lineWidth = 3; ctx.lineJoin = "round"; ctx.strokeStyle = "rgba(8,11,16,.9)"; ctx.strokeText(txt, x + 9, y + 3); ctx.fillStyle = col; ctx.fillText(txt, x + 9, y + 3); ctx.lineJoin = "miter"; ctx.font = "11px system-ui, sans-serif"; }
          h.push({ x, y, r: 9, title: `${fk} ${s.label ?? s.kind}`, lines: [t("map.structAt", dash(s.kind), dash(s.base)), t("map.grid", dash(s.grid)), t("map.at2", Math.round(s.pos[0]), Math.round(s.pos[1]))] });
        }
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
        h.push({ x, y, r: 10, title: t("map.contact", f), lines: [String(ct.desc ?? "?"), t("map.age", age), t("map.at1", gridTxt(ct.pos))] });
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
        h.push({ x: x1, y: y1, r: 8, title: t("map.orderTitle", o.command, o.target ? " -> " + o.target : ""), lines: [String(o.squad), String(o.reason ?? "")] });
      }
      if (layers.squads) for (const s of squads) {
        const x = sx(s.pos[0]), y = sy(s.pos[2]), key = f + "/" + s.id, col = colorOf(f), dm = s.dormant === true;
        if (s.humanLed) {
          const ph = ((ts / 1200) % 1);
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
          ctx.font = "600 11px system-ui, sans-serif"; ctx.fillText((s.name || shortId(s.id)) + (s.humanLed ? " ★" : ""), x + 9, y - 11);
          if (s.kind || veh) { ctx.font = "9px system-ui, sans-serif"; ctx.fillStyle = dm ? "rgba(221,227,236,.45)" : "rgba(221,227,236,.7)"; ctx.fillText([kindLabel(s.kind), veh?.type, veh ? (veh.mounted ? t("veh.mounted") : t("veh.dismounted")) : null].filter(Boolean).join(" "), x + 9, y + 13); }
          ctx.font = "11px system-ui, sans-serif";
        }
        const oc = s.objective?.command ?? (byOrder.get(s.id)?.command ?? "-"), ot = s.objective?.target ?? byOrder.get(s.id)?.target;
        h.push({ x, y, r: 11, key, title: s.name ? `${s.name} (${s.id})` : String(s.id), lines: [t("map.sqL1", f, s.agents, s.startAgents, s.mode ?? s.state ?? "-"), t("map.sqActivity", typeof s.dormant !== "boolean" ? t("act.unknown") : dm ? t("act.dormantLong") : t("act.activeLong")), ...(s.kind ? [t("map.kind", s.kind, s.type ? ": " + s.type : "")] : []),
          ...(veh ? [t("map.vehicle", dash(veh.type), veh.mounted ? t("veh.mounted") : t("veh.dismounted"), veh.state ? ", " + veh.state : "", num(veh.health) ? ", " + t("unit.hp", Math.round(veh.health * 100)) : "")] : []),
          t("map.objectiveLine", oc + (ot ? " " + ot : "")), s.humanLed ? t("map.humanLed") + (s.mission?.state && s.mission.state !== "none" ? t("map.missionSuffix", `${s.mission.state} ${s.mission.kind ?? ""} ${s.mission.target ?? ""}`) : "") : t("map.aiLed"), t("map.at1", gridTxt(s.pos))] });
      }
    }
    if (focus && Array.isArray(focus.pos)) { // focus marker (Missions): pulsing dashed ring + label
      const x = sx(focus.pos[0]), y = sy(focus.pos[1]), pr = 20 + 3 * Math.sin(ts / 220);
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
    const loop = (ts) => { draw(ts); raf = requestAnimationFrame(loop); };
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

  // Input: Pointer Events (mouse, touch, pen) with pointer capture; touch events as fallback where PointerEvent is missing.
  // One pointer = pan (tap = select/tooltip), two pointers = pinch zoom around the pinch centre + pan, wheel = zoom (desktop),
  // double click = fit (mouse), double tap = zoom in x2 (touch). touch-action:none on the canvas (style.css) keeps the page from scrolling/zooming.
  const pts = useRef(new Map()), lastTap = useRef(null);
  latest.current.onSel = onSel;
  useEffect(() => {
    const c = cv.current, P = pts.current, v = view.current;
    const loc = (cx, cy) => { const r = c.getBoundingClientRect(); return [cx - r.left, cy - r.top]; };
    const zoomAt = (mx, my, q) => { const ns = Math.min(5, Math.max(0.02, v.s * q)), k = ns / v.s; v.ox = mx - (mx - v.ox) * k; v.oy = my - (my - v.oy) * k; v.s = ns; v.user = true; };
    const pair = () => { const [a, b] = [...P.values()]; return { d: Math.hypot(a.x - b.x, a.y - b.y) || 1, cx: (a.x + b.x) / 2, cy: (a.y + b.y) / 2 }; };
    const begin = (id, cx, cy, type) => {
      v.anim = null; P.set(id, { x: cx, y: cy, type });
      if (P.size === 1) { drag.current = { x: cx, y: cy, moved: 0, type }; c.classList.add("grabbing"); }
      else if (P.size === 2) { drag.current = drag.current ?? { x: cx, y: cy, moved: 0, type }; drag.current.moved = 1e6; drag.current.pinch = pair(); setTip(null); }
    };
    const move = (id, cx, cy, type) => {
      const p = P.get(id), d = drag.current;
      if (!p) { if (type === "mouse" && !P.size) { const [mx, my] = loc(cx, cy), h = pick(mx, my, false); setTip(h ? { x: mx, y: my, h } : null); } return; }
      if (P.size >= 2 && d?.pinch) {
        p.x = cx; p.y = cy;
        const n = pair(), o = d.pinch, [ox, oy] = loc(o.cx, o.cy), [nx, ny] = loc(n.cx, n.cy);
        zoomAt(ox, oy, n.d / o.d); v.ox += nx - ox; v.oy += ny - oy; d.pinch = n; setTip(null); return;
      }
      if (!d) return;
      d.moved += Math.abs(cx - d.x) + Math.abs(cy - d.y);
      v.user = true; v.ox += cx - d.x; v.oy += cy - d.y; d.x = cx; d.y = cy; p.x = cx; p.y = cy; setTip(null);
    };
    const end = (id, cx, cy, cancel) => {
      const p = P.get(id); if (!p) return;
      P.delete(id);
      const d = drag.current;
      if (P.size > 0) { // pinch ended with one finger left: continue as pan from its position, never as a tap
        const q = [...P.values()][0]; if (d) { d.x = q.x; d.y = q.y; d.moved = 1e6; d.pinch = null; } return;
      }
      drag.current = null; c.classList.remove("grabbing");
      if (cancel || !d || d.moved >= (p.type === "mouse" ? 4 : 10)) return;
      const [mx, my] = loc(cx, cy), h = pick(mx, my, true);
      latest.current.onSel?.(h ? h.key : null);
      if (p.type !== "mouse") {
        const now = performance.now(), lt = lastTap.current;
        if (lt && now - lt.t < 350 && Math.hypot(lt.x - mx, lt.y - my) < 30) { zoomAt(mx, my, 2); lastTap.current = null; }
        else { lastTap.current = { t: now, x: mx, y: my }; const hh = pick(mx, my, false); setTip(hh ? { x: mx, y: my, h: hh } : null); }
      }
    };
    const onWheel = (e) => { e.preventDefault(); v.anim = null; const [mx, my] = loc(e.clientX, e.clientY); zoomAt(mx, my, e.deltaY < 0 ? 1.2 : 1 / 1.2); };
    const L = [[c, "wheel", onWheel, { passive: false }]];
    if (window.PointerEvent) {
      L.push([c, "pointerdown", (e) => { try { c.setPointerCapture(e.pointerId); } catch {} begin(e.pointerId, e.clientX, e.clientY, e.pointerType || "mouse"); }, { passive: true }],
        [c, "pointermove", (e) => { if (P.has(e.pointerId)) e.preventDefault(); move(e.pointerId, e.clientX, e.clientY, e.pointerType || "mouse"); }, { passive: false }],
        [c, "pointerup", (e) => end(e.pointerId, e.clientX, e.clientY, false), { passive: true }],
        [c, "pointercancel", (e) => end(e.pointerId, e.clientX, e.clientY, true), { passive: true }],
        [c, "pointerleave", () => { if (!P.size) setTip(null); }, { passive: true }],
        [c, "dblclick", (e) => { if (e.pointerType !== "touch" && e.pointerType !== "pen") fit(); }, { passive: true }]);
    } else {
      const each = (e, f) => { for (const t of e.changedTouches) f(t); };
      L.push([c, "touchstart", (e) => { e.preventDefault(); each(e, (t) => begin(t.identifier, t.clientX, t.clientY, "touch")); }, { passive: false }],
        [c, "touchmove", (e) => { e.preventDefault(); each(e, (t) => move(t.identifier, t.clientX, t.clientY, "touch")); }, { passive: false }],
        [c, "touchend", (e) => { e.preventDefault(); each(e, (t) => end(t.identifier, t.clientX, t.clientY, false)); }, { passive: false }],
        [c, "touchcancel", (e) => each(e, (t) => end(t.identifier, t.clientX, t.clientY, true)), { passive: true }],
        [c, "mousedown", (e) => begin("m", e.clientX, e.clientY, "mouse"), { passive: true }],
        [c, "mousemove", (e) => move("m", e.clientX, e.clientY, "mouse"), { passive: true }],
        [c, "mouseup", (e) => end("m", e.clientX, e.clientY, false), { passive: true }],
        [c, "mouseleave", () => { setTip(null); P.delete("m"); drag.current = null; c.classList.remove("grabbing"); }, { passive: true }],
        [c, "dblclick", () => fit(), { passive: true }]);
    }
    for (const [el, n, f, o] of L) el.addEventListener(n, f, o);
    return () => { for (const [el, n, f, o] of L) el.removeEventListener(n, f, o); P.clear(); drag.current = null; };
  }, []);

  return html`<section class=${"panel" + (tall ? " tall" : "") + (compact ? " compact" : "")}>
    <h2>${compact ? t("map.missionMap") : t("map.title")} <span class="sub">${t("map.sub", Math.round(ext.x1 - ext.x0), Math.round(ext.z1 - ext.z0))}${world.map?.status === "resolving" ? t("map.loadingImg") : ""}</span></h2>
    <div class="mapbox" ref=${box}>
      <canvas ref=${cv}></canvas>
      <button class="mapbtn" onClick=${() => fit()} title=${t("map.resetView")}><${Icon} n="reset" /> ${t("map.fit")}</button>
      <div class="maphint">${t("map.hint")}</div>
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
  const opt = [["all", t("common.all"), rows.length], ["active", t("act.active"), ac], ["dormant", t("act.dormant"), dm]];
  return html`<div class="seg actf" title=${known ? t("act.filterTip") : t("act.filterNoData")}>
    ${opt.map(([k, l, n]) => html`<button key=${k} class=${act === k ? "on" : ""} disabled=${!known && k !== "all"} onClick=${() => setAct(k)}>${l} <span class="muted">${n}</span></button>`)}</div>`;
}
// key: strong swatch = active, pale swatch = dormant, per faction
function ActLegend({ names, rows }) {
  if (!rows.some((r) => r.dormant !== null)) return null;
  return html`<div class="actleg">${names.map((f) => html`<span key=${f}><i class="sw" style=${{ background: colorOf(f) }}></i><i class="sw" style=${{ background: tint(colorOf(f), DORM_A), boxShadow: `inset 0 0 0 1px ${tint(colorOf(f), 0.85)}` }}></i> <b class=${facClass(f)}>${f}</b></span>`)}
    <span class="muted">${t("act.legend")}</span></div>`;
}

// ---------- squads table ----------
const COLS = [["id", "col.squad"], ["faction", "col.faction"], ["kind", "col.kind"], ["strength", "col.strength"], ["objective", "col.objective"], ["state", "col.state"], ["position", "col.position"]];
const SORTKEY = { id: (r) => r.id, faction: (r) => r.faction, kind: (r) => r.kind, strength: (r) => r.pct ?? -1,
  objective: (r) => r.obj?.command ?? "", state: (r) => r.state, position: (r) => (okPos(r.pos) ? r.pos[0] : -1) };
const stateBadge = (s) => ({ dead: "bad", engaged: "bad", moving: "info", defending: "ok" }[s] ?? "");
const DormBadge = ({ r }) => (r.dormant === true ? html`<span class="badge dorm" title=${t("act.dormantTip")}>${t("act.dormant")}</span>` : "");

// Compact table; the secondary fields (type, vehicle, wounded, movement, leader, order age) open as a detail row on the selected squad.
function SquadsTable({ rows, sel, onSel }) {
  const [sort, setSort] = useState({ key: "faction", dir: 1 });
  const sorted = useMemo(() => {
    const f = SORTKEY[sort.key] ?? SORTKEY.id;
    return [...rows].sort((a, b) => (f(a) > f(b) ? 1 : f(a) < f(b) ? -1 : a.id > b.id ? 1 : -1) * sort.dir);
  }, [rows, sort]);
  useEffect(() => { if (sel) document.querySelector(`tr[data-key="${CSS.escape(sel)}"]`)?.scrollIntoView({ block: "nearest" }); }, [sel]);
  return html`<section class="panel squads">
    <h2>${t("squads.title")} <span class="sub">${t("squads.sub", rows.length)}</span></h2>
    <div class="scroll"><table>
      <thead><tr>${COLS.map(([k, lbl]) => html`<th class="sortable" onClick=${() => setSort((s) => ({ key: k, dir: s.key === k ? -s.dir : 1 }))}>${t(lbl)}${sort.key === k ? (sort.dir > 0 ? " ▲" : " ▼") : ""}</th>`)}</tr></thead>
      <tbody>${sorted.map((r) => {
        const col = r.pct === null ? "#5d697c" : r.pct >= 66 ? "var(--ok)" : r.pct >= 33 ? "var(--warn)" : "var(--bad)", o = r.obj, on = sel === r.key;
        const main = html`<tr key=${r.key} data-key=${r.key} class=${"sqrow" + (on ? " sel" : "") + (r.dormant === true ? " dormrow" : "")} onClick=${() => onSel(on ? null : r.key)}>
          <td style=${{ borderLeft: "3px solid " + colorOf(r.faction) }} title=${r.id}><b>${r.name || r.id}</b>${r.name ? html` <span class="muted mono">${r.id}</span>` : ""}${r.human ? html` <span class="badge human">${t("badge.human")}</span>` : ""}</td><td class=${facClass(r.faction)}>${r.faction}</td>
          <td>${r.kind ? html`<span class=${"badge kind k-" + r.kind}>${kindLabel(r.kind)}</span>` : "-"}</td>
          <td>${r.pct === null ? "-" : html`<span class="bar"><i style=${{ width: Math.min(100, r.pct) + "%", background: col }}></i></span><span class="mono">${r.size}</span> <span class="muted">${r.pct}%</span>`}</td>
          <td class="wrap">${o ? html`<span class=${"cmd " + o.command}>${o.command}</span> ${o.target ? html`<b>${o.target}</b>` : ""}${!o.target && Array.isArray(o.pos) && num(o.pos[0]) ? html`<span class="mono">${Math.round(o.pos[0])}, ${Math.round(o.pos[1])}</span>` : ""}${o.status ? html` <span class="muted">${o.status}</span>` : ""}` : "-"}</td>
          <td><span class=${"badge " + stateBadge(r.state)}>${tk("state", r.state)}</span> <${DormBadge} r=${r} /></td><td class="mono">${r.posTxt}</td></tr>`;
        if (!on) return main;
        return [main, html`<tr key=${r.key + "#d"} class="detail"><td colspan=${COLS.length}><dl class="kv">
          <div><dt>${t("det.type")}</dt><dd>${r.type}</dd></div>
          <div><dt>${t("det.vehicle")}</dt><dd>${r.vehicle ? html`${dash(r.vehicle.type)} <span class=${"badge " + (r.vehicle.mounted ? "info" : "warn")}>${r.vehicle.mounted ? t("veh.mounted") : t("veh.dismounted")}</span>${r.vehicle.state ? html` <span class="muted">${r.vehicle.state}</span>` : ""}${num(r.vehicle.health) ? html` <span class="muted">${t("unit.hp", Math.round(r.vehicle.health * 100))}</span>` : ""}` : "-"}</dd></div>
          <div><dt>${t("det.wounded")}</dt><dd>${r.wounded === null ? "-" : r.wounded}${r.health !== null ? html` <span class="muted">${t("unit.avgHp", Math.round(r.health * 100))}</span>` : ""}</dd></div>
          <div><dt>${t("det.movement")}</dt><dd class="mono">${r.move}</dd></div>
          <div><dt>${t("det.leader")}</dt><dd>${r.human ? html`${t("det.leaderHuman")}${r.mission ? html` <span class="badge warn">${r.mission.state} ${r.mission.kind ?? ""} ${r.mission.target ?? ""}</span>` : ""}` : t("det.leaderAi")}</dd></div>
          <div><dt>${t("det.activity")}</dt><dd>${r.dormant === true ? t("act.dormantShort") : r.dormant === false ? t("act.activeLong") : "-"}</dd></div>
          <div><dt>${t("det.lastOrder")}</dt><dd>${r.orderAge === null ? "-" : t("time.agoS", r.orderAge)}</dd></div>
          <div class="wide"><dt>${t("col.reason")}</dt><dd class="muted">${dash(r.reason)}</dd></div></dl></td></tr>`];
      })}</tbody></table>
      ${!rows.length && html`<div class="empty">${t("squads.empty")}</div>`}</div>
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
    { section: t("gm.rules") },
    ...keys.map((k) => ({ label: humanKey(k), tip: k, merge: true, cell: (f) => ({ txt: fmtVal(war(f)?.mode?.params?.[k]) }) })),
    { label: t("gm.timeLeft"), merge: true, cell: (f) => ({ txt: num(sit(f).timeLeftSec) && sit(f).timeLeftSec >= 0 ? fmtTime(sit(f).timeLeftSec) : t("gm.noLimit") }) },
    { section: t("gm.live") },
    { label: t("gm.tickets"), tip: t("gm.ticketsTip"), cell: (f) => { const tix = war(f)?.tickets; return num(tix) && tix >= 0 ? { txt: String(tix), v: tix } : { txt: t("gm.na"), dim: true }; } },
    { label: t("gm.supplies"), tip: t("gm.suppliesTip"), cell: (f) => { const w = war(f); return w ? { txt: html`${dash(w.supplies)}${num(w.income) ? html`<span class="muted"> ${t("gm.incomePerMin", w.income)}</span>` : ""}`, v: num(w.supplies) ? w.supplies : 0 } : { txt: "-" }; } },
    { label: t("gm.ai"), tip: t("gm.aiTip"), cell: (f) => cap(war(f)?.liveAI, war(f)?.aiCap) },
    { label: t("gm.vehicles"), tip: t("gm.vehiclesTip"), cell: (f) => cap(war(f)?.vehicles, war(f)?.vehicleCap) },
  ];
  return html`<section class="panel"><h2>${t("gm.title")} <span class="sub">${mode ? html`<b>${mode.name || mode.id}</b> ` : ""}${phase ? html`<span class=${"badge " + (phase === "running" ? "ok" : phase === "setup" ? "warn" : "info")}>${phase}</span>` : ""}</span></h2>
    <div class="scroll"><${Compare} names=${names} rows=${rows} /></div></section>`;
}

// ---------- per-faction SITREP (generated elsewhere: st.factions[f].sitrep = { text, sections:[{title, body}], generatedAt, source, warTimeSec }) ----------
function SitrepCard({ f, e, now }) {
  const [view, setView] = useState("en"); // per-card language toggle; en = canonical text as stored
  const s = e?.sitrep && typeof e.sitrep === "object" ? e.sitrep : null, secs = Array.isArray(s?.sections) ? s.sections.filter((x) => x && (x.title || x.body)) : [];
  const filled = s && (s.text || secs.length), age = num(s?.generatedAt) ? Math.max(0, Math.round(((now ?? Date.now()) - s.generatedAt) / 1000)) : null;
  const tbl = s?.text_by_lang && typeof s.text_by_lang === "object" ? s.text_by_lang : null;
  const langs = tbl ? ["en", ...Object.keys(tbl).filter((l) => l !== "en" && tbl[l] && typeof tbl[l] === "object")] : ["en"];
  const lg = langs.includes(view) ? view : "en", tr = lg !== "en" ? tbl[lg] : null;
  const head = tr ? tr.headline : s?.text, body = (x) => (tr ? tr[x.id] ?? x.body : x.body), title = (x) => (x.id && has("sitrep.sec." + x.id) ? tk("sitrep.sec", x.id) : x.title);
  return html`<section class="panel sitrep" style=${accent(f)}><h2><span class=${facClass(f)}>${f}</span> SITREP
    ${langs.length > 1 && html`<span class="seg langtog" role="group" aria-label=${t("sitrep.langToggle")} title=${t("sitrep.langToggle")}>${langs.map((l) => html`<button type="button" key=${l} class=${l === lg ? "on" : ""} onClick=${() => setView(l)}>${l.toUpperCase()}</button>`)}</span>`}
    ${filled && html`<span class="sub">${s.source ? html`<span class=${"badge " + (s.source === "llm" ? "info" : "")} title=${s.source === "llm" ? t("sitrep.byLlm") : t("sitrep.byRules")}>${s.source}</span> ` : ""}${num(s.warTimeSec) ? t("sitrep.warTime", fmtTime(s.warTimeSec)) + " " : ""}${age === null ? "" : age < 90 ? t("time.agoS", age) : t("time.agoMin", Math.round(age / 60))}${age !== null && age > 600 ? html` <span class="badge warn">${t("badge.stale")}</span>` : ""}</span>`}</h2>
    ${filled ? html`<div class="sitbody">${head && html`<p class="sittext">${head}</p>`}
      ${secs.length > 0 && html`<dl class="sitsecs">${secs.map((x, i) => html`<div key=${i}><dt>${dash(title(x))}</dt><dd>${body(x)}</dd></div>`)}</dl>`}</div>`
      : html`<div class="empty sitempty">${t("sitrep.empty")}</div>`}</section>`;
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
function ForcesCompare({ st, rows, names, full, title = null }) {
  if (!names.length) return null;
  const S = new Map(names.map((f) => [f, forceStats(st, rows, f)]));
  const g = (f) => S.get(f);
  const out = [
    { label: t("fc.squads"), tip: t("fc.squadsTip"), cell: (f) => ({ txt: String(g(f).mine.length), stack: g(f).known ? [[g(f).act, 1], [g(f).dorm, DORM_A]] : null, v: g(f).mine.length }) },
    ...(rows.some((r) => r.dormant !== null) ? [{ label: t("fc.activeDormant"), tip: t("fc.activeDormantTip"), cell: (f) => (g(f).known ? { txt: html`${g(f).act} <span class="muted">/</span> ${g(f).dorm}` } : { txt: "-", dim: true }) }] : []),
    { label: t("fc.soldiers"), tip: t("fc.soldiersTip"), cell: (f) => ({ txt: html`${g(f).alive}${g(f).start > g(f).alive ? html`<span class="muted"> / ${g(f).start}</span>` : ""}`, v: g(f).alive, max: g(f).start || 1 }) },
    { label: t("fc.vehicleUnits"), cell: (f) => ({ txt: String(g(f).vehicles) }) },
    { label: t("fc.humanLed"), cell: (f) => ({ txt: String(g(f).human) }) },
    { label: t("fc.contacts"), cell: (f) => ({ txt: String(g(f).contacts) }) },
    { label: t("fc.byKind"), cell: (f) => ({ txt: html`<span class="kinds">${g(f).kinds.size ? [...g(f).kinds.entries()].map(([k, n]) => html`<span class=${"badge kind k-" + k} key=${k}>${kindLabel(k)} ${n}</span>`) : "-"}</span>` }) },
    ...(full ? [
      { label: t("fc.engaged"), cell: (f) => ({ txt: String(g(f).engaged) }) },
      { label: t("fc.moving"), cell: (f) => ({ txt: String(g(f).moving) }) },
      { label: t("fc.wounded"), cell: (f) => ({ txt: String(g(f).wounded) }) },
    ] : []),
    { label: t("fc.updated"), cell: (f) => ({ txt: ago(st, st.factions[f]?.ts), dim: true }) },
  ];
  return html`<section class="panel"><h2>${title ?? t("fc.title")} <span class="sub">${t("fc.perSide")}</span></h2><div class="scroll"><${Compare} names=${names} rows=${out} /></div></section>`;
}

// ---------- objectives + recent events ----------
function ObjectivesPanel({ st }) {
  const { objs, viewer } = publicPicture(st);
  if (!objs.length) return null;
  // progress is signed from the viewer's side (+100 = viewer's, -100 = enemy's): show who it favours, in that faction's colour
  const other = FACTIONS.find((x) => x !== viewer);
  const prog = (o) => { if (!num(o.progress) || o.progress === 0) return html`<span class="dim">0 %</span>`; const to = o.progress > 0 ? viewer : other, c = to ? colorOf(to) : NEUTRAL;
    return html`<span class="pbar" title=${t("obj.toward", to ?? "?")}><i style=${{ width: Math.min(100, Math.abs(o.progress)) + "%", background: c }}></i></span><span class="mono">${Math.abs(o.progress)} %</span>`; };
  return html`<section class="panel side"><h2>${t("obj.title")} <span class="sub">${t("obj.sub", objs.length)}</span></h2><div class="scroll"><table>
    <thead><tr><th>${t("col.id")}</th><th>${t("col.name")}</th><th>${t("col.value")}</th><th>${t("col.owner")}</th><th>${t("col.progress")}</th><th>${t("col.position")}</th></tr></thead><tbody>
    ${objs.map((o) => html`<tr key=${o.id}><td><b>${o.id}</b></td><td>${dash(o.name)}</td><td>${dash(o.value)}</td>
      <td>${o.owner && o.owner !== "none" ? html`<b class=${facClass(o.owner)}>${o.owner}</b>` : html`<span class="dim">${t("obj.neutral")}</span>`}${o.contested ? html` <span class="badge warn">${t("obj.contested")}</span>` : ""}</td>
      <td>${prog(o)}</td><td class="mono">${Math.round(o.pos[0])}, ${Math.round(o.pos[1])}</td></tr>`)}
    </tbody></table></div></section>`;
}
function EventsPanel({ st, faction }) {
  const list = [];
  for (const f of Object.keys(st.factions ?? {})) if (faction === "all" || f === faction) for (const e of st.factions[f].eventLog ?? []) list.push({ ...e, faction: f });
  list.sort((a, b) => b.ts - a.ts);
  if (!list.length) return null;
  return html`<section class="panel side"><h2>${t("ev.title")} <span class="sub">${t("common.newestFirst")}</span></h2><div class="scroll"><table><tbody>
    ${list.slice(0, 30).map((e, i) => html`<tr key=${i}><td class="mono muted">${fmtClock(e.ts)}</td><td class=${facClass(e.faction)}>${e.faction}</td><td class="wrap">${e.text}</td></tr>`)}
    </tbody></table></div></section>`;
}

// ---------- sitrep ----------
function Sitrep({ st, faction }) {
  const names = sortedKeys(st.factions).filter((f) => faction === "all" || f === faction);
  return html`<section class="panel side"><h2>${t("ct.title")} <span class="sub">${t("ct.sub")}</span></h2><div class="scroll">
    ${names.map((f) => {
      const e = st.factions[f], sit = e.situation ?? {}, ct = Array.isArray(sit.contacts) ? sit.contacts : [];
      return html`<div class="facblock"><div class="hd"><b class=${facClass(f)}>${f}</b>
        <span class="muted">${t("ct.hdr", dash(sit.time), Array.isArray(sit.squads) ? sit.squads.length : 0, (((st.now ?? Date.now()) - e.ts) / 1000).toFixed(0))}</span></div>
        <table><thead><tr><th>${t("col.contact")}</th><th>${t("col.position")}</th><th>${t("col.age")}</th></tr></thead><tbody>
        ${ct.length ? ct.map((c) => html`<tr><td>${dash(c.desc)}${c.kind && c.kind !== "unknown" ? html` <span class=${"badge " + (c.kind === "vehicle" ? "bad" : "info")}>${c.kind}</span>` : ""}</td><td class="mono">${gridTxt(c.pos)}</td><td>${dash(c.ageSec)} s</td></tr>`) : html`<tr><td colspan="3" class="dim">${t("ct.none")}</td></tr>`}
        </tbody></table></div>`;
    })}
    ${!names.length && html`<div class="empty">${t("common.noData")}</div>`}</div></section>`;
}

// ---------- exchange log ----------
const J = (o) => JSON.stringify(o, null, 2);
function Log({ st, faction }) {
  const ex = (st.exchanges ?? []).filter((x) => faction === "all" || x.faction === faction);
  const noLlm = st.paused || (st.config?.engine && st.config.engine !== "llm");
  return html`<section class="panel log"><h2>${t("log.title")} <span class="sub">${t("log.sub", st.exchanges?.length ?? 0)}</span>
    ${noLlm && html`<span class="badge warn">${t("strip.hqEngine")} ${st.config?.engine && st.config.engine !== "llm" ? st.config.engine : "llm"}${st.paused ? " " + t("strip.pausedParen") : ""}, ${t("strip.noLlm")}</span>`}</h2><div class="scroll">
    ${ex.map((x) => html`<details class="ex" key=${x.ts + x.faction}><summary>
      <span class="mono muted">${fmtClock(x.ts)}</span><b class=${facClass(x.faction)}>${x.faction}</b>
      <span class="muted">${x.provider}${x.model ? " / " + x.model : ""}${x.fellBack && x.modelUsed && x.modelUsed !== x.model ? html` &rarr; ${x.modelUsed}` : ""}</span>
      <${FbBadge} x=${x} />
      ${x.ok && String(x.reason ?? "").startsWith("skipped") ? html`<span class="badge info">${x.reason}</span>` : x.ok ? html`<span class=${"badge " + (x.orders.length ? "ok" : "warn")}>${x.orders.length ? t("log.ok") : t("log.okNoOrders")}</span>` : html`<span class="badge bad">${t("log.fallback", x.reason)}</span>`}
      ${x.dropped?.length ? html`<span class="badge warn">${t("log.dropped", x.dropped.length)}</span>` : ""}
      ${x.wake && html`<span class=${"badge " + (x.wake.reason === "trigger" ? "warn" : "info")} title=${x.wake.triggers?.join("\n") ?? ""}>${t("log.wake", x.wake.reason)}${x.wake.triggers?.length ? ": " + x.wake.triggers.slice(0, 2).join("; ") + (x.wake.triggers.length > 2 ? ` +${x.wake.triggers.length - 2}` : "") : ""}</span>`}
      ${x.nextCheck && html`<span class="muted" title=${x.nextCheck.reason}>${t("log.next", x.nextCheck.sec)}${x.nextCheck.reason ? ": " + x.nextCheck.reason : ""}</span>`}
      <span>${t("log.ordersN", x.orders.length)}</span>
      ${x.buy?.length ? html`<span class="badge ok" title=${x.buy.map((b) => `${b.count}x ${b.type}`).join(", ")}>${t("log.buy", x.buy.map((b) => `${b.count}x ${b.type}`).join(", "))}</span>` : ""}
      ${x.missions?.length ? html`<span class="badge info">${t("log.offersN", x.missions.reduce((n, m) => n + m.offers.length, 0))}</span>` : ""}
      ${x.keep ? html`<span class="badge info">${t("log.keep")}</span>` : ""}<span class="muted">${x.latencyMs} ms${x.usage ? ` , ${x.usage.input}/${x.usage.output} tok` : ""}${num(x.usage?.cost) ? ` , $${x.usage.cost.toFixed(4)}` : ""}</span></summary>
      <div class="body"><h3>${t("log.hOrders")}</h3><pre>${J(x.orders)}</pre>
      ${x.buy?.length ? html`<h3>${t("log.hBuy")}</h3><pre>${J(x.buy)}</pre>` : ""}
      ${x.missions?.length ? html`<h3>${t("log.hOffers")}</h3><pre>${J(x.missions)}</pre>` : ""}
      ${x.request?.events?.length ? html`<h3>${t("log.hEvents")}</h3><pre>${x.request.events.join("\n")}</pre>` : ""}
      ${x.dropped?.length ? html`<h3>${t("log.hDropped")}</h3><pre>${J(x.dropped)}</pre>` : ""}
      ${x.rawReply != null ? html`<h3>${t("log.hRaw")}</h3><pre>${x.rawReply}</pre>` : ""}
      <${MetricsBox} m=${x.metrics} />
      <h3>${t("col.request")}</h3><pre>${J(x.request)}</pre></div></details>`)}
    ${!ex.length && html`<div class="empty">${t("log.empty")}</div>`}</div></section>`;
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
const LAYER_DEFAULTS = { image: true, structures: true, roads: true, buildings: true, objects: true, places: true, grid: true, objectives: true, squads: true, contacts: true, orders: true, labels: true, flip: false, dim: 0.8 };
function loadLayers() {
  try { return { ...LAYER_DEFAULTS, ...JSON.parse(localStorage.getItem(LAYER_KEY) ?? "{}") }; } catch { return { ...LAYER_DEFAULTS }; }
}
const ago = (st, ts) => (ts ? t("time.agoS", Math.max(0, Math.round(((st.now ?? Date.now()) - ts) / 1000))) : "-");

// Faction selector + "show inactive" toggle; shared by all views (except Usage & Config).
function FactionBar({ names, faction, setFaction, hidden, showInactive, setShowInactive }) {
  return html`<div class="toolbar subbar">
    <div class="seg">${["all", ...names].map((n) => html`<button key=${n} class=${faction === n ? "on" : ""} onClick=${() => setFaction(n)}>${n === "all" ? t("common.all") : n}</button>`)}</div>
    <label class="toggles inact" title=${t("fb.inactiveTip")}>
      <span><input type="checkbox" checked=${showInactive} onChange=${(e) => setShowInactive(e.target.checked)} /> ${t("fb.showInactive")}${hidden.length ? ` (${t("fb.hidden", hidden.length, hidden.join(", "))})` : ""}</span></label>
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
  return html`<section class="panel"><h2>${t("own.title")} <span class="sub">${t("own.sub", objs.length)}${contested ? t("own.contested", contested) : ""}</span></h2><div class="cfgbody">
    <div class="ownbar">${keys.map((k) => html`<i key=${k} style=${{ width: (100 * (by.get(k) ?? 0) / tot) + "%", background: k ? colorOf(k) : NEUTRAL }} title=${(k || t("obj.neutral")) + ": " + (by.get(k) ?? 0) + " / " + tot + (hatch.get(k) ? ", " + t("own.contestedN", hatch.get(k)) : "")}>${hatch.get(k) ? html`<b class="hatch" style=${{ width: (100 * hatch.get(k) / by.get(k)) + "%" }}></b>` : ""}</i>`)}</div>
    <div class="ownleg">${keys.map((k) => html`<span key=${k}><b class=${k ? facClass(k) : "f-none"}>${k || t("obj.neutral")}</b> <span class="muted">${by.get(k) ?? 0} / ${tot}</span></span>`)}</div></div></section>`;
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
    ${!Object.keys(st.factions ?? {}).length && html`<div class="empty">${t("ov.waiting")}</div>`}</main>`;
}

// ---- Map ----
function SelCard({ rows, sel }) {
  const r = sel ? rows.find((x) => x.key === sel) : null;
  if (!r) return html`<div class="muted selcard">${t("sel.hint")}</div>`;
  const o = r.obj;
  return html`<div class="selcard"><b class=${facClass(r.faction)}>${r.name || r.id}</b>${r.name ? html` <span class="muted mono">${r.id}</span>` : ""} ${r.kind ? html`<span class=${"badge kind k-" + r.kind}>${kindLabel(r.kind)}</span>` : ""} <span class=${"badge " + stateBadge(r.state)}>${tk("state", r.state)}</span> <${DormBadge} r=${r} />${r.dormant === false ? html`<span class="badge actv">${t("act.active")}</span>` : ""}
    <span class="muted">${t("sel.soldiers", r.size)}${r.pct !== null ? ", " + r.pct + "%" : ""}${r.vehicle ? ", " + dash(r.vehicle.type) + " (" + (r.vehicle.mounted ? t("veh.mounted") : t("veh.dismounted")) + ")" : ""}, ${r.move}, ${t("map.at1", r.posTxt)}</span>
    ${o ? html` <span class=${"cmd " + o.command}>${o.command}</span> ${o.target ?? ""} <span class="muted">${o.reason ?? ""}</span>` : ""}${r.human ? html` <span class="badge human">${t("badge.human")}</span>` : ""}</div>`;
}
function MapView({ st, faction, rows, sel, onSel, layers, setLayers, world, act, setAct }) {
  const names = sortedKeys(st.factions).filter((f) => faction === "all" || f === faction);
  const tog = (k) => html`<label key=${k}><input type="checkbox" checked=${layers[k]} onChange=${() => setLayers((l) => ({ ...l, [k]: !l[k] }))} /> ${tk("layer", k)}</label>`;
  return html`<main class="view">
    <div class="toolbar">
      <${ActFilter} act=${act} setAct=${setAct} rows=${rows} /><${ActLegend} names=${names} rows=${rows} />
      <div class="toggles nomargin">${["grid", "objectives", "structures", "squads", "contacts", "orders", "labels"].map(tog)}</div>
      <div class="toggles" title=${t("map.layersTip")}>
        ${["image", "roads", "buildings", "objects", "places", "flip"].map(tog)}
        <span class="muted" title=${t("map.keyTip")}>${t("map.keyText")}</span>
        <label>${t("layer.dim")} <input type="range" min="0.1" max="1" step="0.05" value=${layers.dim} onInput=${(e) => setLayers((l) => ({ ...l, dim: Number(e.target.value) }))} style=${{ width: "80px", verticalAlign: "middle" }} /></label>
      </div>
    </div>
    <${MapPanel} st=${st} faction=${faction} rows=${rows} sel=${sel} onSel=${onSel} layers=${layers} world=${world} tall=${true} act=${act} />
    <${SelCard} rows=${rows} sel=${sel} /></main>`;
}

// ---- Forces ----
function CatalogPanel({ st, f }) {
  const e = st.factions[f], sit = e.situation ?? {}, sup = sit.war?.supplies;
  const cat = (Array.isArray(sit.catalog) ? sit.catalog : []).filter((c) => c && typeof c.type === "string").sort((a, b) => (num(a.cost) ? a.cost : 1e9) - (num(b.cost) ? b.cost : 1e9));
  return html`<section class="panel" style=${accent(f)}><h2><span class=${facClass(f)}>${f}</span> ${t("cat.title")} <span class="sub">${t("cat.sub", dash(sup), num(sit.war?.income) ? ` (${t("gm.incomePerMin", sit.war.income)})` : "")}</span></h2><div class="scroll"><table>
    <thead><tr><th>${t("det.type")}</th><th>${t("col.kind")}</th><th>${t("col.cost")}</th><th>${t("col.affordable")}</th><th>${t("col.description")}</th></tr></thead><tbody>
    ${cat.map((c) => html`<tr key=${c.type}><td><b>${c.type}</b></td><td>${c.kind ? html`<span class=${"badge kind k-" + c.kind}>${kindLabel(c.kind)}</span>` : "-"}</td><td class="mono">${dash(c.cost)}</td>
      <td>${num(c.cost) && num(sup) ? html`<span class=${"badge " + (c.cost <= sup ? "ok" : "bad")}>${c.cost <= sup ? t("val.yes") : t("cat.short", c.cost - sup)}</span>` : "-"}</td><td class="wrap muted">${dash(c.desc)}</td></tr>`)}
    </tbody></table>${!cat.length && html`<div class="empty">${t("cat.empty")}</div>`}</div></section>`;
}
function PurchasesPanel({ st, faction }) {
  const list = [];
  for (const x of st.exchanges ?? []) if ((faction === "all" || x.faction === faction) && x.buy?.length) for (const b of x.buy) list.push({ ts: x.ts, faction: x.faction, ...b });
  return html`<section class="panel"><h2>${t("pur.title")} <span class="sub">${t("pur.sub")}</span></h2><div class="scroll"><table>
    <thead><tr><th>${t("col.time")}</th><th>${t("col.faction")}</th><th>${t("col.order")}</th><th>${t("col.reason")}</th></tr></thead><tbody>
    ${list.slice(0, 60).map((b, i) => html`<tr key=${i}><td class="mono muted">${fmtClock(b.ts)}</td><td class=${facClass(b.faction)}>${b.faction}</td><td><b>${b.count}x ${b.type}</b></td><td class="wrap muted">${dash(b.reason)}</td></tr>`)}
    </tbody></table>${!list.length && html`<div class="empty">${t("pur.empty")}</div>`}</div></section>`;
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
  return html`<section class="panel" style=${accent(f)}><h2><span class=${facClass(f)}>${f}</span> ${t("dec.title")} <span class="sub">${x ? fmtClock(x.ts) : t("dec.noExchangeShort")}</span></h2><div class="cfgbody">
    ${sc && html`<div class="muted" title=${(sc.lastWakeTriggers ?? []).join("\n")}>${t("dec.sched", sc.manual ? t("dec.pendingWake") : sc.nextDueInSec > 0 ? t("wake.in", sc.nextDueInSec) : t("wake.due"), dash(sc.intervalSec), dash(sc.chosenSec), sc.chosenReason ? ": " + sc.chosenReason : "", (sc.stretch ?? 1).toFixed(2),
      sc.lastWakeReason ?? "-", sc.lastWakeTriggers?.length ? " (" + sc.lastWakeTriggers.join("; ") + ")" : "", sc.lastLlmAgoSec === null || sc.lastLlmAgoSec === undefined ? t("time.never") : t("time.agoS", sc.lastLlmAgoSec), sc.gated ?? 0, sc.failed ? t("dec.failedSfx") : "")}</div>`}
    ${!x ? html`<div class="empty">${t("dec.noExchange")}</div>` : html`
      <div class="hdr">${x.ok ? html`<span class=${"badge " + (x.orders.length || x.buy?.length || x.missions?.length ? "ok" : "warn")}>${t("log.ok")}</span>` : html`<span class="badge bad">${t("log.fallback", x.reason)}</span>`}
        <${FbBadge} x=${x} />
        <span class="muted">${x.provider}${x.model ? " / " + x.model : ""}${x.fellBack && x.modelUsed && x.modelUsed !== x.model ? html` &rarr; ${x.modelUsed}` : ""}, ${x.latencyMs} ms${x.usage ? `, ${x.usage.input}/${x.usage.output} tok` : ""}${num(x.usage?.cost) ? `, $${x.usage.cost.toFixed(4)}` : ""}</span>
        ${x.wake && html`<span class="badge info" title=${x.wake.triggers?.join("\n") ?? ""}>${t("log.wake", x.wake.reason)}</span>`}
        ${x.nextCheck && html`<span class="muted">${t("dec.nextCheck", x.nextCheck.sec)}${x.nextCheck.reason ? ": " + x.nextCheck.reason : ""}</span>`}${x.keep ? html`<span class="badge info">${t("log.keep")}</span>` : ""}</div>
      <h3 class="cfgh">${t("dec.applied", x.orders.length)}</h3>
      ${x.orders.length ? html`<table><thead><tr><th>${t("col.squad")}</th><th>${t("col.command")}</th><th>${t("col.target")}</th><th>${t("col.reason")}</th></tr></thead><tbody>${x.orders.map((o, i) => html`<tr key=${i}><td><b>${o.squad}</b></td><td><span class=${"cmd " + o.command}>${o.command}</span></td>
        <td class="mono">${o.target ?? gridTxt(Array.isArray(o.pos) ? [o.pos[0], 0, o.pos[1]] : null)}</td><td class="wrap muted">${dash(o.reason)}</td></tr>`)}</tbody></table>` : html`<div class="dim">${t("common.none")}</div>`}
      ${x.buy?.length ? html`<h3 class="cfgh">${t("dec.buy", x.buy.length)}</h3><table><tbody>${x.buy.map((b, i) => html`<tr key=${i}><td><b>${b.count}x ${b.type}</b></td><td class="wrap muted">${dash(b.reason)}</td></tr>`)}</tbody></table>` : ""}
      ${x.missions?.length ? html`<h3 class="cfgh">${t("dec.offers")}</h3><table><tbody>${x.missions.flatMap((m) => m.offers.map((o, i) => html`<tr key=${m.squad + i}><td><b>${m.squad}</b></td><td>${o.kind}</td><td class="mono">${o.target ?? gridTxt(Array.isArray(o.pos) ? [o.pos[0], 0, o.pos[1]] : null)}</td><td class="wrap muted">${dash(o.brief)}</td></tr>`))}</tbody></table>` : ""}
      ${x.dropped?.length ? html`<h3 class="cfgh">${t("dec.dropped", x.dropped.length)}</h3><table><tbody>${x.dropped.map((d, i) => html`<tr key=${i}><td class="wrap mono">${dropLabel(d)}</td><td class="wrap"><span class="badge warn">${d.reason}</span></td></tr>`)}</tbody></table>` : ""}
      ${x.request?.events?.length ? html`<details><summary class="muted">${t("dec.events", x.request.events.length)}</summary><pre>${x.request.events.join("\n")}</pre></details>` : ""}
      <details><summary class="muted">${t("log.hRaw")}</summary><pre>${x.rawReply != null ? x.rawReply : t("dec.noRaw")}</pre></details>`}
    ${e.orders?.length ? html`<div class="muted">${t("dec.standing", e.orders.length)}</div>` : ""}
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
  const why = noHum.length ? " " + t("mis.why", noHum.join(", ")) : "";
  const stBadge = (s) => (s === "accepted" ? "ok" : s === "offered" ? "warn" : s === "completed" ? "info" : "");
  // where an offer / mission points: objective id -> objective centre, else explicit [x, z], else (fallback) the squad
  const aim = (o, squadKey) => {
    const ob = o.target ? objById.get(o.target) : null;
    if (ob) return { pos: ob.pos, label: ob.id + (ob.name ? " " + ob.name : "") };
    if (Array.isArray(o.pos) && num(o.pos[0]) && num(o.pos[1])) return { pos: [o.pos[0], o.pos[1]], label: t("mis.targetLabel") };
    const r = rows.find((x) => x.key === squadKey);
    return r && okPos(r.pos) ? { pos: [r.pos[0], r.pos[2]], label: r.id } : null;
  };
  const go = (key, a, squadKey) => { if (!a) return; if (squadKey) onSel(squadKey); setFocus({ key, pos: a.pos, label: a.label, nonce: Date.now() }); };
  const mapLayers = { ...layers, grid: false, buildings: false, labels: true };
  const tgt = (o) => o.target ?? gridTxt(Array.isArray(o.pos) ? [o.pos[0], 0, o.pos[1]] : null);
  return html`<main class="view">
    <div class="grid2">
      <section class="panel"><h2>${t("mis.humans")} <span class="sub">${t("mis.humansSub", humans.length)}</span></h2><div class="scroll"><table>
        <thead><tr><th>${t("col.squad")}</th><th>${t("col.faction")}</th><th>${t("col.mission")}</th><th>${t("col.kind")}</th><th>${t("col.target")}</th><th>${t("col.state")}</th></tr></thead><tbody>
        ${humans.map((r) => { const key = "h:" + r.key;
          return html`<tr key=${r.key} class=${"sqrow" + (focus?.key === key ? " sel" : "")} onClick=${() => go(key, aim(r.mission ?? {}, r.key), r.key)}><td style=${{ borderLeft: "3px solid " + colorOf(r.faction) }}><b>${r.id}</b></td><td class=${facClass(r.faction)}>${r.faction}</td>
          <td>${r.mission ? html`<span class=${"badge " + stBadge(r.mission.state)}>${r.mission.state}</span>` : html`<span class="dim">${t("common.none")}</span>`}</td><td>${dash(r.mission?.kind)}</td><td>${dash(r.mission?.target)}</td>
          <td><span class=${"badge " + stateBadge(r.state)}>${tk("state", r.state)}</span></td></tr>`; })}</tbody></table>
        ${!humans.length && html`<div class="empty">${t("mis.noHumans")}</div>`}</div></section>
      <section class="panel"><h2>${t("mis.offers")} <span class="sub">${t("mis.offersSub")}</span></h2><div class="scroll"><table>
        <thead><tr><th>${t("col.faction")}</th><th>${t("col.squad")}</th><th>${t("col.kind")}</th><th>${t("col.target")}</th><th>${t("col.brief")}</th></tr></thead><tbody>
        ${offers.map((o, i) => { const key = "o:" + i; return html`<tr key=${i} class=${"sqrow" + (focus?.key === key ? " sel" : "")} onClick=${() => go(key, aim(o, o.faction + "/" + o.squad), o.faction + "/" + o.squad)}><td style=${{ borderLeft: "3px solid " + colorOf(o.faction) }} class=${facClass(o.faction)}>${o.faction}</td><td><b>${o.squad}</b></td><td>${o.kind}</td><td class="mono">${tgt(o)}</td><td class="wrap muted">${dash(o.brief)}</td></tr>`; })}</tbody></table>
        ${!offers.length && html`<div class="empty">${t("mis.noOffers")}${why}</div>`}</div></section>
    </div>
    <${MapPanel} st=${st} faction=${faction} rows=${rows} sel=${sel} onSel=${onSel} layers=${mapLayers} world=${world} act=${act} focus=${focus} compact=${true} />
    <div class="grid2" style=${{ marginTop: "12px" }}>
      <section class="panel"><h2>${t("mis.events")} <span class="sub">${t("mis.eventsSub")}</span></h2><div class="scroll"><table><tbody>
        ${evs.slice(0, 40).map((e, i) => html`<tr key=${i}><td class="mono muted">${fmtClock(e.ts)}</td><td class=${facClass(e.faction)}>${e.faction}</td><td class="wrap">${e.text}</td></tr>`)}</tbody></table>
        ${!evs.length && html`<div class="empty">${t("mis.noEvents")}${why}</div>`}</div></section>
      <section class="panel"><h2>${t("mis.history")} <span class="sub">${t("mis.historySub")}</span></h2><div class="scroll"><table>
        <thead><tr><th>${t("col.time")}</th><th>${t("col.faction")}</th><th>${t("col.squad")}</th><th>${t("col.kind")}</th><th>${t("col.target")}</th><th>${t("col.brief")}</th></tr></thead><tbody>
        ${hist.slice(0, 60).map((o, i) => { const key = "x:" + i; return html`<tr key=${i} class=${"sqrow" + (focus?.key === key ? " sel" : "")} onClick=${() => go(key, aim(o, o.faction + "/" + o.squad), null)}><td class="mono muted">${fmtClock(o.ts)}</td><td class=${facClass(o.faction)}>${o.faction}</td><td><b>${o.squad}</b></td><td>${o.kind}</td><td class="mono">${tgt(o)}</td><td class="wrap muted">${dash(o.brief)}</td></tr>`; })}</tbody></table>
        ${!hist.length && html`<div class="empty">${t("mis.noHistory")}${why}</div>`}</div></section>
    </div></main>`;
}


// ---- player requests (M3 phase Q): situation.requests[] of every faction, newest first ----
function RequestsView({ st, faction }) {
  const rows = [];
  for (const [f, fs] of Object.entries(st.factions ?? {})) {
    if (faction !== "all" && f !== faction) continue;
    const sit = fs?.situation;
    const ts = fs?.ts ?? 0;
    for (const r of Array.isArray(sit?.requests) ? sit.requests : []) rows.push({ ...r, faction: f, at: ts - (num(r.ageSec) ? r.ageSec * 1000 : 0) });
  }
  rows.sort((a, b) => b.at - a.at);
  const granted = rows.filter((r) => r.status === "granted").length;
  return html`<main class="view">
    <section class="panel"><h2>${t("req.title")} <span class="sub">${t("req.sub", rows.length, granted)}</span></h2><div class="scroll"><table>
      <thead><tr><th>${t("col.time")}</th><th>${t("col.faction")}</th><th>${t("col.player")}</th><th>${t("col.role")}</th><th>${t("col.request")}</th><th>${t("col.status")}</th><th>${t("col.answer")}</th></tr></thead><tbody>
      ${rows.map((r, i) => html`<tr key=${r.faction + r.id + i}><td class="mono muted">${r.at ? fmtClock(r.at) : "-"}</td><td class=${facClass(r.faction)}>${r.faction}</td><td><b>${dash(r.player)}</b></td><td>${dash(r.role)}</td>
        <td><b>${dash(r.kind)}</b> <span class="mono muted">${dash(r.args)}</span></td><td><span class=${"badge " + (r.status === "granted" ? "ok" : "bad")}>${dash(r.status)}</span></td><td class="wrap muted">${dash(r.reason)}</td></tr>`)}</tbody></table>
      ${!rows.length && html`<div class="empty">${t("req.empty")}</div>`}</div></section>
  </main>`;
}

// ---- HQ chat (M3 phase A): /hq <text> messages answered by the sidecar agent turn (tool calls, queued actions, reply, cost) ----
function HqChatView({ st, faction }) {
  const [open, setOpen] = useState(null);
  const all = Array.isArray(st.hq) ? st.hq : [];
  const rows = faction === "all" ? all : all.filter((h) => h.faction === faction);
  const cost = rows.reduce((a, h) => a + (h.usage?.cost ?? 0), 0);
  const state = (h) => (h.state === "running" ? ["warn", t("chat.running")] : h.ok ? ["ok", t("chat.answered")] : ["bad", h.error?.startsWith("fallback") ? t("chat.fallback") : t("chat.failed")]);
  return html`<main class="view">
    <section class="panel"><h2>${t("chat.title")} <span class="sub">${t("chat.sub", rows.length, usd4(cost))}</span></h2><div class="scroll"><table>
      <thead><tr><th>${t("col.time")}</th><th>${t("col.faction")}</th><th>${t("col.player")}</th><th>${t("col.message")}</th><th>${t("col.tools")}</th><th>${t("col.actions")}</th><th>${t("col.reply")}</th><th>${t("col.state")}</th><th>${t("col.cost")}</th><th>${t("col.latency")}</th></tr></thead><tbody>
      ${rows.flatMap((h, i) => {
        const [cls, label] = state(h), calls = (h.steps ?? []).flatMap((s) => s.toolCalls ?? []), key = h.faction + h.msg.id + h.ts;
        return [html`<tr key=${key} class=${"sqrow" + (open === key ? " sel" : "")} onClick=${() => setOpen(open === key ? null : key)}>
          <td class="mono muted">${fmtClock(h.ts)}</td><td class=${facClass(h.faction)}>${h.faction}</td>
          <td><b>${h.msg.player}</b> <span class="muted">${h.msg.role}${h.msg.grid ? ", " + h.msg.grid : ""}</span></td><td class="wrap">${h.msg.text}${h.historyCount ? html` <span class="badge" title=${t("chat.historyTip")}>${t("chat.history", h.historyCount)}</span>` : ""}</td>
          <td class="mono">${t("chat.steps", (h.steps ?? []).length, calls.length)}</td>
          <td class="mono wrap">${(h.actions ?? []).map((a) => a.action + "(" + (a.args ?? []).map((x) => x.name + "=" + x.value).join(", ") + ")").join("; ") || "-"}</td>
          <td class="wrap">${dash(h.reply || h.error)}</td><td><span class=${"badge " + cls}>${label}</span> <${FbBadge} x=${h} /></td>
          <td class="mono">${h.usage?.cost ? usd4(h.usage.cost) : "-"}</td><td class="mono">${h.latencyMs != null ? h.latencyMs + " ms" : "-"}</td></tr>`,
        open === key && html`<tr key=${key + "d"}><td colspan="10"><div class="cfgbody">
          <div class="muted">${h.provider}${h.model ? " / " + h.model : ""}${h.fellBack && h.modelUsed && h.modelUsed !== h.model ? html` &rarr; ${h.modelUsed}` : ""}; ${t("chat.detail", (h.tools ?? []).length, h.usage?.input ?? 0, h.usage?.output ?? 0)}</div>
          ${(h.steps ?? []).map((s) => html`<div key=${s.n} class="wrap"><b>${t("chat.step", s.n)}</b> <span class="muted">${s.finishReason}</span>
            ${(s.toolCalls ?? []).map((c, j) => html`<div key=${j} class="mono wrap">&rarr; ${c.name}(${JSON.stringify(c.input)}) = ${JSON.stringify(c.output)}</div>`)}
            ${s.text && html`<div>${s.text}</div>`}</div>`)}
          ${(h.msg.history ?? []).length > 0 && html`<div class="wrap"><b>${t("chat.historyHead")}</b>${h.msg.history.map((x, j) => html`<div key=${j} class="mono wrap muted">${x.p} &rarr; ${x.hq}</div>`)}</div>`}
          ${h.error && html`<div class="wrap"><span class="badge bad">${h.error}</span></div>`}</div></td></tr>`];
      })}</tbody></table>
      ${!rows.length && html`<div class="empty">${t("chat.empty")}</div>`}</div></section>
  </main>`;
}

// ---- debug log: the game's CLW_Debug lines (not shown in game chat by default), newest first ----
function DebugView({ st }) {
  const [q, setQ] = useState("");
  const all = Array.isArray(st.debugLog) ? st.debugLog : [];
  const needle = q.trim().toLowerCase();
  const rows = (needle ? all.filter((d) => String(d.msg).toLowerCase().includes(needle)) : all).slice(0, 300);
  const clock = (sec) => { const s = Math.max(0, Math.round(sec)), p = (v) => String(v).padStart(2, "0"); return `${p(Math.floor(s / 3600))}:${p(Math.floor((s % 3600) / 60))}:${p(s % 60)}`; };
  return html`<main class="view"><section class="panel"><h2>${t("dbg.title")} <span class="sub">${t("dbg.sub", rows.length, all.length)}</span></h2>
    <div class="cfgbody"><label class="field"><span>${t("dbg.filter")}</span><input type="text" value=${q} placeholder=${t("dbg.placeholder")} onInput=${(e) => setQ(e.target.value)} /></label></div>
    <div class="scroll"><table><tbody>
      ${rows.map((d, i) => html`<tr key=${i}><td class="mono muted">${fmtClock(d.ts)}</td><td class="mono muted">${clock(d.t)}</td><td class="wrap">${d.msg}</td></tr>`)}</tbody></table>
      ${!rows.length && html`<div class="empty">${all.length ? t("dbg.noMatch") : t("dbg.none")}</div>`}</div></section></main>`;
}

// ---- action manifest (protocol v3): what each faction's game offers HQ in this war mode, read-only ----
function ManifestTable({ title, rows }) {
  return html`<h3 class="cfgh">${title} <span class="muted">(${rows.length})</span></h3>
    ${rows.length ? html`<div class="scroll"><table><thead><tr><th>${t("col.name")}</th><th>${t("man.replyKey")}</th><th>${t("man.authority")}</th><th>${t("man.requires")}</th><th>${t("man.params")}</th><th>${t("col.description")}</th></tr></thead><tbody>
      ${rows.map((e) => html`<tr key=${e.name}><td><b>${e.name}</b></td><td class="mono">${e.channel}</td><td>${e.authority}</td><td class="mono">${(e.requires ?? []).join(", ") || "-"}</td>
        <td class="mono wrap">${(e.params ?? []).map((p) => p.name + (p.required ? "*" : "") + ":" + p.type).join(", ")}</td><td class="wrap muted">${e.desc}</td></tr>`)}</tbody></table></div>` : html`<div class="empty">${t("common.none")}</div>`}`;
}
function ActionsView({ st }) {
  const ms = st.manifests ?? { factions: {}, builtin: null };
  const names = Object.keys(ms.factions ?? {}).sort();
  return html`<main class="view">
    ${names.map((n) => { const m = ms.factions[n]; return html`<section class="panel" key=${n}><h2>${n} <span class="sub">${t("man.sub", m.hash, m.mode || "?", fmtClock(m.receivedAt))}</span></h2>
      <div class="cfgbody"><div class="muted">${t("man.caps")} <b>${(m.capabilities ?? []).join(", ") || t("common.none")}</b></div>
      <${ManifestTable} title=${t("man.intents")} rows=${m.intents ?? []} /><${ManifestTable} title=${t("man.primitives")} rows=${m.primitives ?? []} /></div></section>`; })}
    ${!names.length && ms.builtin && html`<section class="panel"><h2>${t("man.builtin")} <span class="sub">${t("man.builtinSub")}</span></h2><div class="cfgbody"><${ManifestTable} title=${t("man.intentsShort")} rows=${ms.builtin.intents ?? []} /></div></section>`}
  </main>`;
}

// ---- Latency (LLM call metrics: st.latency per role, st.llmCalls ring; both optional, older sidecars have neither) ----
const SLOW_MS = 10000;
const fmtMs = (v) => (!num(v) ? "-" : v < 1000 ? `${Math.round(v)} ms` : `${(v / 1000).toFixed(1)} s`);
const fmtTok = (v) => (num(v) ? String(Math.round(v)) : "-");
const roleLabel = (r) => tk("lat.role", r) ?? "?";
// a call is slow above the role's configured timeout (config.roleTimeouts, seconds, 0 = slot default) or above 10 s
const isSlow = (c, cfg) => { const to = cfg?.roleTimeouts?.[c.role]; return num(c.totalMs) && (c.totalMs > SLOW_MS || (num(to) && to > 0 && c.totalMs > to * 1000)); };
// prompt sections {name: {chars, tokens}}, largest first, one bar each (width relative to the largest)
function SectionBars({ sections, title }) {
  const rows = Object.entries(sections && typeof sections === "object" ? sections : {}).filter(([, v]) => v && typeof v === "object").map(([k, v]) => [k, num(v.chars) ? v.chars : 0, num(v.tokens) ? v.tokens : 0]).sort((a, b) => b[2] - a[2] || b[1] - a[1]);
  if (!rows.length) return "";
  const mx = Math.max(1, rows[0][2]), tot = rows.reduce((n, r) => n + r[2], 0);
  return html`<div class="secbars">${title ? html`<h3>${title}</h3>` : ""}${rows.map(([k, ch, tok]) => html`<div class="secrow" key=${k}><span class="secname mono">${k}</span>
    <span class="secbar"><i style=${{ width: Math.max(1, Math.round((100 * tok) / mx)) + "%" }}></i></span>
    <span class="secval mono muted">${t("lat.secVal", ch, tok, tot ? Math.round((100 * tok) / tot) : 0)}</span></div>`)}</div>`;
}
// metrics of one LLM call (exchange.metrics = the llmCalls entry of its commander call)
function MetricsBox({ m }) {
  if (!m || typeof m !== "object") return "";
  return html`<h3>${t("lat.metrics")}</h3><div class="metrics">
    <span>${t("lat.ttft")} <b>${fmtMs(m.ttftMs)}</b></span><span>${t("lat.total")} <b>${fmtMs(m.totalMs)}</b></span>
    <span>${t("lat.tokIn")} <b>${fmtTok(m.inTok)}</b></span><span>${t("lat.tokOut")} <b>${fmtTok(m.outTok)}</b></span><span>${t("lat.tokCached")} <b>${fmtTok(m.cachedTok)}</b></span>
    ${m.outputMode ? html`<span>${t("lat.mode")} <b>${m.outputMode}</b></span>` : ""}${m.slot ? html`<span>${t("lat.slot")} <b>${m.slot}</b></span>` : ""}
    ${num(m.promptChars) ? html`<span>${t("lat.promptChars")} <b>${m.promptChars}</b></span>` : ""}${m.fellBack ? html`<span class="badge warn">${t("fb.badge")}</span>` : ""}</div>
    <${SectionBars} sections=${m.sections} />`;
}
function LatencyView({ st }) {
  const [open, setOpen] = useState(null);
  const lat = st.latency && typeof st.latency === "object" ? st.latency : {}, calls = Array.isArray(st.llmCalls) ? st.llmCalls : [], cfg = st.config ?? {};
  const roles = Object.keys(lat).filter((r) => lat[r] && typeof lat[r] === "object");
  const now = st.now ?? Date.now();
  const callKey = (c, i) => `${c.ts}|${c.faction}|${c.role}|${i}`;
  return html`<main class="view">
    <section class="panel"><h2>${t("lat.sumTitle")} <span class="sub">${t("lat.sumSub")}</span></h2><div class="scroll"><table>
      <thead><tr><th>${t("lat.col.role")}</th><th>${t("lat.col.n")}</th><th>${t("lat.col.ok")}</th><th>${t("lat.col.p50")}</th><th>${t("lat.col.p90")}</th><th>${t("lat.col.ttft")}</th><th>${t("lat.col.in")}</th><th>${t("lat.col.out")}</th><th>${t("lat.col.cached")}</th><th>${t("lat.col.last")}</th></tr></thead><tbody>
      ${roles.map((r) => { const e = lat[r], slow = num(e.p90Ms) && e.p90Ms > SLOW_MS; return html`<tr key=${r}><td><b>${roleLabel(r)}</b></td><td>${dash(e.n)}</td>
        <td><span class=${"badge " + (num(e.n) && e.okN === e.n ? "ok" : "warn")}>${dash(e.okN)}/${dash(e.n)}</span></td>
        <td class="mono">${fmtMs(e.p50Ms)}</td><td class=${"mono" + (slow ? " slow" : "")}>${fmtMs(e.p90Ms)}</td><td class="mono">${fmtMs(e.avgTtftMs)}</td>
        <td class="mono">${fmtTok(e.avgInTok)}</td><td class="mono">${fmtTok(e.avgOutTok)}</td><td class="mono">${fmtTok(e.avgCachedTok)}</td>
        <td class="mono muted">${fmtMs(e.lastMs)}${num(e.lastAt) ? " , " + t("time.agoS", Math.max(0, Math.round((now - e.lastAt) / 1000))) : ""}</td></tr>`; })}</tbody></table>
      ${!roles.length && html`<div class="empty">${t("lat.sumEmpty")}</div>`}</div></section>
    <section class="panel" style=${{ marginTop: "12px" }}><h2>${t("lat.callsTitle")} <span class="sub">${t("lat.callsSub", calls.length)}</span></h2><div class="scroll"><table>
      <thead><tr><th>${t("lat.col.time")}</th><th>${t("lat.col.faction")}</th><th>${t("lat.col.role")}</th><th>${t("cfg.model")}</th><th>${t("lat.col.in")}</th><th>${t("lat.col.out")}</th><th>${t("lat.col.cached")}</th><th>${t("lat.col.ttft")}</th><th>${t("lat.total")}</th><th>${t("lat.col.status")}</th><th>${t("lat.mode")}</th></tr></thead><tbody>
      ${calls.slice(0, 200).map((c, i) => { if (!c || typeof c !== "object") return ""; const k = callKey(c, i), isOpen = open === k, slow = isSlow(c, cfg);
        return [html`<tr key=${k} class=${"sqrow" + (isOpen ? " sel" : "") + (slow ? " slowrow" : "")} onClick=${() => setOpen(isOpen ? null : k)} title=${t("lat.rowTip")}>
          <td class="mono muted">${num(c.ts) ? fmtClock(c.ts) : "-"}</td><td class=${facClass(c.faction)}>${dash(c.faction)}</td><td><b>${roleLabel(c.role)}</b></td><td class="muted">${dash(c.model)}</td>
          <td class="mono">${fmtTok(c.inTok)}</td><td class="mono">${fmtTok(c.outTok)}</td><td class="mono">${fmtTok(c.cachedTok)}</td><td class="mono">${fmtMs(c.ttftMs)}</td><td class=${"mono" + (slow ? " slow" : "")}>${fmtMs(c.totalMs)}${slow ? html` <span class="badge warn">${t("lat.slow")}</span>` : ""}</td>
          <td>${c.ok ? html`<span class="badge ok">${t("log.ok")}</span>` : html`<span class="badge bad" title=${c.error ?? ""}>${t("lat.err")}</span>`}${c.fellBack ? html` <span class="badge warn" title=${t("fb.tip", c.error ?? "?")}>${t("fb.badge")}</span>` : ""}</td>
          <td class="muted">${dash(c.outputMode)}</td></tr>`,
          isOpen && html`<tr key=${k + "d"} class="secdetail"><td colspan="11">${c.error ? html`<div class="muted wrap">${t("lat.error", c.error)}</div>` : ""}
            <${SectionBars} sections=${c.sections} title=${t("lat.sections", dash(c.promptChars))} />${!c.sections || !Object.keys(c.sections).length ? html`<div class="dim">${t("lat.noSections")}</div>` : ""}</td></tr>`]; })}</tbody></table>
      ${!calls.length && html`<div class="empty">${t("lat.callsEmpty")}</div>`}</div></section></main>`;
}

// ---- tabs ----
const TABS = [["overview", "tab.overview"], ["map", "tab.map"], ["forces", "tab.forces"], ["hq", "tab.hq"], ["journal", "tab.journal"], ["missions", "tab.missions"], ["requests", "tab.requests"], ["hqchat", "tab.hqchat"], ["actions", "tab.actions"], ["loadouts", "tab.loadouts"], ["latency", "tab.latency"], ["debug", "tab.debug"], ["usage", "tab.usage"]];
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
  const [addonDirs, setAddonDirs] = useState(null); // one dir per line
  const [maxSize, setMaxSize] = useState(2048);
  const load = useCallback(async () => {
    try { setW(await (await fetch("/api/world", { cache: "no-store" })).json()); } catch { /* offline */ }
  }, []);
  useEffect(() => {
    load(); const t = setInterval(load, 3000);
    fetch("/api/config", { cache: "no-store" }).then((r) => r.json()).then((c) => { setGameDir(c.gameDir ?? ""); setAddonDirs((c.addonDirs ?? []).join("\n")); setMaxSize(c.mapMaxSize ?? 2048); }).catch(() => {});
    return () => clearInterval(t);
  }, [load]);
  const act = async (url, body, okMsg) => {
    try { const { ok, j } = await postJson(url, body); toast(ok ? okMsg : t("common.failed", j.error ?? t("err.request")), ok ? "ok" : "err"); load(); }
    catch (err) { toast(t("err.generic", err.message), "err"); }
  };
  const m = w?.map ?? {}, v = w?.vector ?? {};
  return html`<section class="panel" style=${{ marginTop: "12px" }}><h2>${t("map.title")} <span class="sub">${t("mc.sub")}</span></h2>
    <div class="cfgbody">
      <div class="muted">${t("mc.world")} <b>${w?.worldKey || t("mc.noWorld")}</b>${w?.info?.mapPrefab ? " / " + w.info.mapPrefab : ""}</div>
      <div>${t("mc.image")} <b>${m.status ?? t("common.none")}</b>${m.source ? html` <span class="muted">${m.source}, ${m.width}x${m.height}</span>` : ""}${m.error ? html` <span class="muted">${m.error}</span>` : ""}</div>
      <div>${t("mc.roads")} <b>${v.roads ?? t("mc.notReceived")}</b>, ${t("mc.places")} <b>${v.locations ?? t("mc.notReceived")}</b>, ${t("mc.buildings")} <b>${v.buildings ?? t("mc.notReceived")}</b></div>
      ${v.roads == null || v.locations == null || v.buildings == null ? html`<div class="dim">${t("mc.waiting")}</div>` : null}
      <div class="muted">${t("mc.detected", w?.detectedGameDir || t("common.none"))}</div>
      <label class="field"><span>${t("mc.gameDir")}</span><input type="text" value=${gameDir ?? ""} placeholder=${t("mc.gameDirPh")} onInput=${(e) => setGameDir(e.target.value)} /></label>
      <div class="muted">${t("mc.addonsEff", w?.addonDirs?.count ?? 0)}${w?.addonDirs?.paths?.length ? html` <span title=${w.addonDirs.paths.join("\n")}>(${w.addonDirs.paths.slice(0, 3).join(", ")}${w.addonDirs.paths.length > 3 ? ", ..." : ""})</span>` : ""}${w?.addonDirs?.skipped?.length ? " " + t("mc.addonsSkipped", w.addonDirs.skipped.map((x) => x.path).join(", ")) : ""}</div>
      <label class="field"><span>${t("mc.addonDirs")}</span><textarea rows="3" value=${addonDirs ?? ""} placeholder=${t("mc.addonDirsPh")} onInput=${(e) => setAddonDirs(e.target.value)}></textarea></label>
      <label class="field"><span>${t("mc.maxSize")}</span><input type="number" min="256" max="8192" value=${maxSize} onInput=${(e) => setMaxSize(e.target.value)} /></label>
      <div class="btns">
        <button class="btn" type="button" onClick=${() => act("/api/config", { gameDir: gameDir ?? "", addonDirs: (addonDirs ?? "").split(/\r?\n/).map((x) => x.trim()).filter(Boolean), mapMaxSize: Number(maxSize) }, t("mc.saved"))}><${Icon} n="save" /> ${t("common.save")}</button>
        <button class="btn ghost" type="button" onClick=${() => act("/api/map/reload", {}, t("mc.reresolving"))}><${Icon} n="reset" /> ${t("mc.reresolve")}</button>
        <button class="btn ghost" type="button" onClick=${() => act("/api/vector/reset", {}, t("mc.vectorCleared"))}><${Icon} n="reset" /> ${t("mc.resend")}</button></div>
      <small class="muted">${t("mc.note")}</small>
    </div></section>`;
}
// ---------- backup (fallback) model of an LLM slot ----------
const fbForm = (x) => ({ model: x?.model ?? "", provider: x?.provider ?? "", baseUrl: x?.baseUrl ?? "", apiKey: "", timeoutSec: x?.timeoutSec ?? "" });
// no model = no backup (null clears it); empty provider / baseUrl / timeout = inherit from the slot; an empty key keeps the stored one
const fbBody = (v) => (!v || !v.model.trim() ? null : { model: v.model.trim(), provider: v.provider, baseUrl: v.baseUrl.trim(), timeoutSec: v.timeoutSec === "" ? null : Number(v.timeoutSec), ...(v.apiKey ? { apiKey: v.apiKey } : {}) });
function BackupBox({ v, setV, meta, busy, onTest, res }) {
  return html`<details class="fbbox" open=${!!v.model}><summary>${t("fb.title")}${v.model && html` <span class="badge info">${v.model}</span>`}</summary>
    <div class="fbin">
      <small class="muted">${t("fb.note")}</small>
      <label class="field"><span>${t("fb.model")}</span><input type="text" value=${v.model} placeholder=${t("fb.modelPh")} onInput=${(e) => setV("model", e.target.value)} /></label>
      <details class="fbadv"><summary>${t("fb.advanced")}</summary>
        <small class="muted">${t("fb.inherit")}</small>
        <label class="field"><span>${t("cfg.provider")}</span><select value=${v.provider} onChange=${(e) => setV("provider", e.target.value)}>
          <option value="">${t("fb.sameAsMain")}</option>
          <option value="anthropic">anthropic</option>
          <option value="openai-compatible">${t("cfg.opt.openai")}</option>
          <option value="openrouter">${t("cfg.opt.openrouter")}</option></select></label>
        <label class="field"><span>${t("cfg.baseUrl")}</span><input type="text" value=${v.baseUrl} placeholder=${t("fb.sameAsMain")} onInput=${(e) => setV("baseUrl", e.target.value)} /></label>
        <label class="field"><span>${t("cfg.apiKey")}</span><input type="password" autocomplete="new-password" value=${v.apiKey} placeholder=${meta.set ? t("cfg.keyStored", meta.last4) : t("fb.sameAsMain")} onInput=${(e) => setV("apiKey", e.target.value)} />
          <small>${t("fb.keyNote")}</small></label>
        <label class="field"><span>${t("cfg.timeout")}</span><input type="number" min="1" max="300" value=${v.timeoutSec} placeholder=${t("fb.sameAsMain")} onInput=${(e) => setV("timeoutSec", e.target.value)} /></label>
      </details>
      <div class="btns"><button class="btn ghost" type="button" disabled=${busy || !v.model.trim()} onClick=${onTest}><${Icon} n="plug" /> ${t("fb.test")}</button></div>
      ${res && html`<pre>${res.pending ? t("cfg.testing") : J(res)}</pre>`}
    </div></details>`;
}
// small "answered by the backup model" badge for exchange / chat log rows
// ---------- latency settings groups (llm-latency-design.md "New config"): key -> [type, default, min, max] ----------
// A group is shown only when GET /api/config returned it (older sidecars lack most of them), and posted only when changed, so nothing is ever rejected as unknown.
const ROLE_KEYS = ["commander", "planner", "sitrep", "journal", "hq", "translate"];
const LAT_GROUPS = {
  sitrep: { enabled: ["b", true], intervalSec: ["n", 120, 30, 3600] },
  output: { mode: ["s", "json_object"], terse: ["b", true], reasonWords: ["n", 12, 4, 30], reasoning: ["s", "off"] },
  planner: { enabled: ["b", true], intervalSec: ["n", 180, 60, 900] },
  roles: { commander: ["s", "strong"], planner: ["s", "strong"], sitrep: ["s", "weak"], journal: ["s", "weak"], hq: ["s", "strong"], translate: ["s", "weak"] },
  escalate: { enabled: ["b", false], slot: ["s", "strong"] },
  roleTimeouts: { commander: ["n", 0, 0, 300], planner: ["n", 0, 0, 300], sitrep: ["n", 0, 0, 300], journal: ["n", 0, 0, 300], hq: ["n", 0, 0, 300] },
  prompt: { lean: ["b", true], staticBlock: ["b", true], cache: ["b", true], contactMax: ["n", 25, 5, 60], contactMaxAgeSec: ["n", 600, 60, 1800] },
};
const latTyped = (g, v) => Object.fromEntries(Object.entries(LAT_GROUPS[g]).map(([k, [ty, def]]) => {
  const x = v?.[k];
  return [k, ty === "b" ? (typeof x === "boolean" ? x : x === undefined || x === null ? def : !!x) : ty === "n" ? (x === "" || x === null || x === undefined || !Number.isFinite(Number(x)) ? def : Number(x)) : (typeof x === "string" && x ? x : def)];
}));
// form state from the config reply: groups the backend did not return are left out (their UI is hidden)
function latInit(c) {
  const out = { _lat: { snap: {} } };
  for (const g of Object.keys(LAT_GROUPS)) {
    const v = c?.[g];
    if (!v || typeof v !== "object" || Array.isArray(v)) continue;
    if (g === "roles" && !("planner" in v)) continue; // the pre-latency sidecar reports a read-only role map here
    out[g] = latTyped(g, v);
    out._lat.snap[g] = JSON.stringify(out[g]);
  }
  return out;
}
// adds the changed groups to the POST body
function latBody(f, b) {
  for (const g of Object.keys(f._lat?.snap ?? {})) {
    const v = latTyped(g, f[g]);
    if (JSON.stringify(v) !== f._lat.snap[g]) b[g] = v;
  }
}
function LatencySettings({ f, setG }) {
  const has_ = (g) => !!f._lat?.snap && g in f._lat.snap;
  if (!Object.keys(f._lat?.snap ?? {}).some((g) => g !== "sitrep")) return "";
  const sw = (g, k, label) => html`<label class="switch"><input type="checkbox" checked=${f[g][k]} onChange=${(e) => setG(g, k, e.target.checked)} /><span>${label}</span></label>`;
  const nm = (g, k, label) => { const [, , min, max] = LAT_GROUPS[g][k]; return html`<label class="field"><span>${label}</span><input type="number" min=${min} max=${max} value=${f[g][k]} onInput=${(e) => setG(g, k, e.target.value)} /></label>`; };
  const slotSel = (g, k, label) => html`<label class="field"><span>${label}</span><select value=${f[g][k]} onChange=${(e) => setG(g, k, e.target.value)}>
    <option value="strong">${t("cfg.l.strong")}</option><option value="weak">${t("cfg.l.weak")}</option></select></label>`;
  return html`<h3 class="cfgh">${t("cfg.l.title")}</h3><small class="muted">${t("cfg.l.note")}</small>
    ${has_("output") && html`<h3 class="cfgh">${t("cfg.l.output")}</h3>
      <label class="field"><span>${t("cfg.l.mode")}</span><select value=${f.output.mode} onChange=${(e) => setG("output", "mode", e.target.value)}>
        <option value="json_object">${t("cfg.l.mode.json_object")}</option><option value="json_schema">${t("cfg.l.mode.json_schema")}</option><option value="text">${t("cfg.l.mode.text")}</option></select>
        <small>${t("cfg.l.modeHelp")}</small></label>
      <div class="pair">${sw("output", "terse", t("cfg.l.terse"))}${nm("output", "reasonWords", t("cfg.l.reasonWords"))}</div>
      <small class="muted">${t("cfg.l.terseHelp")}</small>
      <label class="field"><span>${t("cfg.l.reasoning")}</span><select value=${f.output.reasoning} onChange=${(e) => setG("output", "reasoning", e.target.value)}>
        ${["off", "default", "low", "medium", "high"].map((m) => html`<option value=${m}>${t("cfg.l.reasoning." + m)}</option>`)}</select>
        <small>${t("cfg.l.reasoningHelp")}</small></label>`}
    ${has_("planner") && html`<h3 class="cfgh">${t("cfg.l.planner")}</h3>
      <div class="pair">${sw("planner", "enabled", t("cfg.l.plannerOn"))}${nm("planner", "intervalSec", t("cfg.l.plannerInt"))}</div>
      <small class="muted">${t("cfg.l.plannerHelp")}</small>`}
    ${has_("roles") && html`<h3 class="cfgh">${t("cfg.l.roles")}</h3><small class="muted">${t("cfg.l.rolesHelp")}</small>
      <div class="pair">${ROLE_KEYS.map((r) => slotSel("roles", r, tk("lat.role", r)))}</div>`}
    ${has_("escalate") && html`<h3 class="cfgh">${t("cfg.l.escalate")}</h3>
      <div class="pair">${sw("escalate", "enabled", t("cfg.l.escalateOn"))}${slotSel("escalate", "slot", t("cfg.l.escalateSlot"))}</div>
      <small class="muted">${t("cfg.l.escalateHelp")}</small>`}
    ${has_("roleTimeouts") && html`<h3 class="cfgh">${t("cfg.l.timeouts")}</h3><small class="muted">${t("cfg.l.timeoutsHelp")}</small>
      <div class="pair">${Object.keys(LAT_GROUPS.roleTimeouts).map((r) => nm("roleTimeouts", r, tk("lat.role", r)))}</div>`}
    ${has_("prompt") && html`<h3 class="cfgh">${t("cfg.l.prompt")}</h3>
      <div class="pair">${sw("prompt", "lean", t("cfg.l.lean"))}${sw("prompt", "staticBlock", t("cfg.l.staticBlock"))}</div>
      ${sw("prompt", "cache", t("cfg.l.cache"))}
      <div class="pair">${nm("prompt", "contactMax", t("cfg.l.contactMax"))}${nm("prompt", "contactMaxAgeSec", t("cfg.l.contactAge"))}</div>
      <small class="muted">${t("cfg.l.promptHelp")}</small>`}`;
}
// SITREP group (enabled + interval): the SITREP is its own LLM call on the sitrep role's slot
function SitrepSettings({ f, setG }) {
  if (!f._lat?.snap || !("sitrep" in f._lat.snap)) return "";
  return html`<h3 class="cfgh">${t("cfg.l.sitrep")}</h3>
    <div class="pair"><label class="switch"><input type="checkbox" checked=${f.sitrep.enabled} onChange=${(e) => setG("sitrep", "enabled", e.target.checked)} /><span>${t("cfg.l.sitrepOn")}</span></label>
      <label class="field"><span>${t("cfg.l.sitrepInt")}</span><input type="number" min="30" max="3600" value=${f.sitrep.intervalSec} onInput=${(e) => setG("sitrep", "intervalSec", e.target.value)} /></label></div>
    <small class="muted">${t("cfg.l.sitrepHelp")}</small>`;
}
const FbBadge = ({ x }) => (x?.fellBack ? html`<span class="badge warn" title=${t("fb.tip", x.primaryError ?? "?")}>${t("fb.badge")}</span>` : "");
function Config({ st, toast, refresh }) {
  const [f, setF] = useState(null);
  const [keyMeta, setKeyMeta] = useState({ set: false, last4: "" });
  const [busy, setBusy] = useState(false);
  const [test, setTest] = useState(null);
  const [testTr, setTestTr] = useState(null);
  const [weakKeyMeta, setWeakKeyMeta] = useState({ set: false, last4: "" });
  const [fbKeyMeta, setFbKeyMeta] = useState({ strong: { set: false, last4: "" }, weak: { set: false, last4: "" } });
  const [fbTest, setFbTest] = useState({});
  const load = useCallback(async () => {
    try {
      const c = await (await fetch("/api/config", { cache: "no-store" })).json();
      setF({ provider: c.provider, model: c.model, baseUrl: c.baseUrl, apiKey: "", timeoutSec: c.timeoutSec, maxTokens: c.maxTokens, enabled: !!c.enabled, engine: c.engine ?? "llm", minIntervalSec: c.minIntervalSec ?? 0, skipUnchanged: c.skipUnchanged !== false, stopAtLimit: c.stopAtLimit !== false, wakeup: { ...c.wakeup }, triggers: { ...c.triggers }, memory: { ...c.memory },
        fallback: fbForm(c.fallback),
        weak: { provider: c.weak?.provider ?? "none", model: c.weak?.model ?? "", baseUrl: c.weak?.baseUrl ?? "", apiKey: "", timeoutSec: c.weak?.timeoutSec ?? 30, fallback: fbForm(c.weak?.fallback) },
        i18n: { enabled: c.i18n?.enabled !== false, translateTimeoutSec: c.i18n?.translateTimeoutSec ?? 10 }, ...latInit(c) });
      setKeyMeta({ set: c.apiKeySet, last4: c.apiKeyLast4 });
      setWeakKeyMeta({ set: !!c.weak?.apiKeySet, last4: c.weak?.apiKeyLast4 ?? "" });
      setFbKeyMeta({ strong: { set: !!c.fallback?.apiKeySet, last4: c.fallback?.apiKeyLast4 ?? "" }, weak: { set: !!c.weak?.fallback?.apiKeySet, last4: c.weak?.fallback?.apiKeyLast4 ?? "" } });
    } catch (e) { toast(t("cfg.loadFail", e.message), "err"); }
  }, []);
  useEffect(() => { load(); }, [load]);
  if (!f) return html`<main><div class="empty">${t("common.loading")}</div></main>`;
  const set = (k, v) => setF((o) => ({ ...o, [k]: v }));
  const setG = (g, k, v) => setF((o) => ({ ...o, [g]: { ...o[g], [k]: v } }));
  const numG = (g) => Object.fromEntries(Object.entries(f[g]).map(([k, v]) => [k, typeof v === "boolean" ? v : Number(v)]));
  // number input / toggle bound to a group setting
  const gnum = (g, k, label, min, max) => html`<label class="field"><span>${label}</span><input type="number" min=${min} max=${max} value=${f[g][k]} onInput=${(e) => setG(g, k, e.target.value)} /></label>`;
  const gsw = (g, k, label) => html`<label class="switch"><input type="checkbox" checked=${f[g][k]} onChange=${(e) => setG(g, k, e.target.checked)} /><span>${label}</span></label>`;
  const body = () => {
    const b = { provider: f.provider, model: f.model, baseUrl: f.baseUrl, timeoutSec: Number(f.timeoutSec), maxTokens: Number(f.maxTokens), enabled: f.enabled, engine: f.engine, minIntervalSec: Number(f.minIntervalSec), skipUnchanged: f.skipUnchanged, stopAtLimit: f.stopAtLimit, wakeup: numG("wakeup"), triggers: numG("triggers"), memory: numG("memory") };
    if (f.apiKey) b.apiKey = f.apiKey;
    b.fallback = fbBody(f.fallback);
    if (f.weak.provider === "none") b.weak = null; // no weak slot: the strong LLM translates
    else { b.weak = { provider: f.weak.provider, model: f.weak.model, baseUrl: f.weak.baseUrl, timeoutSec: Number(f.weak.timeoutSec), fallback: fbBody(f.weak.fallback) }; if (f.weak.apiKey) b.weak.apiKey = f.weak.apiKey; }
    b.i18n = { enabled: f.i18n.enabled, translateTimeoutSec: Number(f.i18n.translateTimeoutSec) };
    latBody(f, b);
    return b;
  };
  const save = async (e) => {
    e.preventDefault(); setBusy(true);
    try {
      const { ok, j } = await postJson("/api/config", body());
      if (!ok) toast(t("cfg.notSaved", j.error ?? t("err.request")), "err");
      else { toast(j.saved ? t("cfg.saved") : t("cfg.appliedNoFile"), j.saved ? "ok" : "err"); await load(); refresh(); }
    } catch (err) { toast(t("err.generic", err.message), "err"); }
    setBusy(false);
  };
  const doTestTr = async () => {
    setBusy(true); setTestTr({ pending: true });
    try {
      const { ok, j } = await postJson("/api/test", { ...body(), slot: "translate" });
      setTestTr(ok ? j : { ok: false, error: j.error ?? t("err.request") });
      toast(ok && j.ok ? t("cfg.trTestOk", j.latencyMs) : t("cfg.trTestFailed", j.error ?? t("err.request")), ok && j.ok ? "ok" : "err");
    } catch (err) { setTestTr({ ok: false, error: err.message }); toast(t("err.generic", err.message), "err"); }
    setBusy(false);
  };
  const doTestFb = async (slot) => {
    setBusy(true); setFbTest((o) => ({ ...o, [slot]: { pending: true } }));
    try {
      const { ok, j } = await postJson("/api/test", { ...body(), slot, which: "fallback" });
      setFbTest((o) => ({ ...o, [slot]: ok ? j : { ok: false, error: j.error ?? t("err.request") } }));
      toast(ok && j.ok ? t("fb.testOk", j.latencyMs) : t("fb.testFailed", j.error ?? t("err.request")), ok && j.ok ? "ok" : "err");
    } catch (err) { setFbTest((o) => ({ ...o, [slot]: { ok: false, error: err.message } })); toast(t("err.generic", err.message), "err"); }
    setBusy(false);
  };
  const weakOn = f.weak.provider !== "none", setW = (k, v) => setG("weak", k, v), setI = (k, v) => setG("i18n", k, v);
  const doTest = async () => {
    setBusy(true); setTest({ pending: true });
    try {
      const { ok, j } = await postJson("/api/test", body());
      setTest(ok ? j : { ok: false, error: j.error ?? t("err.request") });
      toast(ok && j.ok ? t("cfg.connOk", j.latencyMs) : t("cfg.testFailed", j.error), ok && j.ok ? "ok" : "err");
    } catch (err) { setTest({ ok: false, error: err.message }); toast(t("err.generic", err.message), "err"); }
    setBusy(false);
  };
  return html`<main class="view"><div class="cfgwrap">${st.usage?.enabled && html`<${UsageCard} u=${st.usage} now=${st.now} toast=${toast} refresh=${refresh} />`}<section class="panel" style=${{ marginTop: "12px" }}><h2>${t("cfg.title")} <span class="sub">${t("cfg.sub")}</span></h2>
    <form class="cfgbody" onSubmit=${save} autocomplete="off">
      <label class="field"><span>${t("hq.engineLabel")}</span><select value=${f.engine} onChange=${(e) => set("engine", e.target.value)}>
        <option value="llm">${t("cfg.opt.llm")}</option>
        <option value="rules">${t("cfg.opt.rules")}</option>
        <option value="model" disabled>${t("hq.modelSoon")}</option></select>
        <small>${t("cfg.engineHelp")}</small></label>
      <h3 class="cfgh">${t("cfg.strong")}</h3>
      <small class="muted">${t("cfg.strongNote")}</small>
      <label class="field"><span>${t("cfg.provider")}</span><select value=${f.provider} onChange=${(e) => set("provider", e.target.value)}>
        <option value="fake">${t("cfg.opt.fake")}</option>
        <option value="anthropic">anthropic</option>
        <option value="openai-compatible">${t("cfg.opt.openai")}</option>
        <option value="openrouter">${t("cfg.opt.openrouter")}</option></select></label>
      <label class="field"><span>${t("cfg.model")}</span><input type="text" value=${f.model} onInput=${(e) => set("model", e.target.value)} /></label>
      <label class="field"><span>${t("cfg.baseUrl")}</span><input type="text" value=${f.baseUrl} placeholder=${f.provider === "openrouter" ? t("cfg.baseUrlPhOr") : t("cfg.baseUrlPhOai")} onInput=${(e) => set("baseUrl", e.target.value)} /></label>
      <label class="field"><span>${t("cfg.apiKey")}</span><input type="password" autocomplete="new-password" value=${f.apiKey} placeholder=${keyMeta.set ? t("cfg.keyStored", keyMeta.last4) : t("cfg.keyNotSet")} onInput=${(e) => set("apiKey", e.target.value)} />
        <small>${t("cfg.keyNote")}</small></label>
      <${BackupBox} v=${f.fallback} setV=${(k, x) => setG("fallback", k, x)} meta=${fbKeyMeta.strong} busy=${busy} onTest=${() => doTestFb("strong")} res=${fbTest.strong} />
      <div class="pair">
        <label class="field"><span>${t("cfg.timeout")}</span><input type="number" min="1" max="300" value=${f.timeoutSec} onInput=${(e) => set("timeoutSec", e.target.value)} /></label>
        <label class="field"><span>${t("cfg.maxTokens")}</span><input type="number" min="16" max="32000" value=${f.maxTokens} onInput=${(e) => set("maxTokens", e.target.value)} /></label></div>
      <div class="pair">
        <label class="field"><span>${t("cfg.minInterval")}</span><input type="number" min="0" max="600" value=${f.minIntervalSec} onInput=${(e) => set("minIntervalSec", e.target.value)} /></label>
        <label class="switch"><input type="checkbox" checked=${f.skipUnchanged} onChange=${(e) => set("skipUnchanged", e.target.checked)} /><span>${t("cfg.skipUnchanged")}</span></label></div>
      <small class="muted">${t("cfg.batchNote")}</small>
      <h3 class="cfgh">${t("cfg.weak")}</h3>
      <small class="muted">${t("cfg.weakNote")}</small>
      <label class="field"><span>${t("cfg.provider")}</span><select value=${f.weak.provider} onChange=${(e) => setW("provider", e.target.value)}>
        <option value="none">${t("cfg.opt.none")}</option>
        <option value="fake">${t("cfg.opt.fake")}</option>
        <option value="anthropic">anthropic</option>
        <option value="openai-compatible">${t("cfg.opt.openai")}</option>
        <option value="openrouter">${t("cfg.opt.openrouter")}</option></select></label>
      ${weakOn && html`<label class="field"><span>${t("cfg.model")}</span><input type="text" value=${f.weak.model} onInput=${(e) => setW("model", e.target.value)} /></label>
      <label class="field"><span>${t("cfg.baseUrl")}</span><input type="text" value=${f.weak.baseUrl} placeholder=${f.weak.provider === "openrouter" ? t("cfg.baseUrlPhOr") : t("cfg.baseUrlPhOai")} onInput=${(e) => setW("baseUrl", e.target.value)} /></label>
      <label class="field"><span>${t("cfg.apiKey")}</span><input type="password" autocomplete="new-password" value=${f.weak.apiKey} placeholder=${weakKeyMeta.set ? t("cfg.keyStored", weakKeyMeta.last4) : t("cfg.keyNotSet")} onInput=${(e) => setW("apiKey", e.target.value)} />
        <small>${t("cfg.keyNote")}</small></label>
      <label class="field"><span>${t("cfg.timeout")}</span><input type="number" min="1" max="300" value=${f.weak.timeoutSec} onInput=${(e) => setW("timeoutSec", e.target.value)} /></label>
      <${BackupBox} v=${f.weak.fallback} setV=${(k, x) => setF((o) => ({ ...o, weak: { ...o.weak, fallback: { ...o.weak.fallback, [k]: x } } }))} meta=${fbKeyMeta.weak} busy=${busy} onTest=${() => doTestFb("weak")} res=${fbTest.weak} />`}
      <h3 class="cfgh">${t("cfg.i18n")}</h3>
      <label class="switch"><input type="checkbox" checked=${f.i18n.enabled} onChange=${(e) => setI("enabled", e.target.checked)} /><span>${t("cfg.i18nEnabled")} <span class="muted">${t("cfg.i18nEnabledNote")}</span></span></label>
      <label class="field"><span>${t("cfg.trTimeout")}</span><input type="number" min="1" max="120" value=${f.i18n.translateTimeoutSec} onInput=${(e) => setI("translateTimeoutSec", e.target.value)} /></label>
      <div class="muted">${t("cfg.trUses")} <span class=${"badge " + (weakOn ? "info" : "")}>${weakOn ? t("cfg.trWeak") : t("cfg.trStrong")}</span></div>
      <div class="btns"><button class="btn ghost" type="button" disabled=${busy} onClick=${doTestTr}><${Icon} n="plug" /> ${t("cfg.testTr")}</button></div>
      ${testTr && html`<pre>${testTr.pending ? t("cfg.testing") : J(testTr)}</pre>`}
      <h3 class="cfgh">${t("cfg.wakeup")}</h3>
      ${gsw("wakeup", "dynamic", t("cfg.w.dynamic"))}
      <div class="pair">${gnum("wakeup", "defaultSec", t("cfg.w.default"), 5, 3600)}${gnum("wakeup", "minSec", t("cfg.w.min"), 5, 3600)}</div>
      <div class="pair">${gnum("wakeup", "maxSec", t("cfg.w.max"), 5, 7200)}${gsw("wakeup", "budgetStretch", t("cfg.w.stretch"))}</div>
      <small class="muted">${t("cfg.wakeNote")}</small>
      <h3 class="cfgh">${t("cfg.triggers")}</h3>
      ${gsw("triggers", "enabled", t("cfg.tr.enabled"))}
      <div class="pair">${gsw("triggers", "newContact", t("cfg.tr.newContact"))}${gsw("triggers", "squadLost", t("cfg.tr.squadLost"))}</div>
      <div class="pair">${gsw("triggers", "objectiveReached", t("cfg.tr.objectiveReached"))}${gsw("triggers", "squadChange", t("cfg.tr.squadChange"))}</div>
      <div class="pair">${gnum("triggers", "contactNearSquadM", t("cfg.tr.contactNear"), 0, 5000)}${gnum("triggers", "casualties", t("cfg.tr.casualties"), 0, 1000)}</div>
      <div class="pair">${gnum("triggers", "strengthDropPct", t("cfg.tr.strengthDrop"), 0, 100)}${gnum("triggers", "cooldownSec", t("cfg.tr.cooldown"), 0, 600)}</div>
      <div class="pair">${gsw("triggers", "objectiveChanged", t("cfg.tr.objectiveChanged"))}${gsw("triggers", "unitSpawned", t("cfg.tr.unitSpawned"))}</div>
      <div class="pair">${gsw("triggers", "missionEvent", t("cfg.tr.missionEvent"))}${gnum("triggers", "ticketsDropPct", t("cfg.tr.ticketsDrop"), 0, 100)}</div>
      <small class="muted">${t("cfg.trNote")}</small>
      <${SitrepSettings} f=${f} setG=${setG} />
      <${LatencySettings} f=${f} setG=${setG} />
      <${MemorySettings} f=${f} setG=${setG} />
      ${f.provider === "openrouter" && html`<label class="switch"><input type="checkbox" checked=${f.stopAtLimit} onChange=${(e) => set("stopAtLimit", e.target.checked)} />
        <span>${t("cfg.stopAtLimit")} <span class="muted">${t("cfg.stopAtLimitNote")}</span></span></label>`}
      <label class="switch"><input type="checkbox" checked=${f.enabled} onChange=${(e) => set("enabled", e.target.checked)} />
        <span>${t("cfg.enabled")} <span class="muted">${t("cfg.enabledNote")}</span></span></label>
      <div class="btns"><button class="btn" type="submit" disabled=${busy}><${Icon} n="save" /> ${t("common.save")}</button>
        <button class="btn ghost" type="button" disabled=${busy} onClick=${doTest}><${Icon} n="plug" /> ${t("cfg.test")}</button></div>
      ${test && html`<pre>${test.pending ? t("cfg.testing") : J(test)}</pre>`}
    </form></section>
    <${AccountCard} toast=${toast} cfg=${st.config} defaultPassword=${!!st.auth?.defaultPassword} refresh=${refresh} />
    <${MapCard} toast=${toast} />
    </div></main>`;
}

// ---------- OpenRouter usage card ----------
const usd = (v) => (num(v) ? "$" + (Math.abs(v) < 10 ? v.toFixed(2) : v.toFixed(1)) : "-");
const usd4 = (v) => (num(v) ? "$" + v.toFixed(4) : "-");
const PERIOD = { daily: ["usage_daily", "use.today"], weekly: ["usage_weekly", "use.thisWeek"], monthly: ["usage_monthly", "use.thisMonth"] };
function UsageCard({ u, now, toast, refresh }) {
  const [busy, setBusy] = useState(false);
  if (!u?.enabled) return null;
  const k = u.key, d = k?.data, s = u.session ?? {}, b = u.budget;
  const doRefresh = async () => {
    setBusy(true);
    try { const { ok, j } = await postJson("/api/usage/refresh", {}); toast(ok && j.ok ? t("use.refreshed") : t("use.refreshFailed", j.usage?.key?.error ?? j.error ?? t("err.request")), ok && j.ok ? "ok" : "err"); refresh(); }
    catch (err) { toast(t("err.generic", err.message), "err"); }
    setBusy(false);
  };
  // bar: period usage vs limit, or all-time usage vs limit
  let bar = null, numbers = null;
  if (d && num(d.limit) && d.limit > 0) {
    const per = PERIOD[d.limit_reset], used = per ? d[per[0]] : d.usage;
    const est = (used ?? 0) + (k.costSincePoll ?? 0), pct = Math.min(100, (100 * est) / d.limit);
    bar = { pct, label: `${per ? t(per[1]) : t("use.total")} ${usd(est)} / ${usd(d.limit)} (${per ? t("use.resets", tk("use.reset", d.limit_reset)) : t("use.noReset")})` };
  } else if (d) {
    numbers = [[t("use.today"), d.usage_daily], [t("use.week"), d.usage_weekly], [t("use.month"), d.usage_monthly], [t("use.total"), d.usage]];
  }
  const pc = u.pace, dur = (ms) => { const m = Math.max(0, Math.round(ms / 60000)), dd = Math.floor(m / 1440), hh = Math.floor((m % 1440) / 60); return dd ? t("time.dh", dd, hh) : hh ? t("time.hm", hh, m % 60) : t("time.m", m); };
  let chip = null, mark = null;
  if (pc?.kind === "period" && d) {
    mark = Math.min(100, (100 * pc.expected) / d.limit);
    const rs = ` · ${t("use.resetsIn", dur(pc.resetsInMs))}`;
    if (pc.projected === null) chip = { cls: "", txt: t("use.noData") + rs };
    else {
      const r = pc.projected / d.limit, cls = r > 1 ? "bad" : r >= 0.9 ? "warn" : "ok";
      chip = { cls, txt: pc.delta > 0 ? t("use.over", usd(pc.delta), usd(pc.projected), usd(d.limit), rs) : t("use.under", usd(-pc.delta), usd(pc.projected), usd(d.limit), rs) };
    }
  } else if (pc?.kind === "rate") {
    chip = { cls: "", txt: pc.ratePerHour === null ? t("use.noData") : `${usd(pc.ratePerHour)}/h${pc.hoursToLimit ? ", " + t("use.limitIn", pc.hoursToLimit < 10 ? pc.hoursToLimit.toFixed(1) : Math.round(pc.hoursToLimit)) : ""}` };
  }
  const col = !bar ? "" : bar.pct >= 95 ? "var(--bad)" : bar.pct >= 80 ? "var(--warn)" : "var(--ok)";
  const models = Object.entries(s.byModel ?? {}).sort((a, c) => c[1].cost - a[1].cost);
  const facs = Object.entries(s.byFaction ?? {}).sort((a, c) => c[1].cost - a[1].cost);
  const polled = k?.ts ? t("time.agoS", Math.round(((now ?? Date.now()) - k.ts) / 1000)) : t("time.never");
  return html`<section class="panel"><h2>${t("use.title")} <span class="sub">OpenRouter${d?.label ? " " + t("use.key", d.label) : ""}</span></h2>
    <div class="cfgbody">
      ${b && b.level !== "ok" && html`<div class=${"banner " + (b.level === "warn" ? "warn" : "bad")}>${b.level === "reached"
        ? t("use.reached", usd(b.limit)) + (u.stopAtLimit ? " " + t("use.stopped") : "")
        : t("use.pctUsed", b.pct.toFixed(0), usd(b.remaining)) + (b.pace?.delta > 0 ? " " + t("use.overshoot", usd(b.pace.delta)) : "")}</div>`}
      ${k?.error && html`<div class="banner bad">${t("use.pollFailed", k.error)}</div>`}
      ${!k?.keySet && html`<div class="muted">${t("use.noKey")}</div>`}
      ${!bar && chip && html`<div class=${"upace " + chip.cls}>${chip.txt}</div>`}
      ${bar && html`<div><div class="ubar"><i style=${{ width: bar.pct + "%", background: col }}></i>${mark !== null && html`<span class="umark" style=${{ left: mark + "%" }} title=${t("use.paceMarker")}></span>`}</div><div class="ulabel">${bar.label}${chip && html` <span class=${"upace " + chip.cls}>${chip.txt}</span>`}${num(d.limit_remaining) ? html` <span class="muted">, ${t("use.left", usd(d.limit_remaining - (k.costSincePoll ?? 0)))}</span>` : ""}</div></div>`}
      ${numbers && html`<div><div class="unums">${numbers.map(([l, v]) => html`<div><span class="muted">${l}</span><b>${usd(v)}</b></div>`)}</div><div class="muted">${t("use.noLimit")}</div></div>`}
      <div class="unums">
        <div><span class="muted">${t("use.sessionCost")}</span><b>${usd4(s.cost)}</b></div>
        <div><span class="muted">${t("strip.requestsCap")}</span><b>${s.requests ?? 0}</b></div>
        <div><span class="muted">${t("use.tokensInOut")}</span><b>${s.tokensIn ?? 0} / ${s.tokensOut ?? 0}</b></div></div>
      ${models.length > 0 && html`<table><thead><tr><th>${t("cfg.model")}</th><th>${t("strip.requestsCap")}</th><th>${t("col.cost")}</th></tr></thead><tbody>${models.map(([m, e]) => html`<tr><td>${m}</td><td>${e.requests}</td><td class="mono">${usd4(e.cost)}</td></tr>`)}</tbody></table>`}
      ${facs.length > 0 && html`<table><thead><tr><th>${t("use.factionUnit")}</th><th>${t("strip.requestsCap")}</th><th>${t("col.cost")}</th></tr></thead><tbody>${facs.map(([m, e]) => html`<tr><td>${m}</td><td>${e.requests}</td><td class="mono">${usd4(e.cost)}</td></tr>`)}</tbody></table>`}
      <div class="btns"><button class="btn ghost" type="button" disabled=${busy || !k?.keySet} onClick=${doRefresh}><${Icon} n="reset" /> ${t("use.refresh")}</button>
        <span class="muted" style=${{ alignSelf: "center" }}>${t("use.lastPoll", polled)}</span></div>
      <small class="muted">${t("use.keyAdvice")}</small>
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
  const [, bumpLang] = useState(0); // re-render everything when the dashboard language changes
  useEffect(() => onLang(() => bumpLang((n) => n + 1)), []);
  useEffect(() => { const h = () => { const t = tabOf(location.hash); if (t) setTab(t); }; window.addEventListener("hashchange", h); return () => window.removeEventListener("hashchange", h); }, []);
  useEffect(() => { try { localStorage.setItem(TAB_KEY, tab); } catch { /* storage unavailable */ } }, [tab]);
  useEffect(() => { try { localStorage.setItem(INACT_KEY, showInactive ? "1" : "0"); } catch { /* storage unavailable */ } }, [showInactive]);
  useEffect(() => { try { localStorage.setItem(LAYER_KEY, JSON.stringify(layers)); } catch { /* storage unavailable */ } }, [layers]);
  const names = sortedKeys(st.factions);
  const fac = faction === "all" || names.includes(faction) ? faction : "all"; // a selected faction that became inactive falls back to all
  const rows = useMemo(() => buildRows(st, fac), [st, fac]);
  const go = (id) => (e) => { e.preventDefault(); setTab(id); try { history.replaceState(null, "", "#" + id); } catch { location.hash = id; } };
  const logout = async () => { try { await rawFetch("/api/logout", { method: "POST", headers: { "Content-Type": "application/json" }, body: "{}" }); } catch { /* offline */ } location.href = "/login"; };
  const common = { st, faction: fac, rows, sel, onSel: setSel, act, setAct };
  return html`<div>
    <header class="topbar">
      <div class="brand"><span class="logo"><${Icon} n="logo" /></span>Clausewitz HQ</div>
      <nav>${TABS.map(([id, label]) => html`<a key=${id} href=${"#" + id} class=${tab === id ? "active" : ""} onClick=${go(id)}>${t(label)}${id === "loadouts" && st.loadouts?.bad > 0 ? html` <span class="badge bad" title=${t("lo.badBanner", st.loadouts.bad)}>${st.loadouts.bad}</span>` : ""}</a>`)}</nav>
      <${Strip} st=${st} toast=${toast} refresh=${refresh} />
      <button class="btn ghost mini" type="button" onClick=${logout}>${t("auth.logout")}</button>
      <select class="langsel" value=${getLang()} onChange=${(e) => setLang(e.target.value)} title=${t("lang.title")} aria-label=${t("lang.title")}>${LANGS.map((l) => html`<option key=${l} value=${l}>${l.toUpperCase()}</option>`)}</select>
    </header>
    ${tab !== "usage" && tab !== "debug" && tab !== "actions" && tab !== "loadouts" && tab !== "latency" && html`<div class="subwrap"><${FactionBar} names=${names} faction=${fac} setFaction=${setFaction} hidden=${st.hidden ?? []} showInactive=${showInactive} setShowInactive=${setShowInactive} /></div>`}
    ${st.auth?.defaultPassword && html`<div class="subwrap"><div class="banner warn">${t("auth.defaultBanner")} <a href="#usage" onClick=${go("usage")}>${t("auth.defaultGo")}</a></div></div>`}
    ${tab === "overview" && html`<${Overview} ...${common} />`}
    ${tab === "map" && html`<${MapView} ...${common} layers=${layers} setLayers=${setLayers} world=${world} />`}
    ${tab === "forces" && html`<${ForcesView} ...${common} />`}
    ${tab === "hq" && html`<${HqView} ...${common} />`}
    ${tab === "journal" && html`<${JournalView} st=${st} faction=${fac} facClass=${facClass} />`}
    ${tab === "missions" && html`<${MissionsView} ...${common} layers=${layers} world=${world} />`}
    ${tab === "requests" && html`<${RequestsView} st=${st} faction=${fac} />`}
    ${tab === "hqchat" && html`<${HqChatView} st=${st} faction=${fac} />`}
    ${tab === "actions" && html`<${ActionsView} st=${st} />`}
    ${tab === "loadouts" && html`<${LoadoutsView} toast=${toast} />`}
    ${tab === "latency" && html`<${LatencyView} st=${st} />`}
    ${tab === "debug" && html`<${DebugView} st=${st} />`}
    ${tab === "usage" && html`<${Config} st=${st} toast=${toast} refresh=${refresh} />`}
    <div class="toasts">${toasts.map((t) => html`<div class=${"toast " + t.kind} key=${t.id}>${t.text}</div>`)}</div>
  </div>`;
}
render(html`<${App} />`, document.getElementById("app"));
