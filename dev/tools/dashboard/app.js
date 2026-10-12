// Clausewitz HQ dashboard. Preact + htm (vendored, offline). All game text is rendered through Preact
// (auto-escaped); no dangerouslySetInnerHTML anywhere.
import { html, render, useState, useEffect, useRef, useMemo, useCallback } from "/vendor/standalone.module.js";

const WORLD = 4096, GRID = 512, STALE_MS = 30000; // WORLD = fallback extent until /api/world reports the real one
const DEF_EXT = { x0: 0, z0: 0, x1: WORLD, z1: WORLD };
// Faction colours are derived from the sorted faction keys (no hard-wired names): first blue, second red, then a palette; unowned = grey.
const PALETTE = ["#4aa3ff", "#ff5c5c", "#f0b429", "#3fcf8e", "#b57bff", "#ff8fd1"];
const NEUTRAL = "#8793a6";
let FACTIONS = []; // sorted faction keys seen in the state (set on every poll by setFactions)
function setFactions(st) {
  const set = new Set(Object.keys(st.factions ?? {}));
  for (const e of Object.values(st.factions ?? {})) {
    const sit = e.situation ?? {};
    for (const b of Array.isArray(sit.bases) ? sit.bases : []) if (typeof b?.faction === "string") set.add(b.faction);
    for (const o of Array.isArray(sit.objectives) ? sit.objectives : []) if (typeof o?.owner === "string" && o.owner !== "none" && o.owner !== "") set.add(o.owner);
  }
  FACTIONS = [...set].sort();
}
const facIdx = (f) => FACTIONS.indexOf(f);
const colorOf = (f) => { const i = facIdx(f); return i < 0 ? NEUTRAL : PALETTE[i % PALETTE.length]; };
const enemyColor = (f) => { const o = FACTIONS.find((x) => x !== f); return o === undefined ? "#f0b429" : colorOf(o); };
const facClass = (f) => { const i = facIdx(f); return i < 0 ? "f-none" : "f-c" + (i % PALETTE.length); };
const KIND_LABEL = { infantry: "INF", recon: "RECON", motorized: "MOT", light: "LIGHT", mech: "MECH", armor: "ARMOR", armored_recon: "A-RECON" };
const kindLabel = (k) => (typeof k === "string" && k ? KIND_LABEL[k] ?? k.toUpperCase().slice(0, 8) : "-");
const fmtTime = (sec) => (num(sec) ? `${Math.floor(sec / 60)}:${String(Math.floor(sec % 60)).padStart(2, "0")}` : "-");
// public picture (objectives, bases, war) from the freshest v2 situation of any faction
function publicPicture(st) {
  let objs = [], bases = [], ts = -1;
  for (const e of Object.values(st.factions ?? {})) {
    const sit = e.situation ?? {};
    if (Array.isArray(sit.objectives) && sit.objectives.length && (e.ts ?? 0) >= ts) { objs = sit.objectives; ts = e.ts ?? 0; }
  }
  const seen = new Set();
  for (const e of Object.values(st.factions ?? {})) for (const b of Array.isArray(e.situation?.bases) ? e.situation.bases : []) {
    if (b && typeof b.faction === "string" && Array.isArray(b.pos) && num(b.pos[0]) && num(b.pos[1]) && !seen.has(b.faction)) { seen.add(b.faction); bases.push(b); }
  }
  return { objs: objs.filter((o) => o && Array.isArray(o.pos) && num(o.pos[0]) && num(o.pos[1])), bases };
}
const num = (v) => typeof v === "number" && Number.isFinite(v);
const okPos = (p) => Array.isArray(p) && p.length >= 3 && num(p[0]) && num(p[2]);
const gridTxt = (p) => (okPos(p) ? `${Math.round(p[0])}, ${Math.round(p[2])}` : "-");
const shortId = (id) => String(id).replace(/^CLW_/i, "").replace(/Squad_?/i, "");
const dash = (v) => (v === undefined || v === null || v === "" ? "-" : v);

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
function useState2() {
  const [st, setSt] = useState({ offline: true, factions: {}, exchanges: [], counters: {}, config: {} });
  const poll = useCallback(async () => {
    try {
      const r = await fetch("/api/state", { cache: "no-store" });
      const j = await r.json();
      setFactions(j);
      setSt({ ...j, offline: false, recvAt: Date.now() });
    } catch { setSt((s) => ({ ...s, offline: true })); }
  }, []);
  useEffect(() => { poll(); const t = setInterval(poll, 2000); return () => clearInterval(t); }, [poll]);
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
    ${cf.enabled !== false && Object.keys(st.scheduler ?? {}).sort().map((n) => html`<${WakeChip} key=${n} name=${n} s=${st.scheduler[n]} recvAt=${st.recvAt} toast=${toast} refresh=${refresh} paused=${!!st.paused} engine=${cf.engine} />`)}
    <${HqControls} st=${st} toast=${toast} refresh=${refresh} />
    ${(st.paused || (cf.engine && cf.engine !== "llm")) && html`<div class="chip hqnote">HQ engine: <b>${cf.engine && cf.engine !== "llm" ? cf.engine : "llm"}</b>${st.paused ? " (paused)" : ""}, no LLM calls</div>`}
    <div class=${"chip" + (c.errors ? " bad" : "")}>errors <b>${c.errors ?? 0}</b></div>
    <div class="chip">tokens in/out <b>${c.inputTokens ?? 0} / ${c.outputTokens ?? 0}</b></div>
    ${bud && bud.level !== "ok" && html`<div class=${"chip " + (bud.level === "warn" ? "warn" : "bad")} title="OpenRouter key budget (see Configuration)"><span class="dot"></span>${bud.level === "reached" ? "budget limit reached" : `budget ${bud.pct.toFixed(0)}% used`}</div>`}
    <div class=${"chip " + (stale ? "bad" : "ok")} title="Red when the game has not contacted the sidecar for 30 s"><span class="dot"></span>last request <b>${ageTxt}</b></div>
  </div>`;
}

// ---------- map (canvas renderer with pan/zoom, tooltips, layers) ----------
function MapPanel({ st, faction, rows, sel, onSel, layers, world }) {
  const box = useRef(), cv = useRef();
  const view = useRef({ s: 0.1, ox: 0, oy: 0, fitted: false });
  const hits = useRef([]);
  const drag = useRef(null);
  const latest = useRef({});
  const [tip, setTip] = useState(null);
  latest.current = { st, faction, sel, layers, world };
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
    const { st, faction, sel, layers, world } = latest.current, v = view.current;
    const E = world.extent ?? DEF_EXT, EW = E.x1 - E.x0, EH = E.z1 - E.z0;
    const r = c.getBoundingClientRect(), dpr = window.devicePixelRatio || 1;
    if (!r.width) return;
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
      const squads = (Array.isArray(sit.squads) ? sit.squads : []).filter((s) => s && okPos(s.pos));
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
        const x = sx(s.pos[0]), y = sy(s.pos[2]), key = f + "/" + s.id, col = colorOf(f);
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
        ctx.fillStyle = col; ctx.shadowColor = col; ctx.shadowBlur = 10;
        kindShape(ctx, s.kind, x, y); ctx.fill(); ctx.shadowBlur = 0;
        if (s.humanLed) { ctx.strokeStyle = "#fff"; ctx.lineWidth = 2; ctx.stroke(); }
        const veh = s.vehicle && typeof s.vehicle === "object" ? s.vehicle : null;
        if (veh && veh.mounted === false) { ctx.strokeStyle = "#0a0d12"; ctx.lineWidth = 2; ctx.beginPath(); ctx.moveTo(x - 4, y); ctx.lineTo(x + 4, y); ctx.stroke(); } // dismounted: bar across the symbol
        if (layers.labels) {
          ctx.fillStyle = "#dde3ec";
          ctx.font = "600 11px system-ui, sans-serif"; ctx.fillText(shortId(s.id) + (s.humanLed ? " ★" : ""), x + 9, y - 11);
          if (s.kind || veh) { ctx.font = "9px system-ui, sans-serif"; ctx.fillStyle = "rgba(221,227,236,.7)"; ctx.fillText([kindLabel(s.kind), veh?.type, veh ? (veh.mounted ? "mounted" : "dismounted") : null].filter(Boolean).join(" "), x + 9, y + 13); }
          ctx.font = "11px system-ui, sans-serif";
        }
        const oc = s.objective?.command ?? (byOrder.get(s.id)?.command ?? "-"), ot = s.objective?.target ?? byOrder.get(s.id)?.target;
        h.push({ x, y, r: 11, key, title: String(s.id), lines: [`${f}, ${s.agents}/${s.startAgents} agents, ${s.mode ?? s.state ?? "-"}`, ...(s.kind ? [`kind ${s.kind}${s.type ? ": " + s.type : ""}`] : []),
          ...(veh ? [`vehicle ${dash(veh.type)}, ${veh.mounted ? "mounted" : "dismounted"}${veh.state ? ", " + veh.state : ""}${num(veh.health) ? ", hp " + Math.round(veh.health * 100) + "%" : ""}`] : []),
          `objective: ${oc}${ot ? " " + ot : ""}`, s.humanLed ? "human-led" + (s.mission?.state && s.mission.state !== "none" ? `, mission ${s.mission.state} ${s.mission.kind ?? ""} ${s.mission.target ?? ""}` : "") : "AI-led", `at ${gridTxt(s.pos)}`] });
      }
    }
    hits.current = h;
  }, []);

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
      const v = view.current, [mx, my] = local(e);
      const ns = Math.min(5, Math.max(0.02, v.s * (e.deltaY < 0 ? 1.2 : 1 / 1.2))), q = ns / v.s;
      v.ox = mx - (mx - v.ox) * q; v.oy = my - (my - v.oy) * q; v.s = ns; v.user = true;
    };
    c.addEventListener("wheel", onWheel, { passive: false });
    return () => c.removeEventListener("wheel", onWheel);
  }, []);

  const onDown = (e) => { drag.current = { x: e.clientX, y: e.clientY, moved: 0 }; cv.current.classList.add("grabbing"); };
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

  return html`<section class="panel">
    <h2>Map <span class="sub">${Math.round(ext.x1 - ext.x0)} x ${Math.round(ext.z1 - ext.z0)} m, x east / z north${world.map?.status === "resolving" ? ", loading map image..." : ""}</span></h2>
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

// ---------- squads table ----------
const COLS = [["id", "Squad"], ["faction", "Faction"], ["kind", "Kind"], ["type", "Type"], ["vehicle", "Vehicle"], ["size", "Size"], ["strength", "Strength"], ["wounded", "Wnd"], ["objective", "Objective"],
  ["state", "State"], ["movement", "Movement"], ["leader", "Leader"], ["position", "Position x, z"], ["order", "Last order"]];
const SORTKEY = { id: (r) => r.id, faction: (r) => r.faction, kind: (r) => r.kind, vehicle: (r) => r.vehicle?.type ?? "", type:(r) => String(r.type), size: (r) => r.agents ?? -1, strength: (r) => r.pct ?? -1, wounded: (r) => r.wounded ?? -1,
  objective: (r) => r.obj?.command ?? "", state: (r) => r.state, movement: (r) => r.speed, leader: (r) => (r.human ? 1 : 0), position: (r) => (okPos(r.pos) ? r.pos[0] : -1), order: (r) => r.orderAge ?? 1e9 };
const stateBadge = (s) => ({ dead: "bad", engaged: "bad", moving: "info", defending: "ok" }[s] ?? "");

function SquadsTable({ rows, sel, onSel }) {
  const [sort, setSort] = useState({ key: "id", dir: 1 });
  const sorted = useMemo(() => {
    const f = SORTKEY[sort.key] ?? SORTKEY.id;
    return [...rows].sort((a, b) => (f(a) > f(b) ? 1 : f(a) < f(b) ? -1 : 0) * sort.dir);
  }, [rows, sort]);
  useEffect(() => { if (sel) document.querySelector(`tr[data-key="${CSS.escape(sel)}"]`)?.scrollIntoView({ block: "nearest" }); }, [sel]);
  return html`<section class="panel squads">
    <h2>Squads <span class="sub">${rows.length} total, click header to sort, click row to highlight; '-' = not reported</span></h2>
    <div class="scroll"><table>
      <thead><tr>${COLS.map(([k, t]) => html`<th class="sortable" onClick=${() => setSort((s) => ({ key: k, dir: s.key === k ? -s.dir : 1 }))}>${t}${sort.key === k ? (sort.dir > 0 ? " ▲" : " ▼") : ""}</th>`)}</tr></thead>
      <tbody>${sorted.map((r) => {
        const col = r.pct === null ? "#5d697c" : r.pct >= 66 ? "var(--ok)" : r.pct >= 33 ? "var(--warn)" : "var(--bad)";
        const o = r.obj;
        return html`<tr key=${r.key} data-key=${r.key} class=${"sqrow" + (sel === r.key ? " sel" : "")} onClick=${() => onSel(sel === r.key ? null : r.key)}>
          <td><b>${r.id}</b></td><td class=${facClass(r.faction)}>${r.faction}</td>
          <td>${r.kind ? html`<span class=${"badge kind k-" + r.kind}>${kindLabel(r.kind)}</span>` : "-"}</td><td>${r.type}</td>
          <td>${r.vehicle ? html`${dash(r.vehicle.type)} <span class=${"badge " + (r.vehicle.mounted ? "info" : "warn")}>${r.vehicle.mounted ? "mounted" : "dismounted"}</span>${r.vehicle.state ? html` <span class="muted">${r.vehicle.state}</span>` : ""}${num(r.vehicle.health) ? html` <span class="muted">hp ${Math.round(r.vehicle.health * 100)}%</span>` : ""}` : "-"}</td><td class="mono">${r.size}</td>
          <td>${r.pct === null ? "-" : html`<span class="bar"><i style=${{ width: Math.min(100, r.pct) + "%", background: col }}></i></span>${r.pct}%${r.health !== null && html` <span class="muted">hp ${Math.round(r.health * 100)}%</span>`}`}</td>
          <td>${r.wounded === null ? "-" : r.wounded}</td>
          <td class="wrap">${o ? html`<span class=${"cmd " + o.command}>${o.command}</span> ${o.target ? html`<b>${o.target}</b>` : ""}${!o.target && Array.isArray(o.pos) && num(o.pos[0]) ? html`<span class="mono">${Math.round(o.pos[0])}, ${Math.round(o.pos[1])}</span>` : ""}${o.status ? html` <span class="badge info">${o.status}</span>` : ""} ${o.reason && html`<span class="muted">${o.reason}</span>`}` : "-"}</td>
          <td><span class=${"badge " + stateBadge(r.state)}>${r.state}</span></td><td class="mono">${r.move}</td>
          <td>${r.human ? html`<span class="badge human">human</span>${r.mission ? html` <span class="badge warn" title="mission state">${r.mission.state} ${r.mission.kind ?? ""} ${r.mission.target ?? ""}</span>` : ""}` : "AI"}</td><td class="mono">${r.posTxt}</td>
          <td>${r.orderAge === null ? "-" : r.orderAge + " s"}</td></tr>`;
      })}</tbody></table>
      ${!rows.length && html`<div class="empty">No squads reported yet.</div>`}</div>
  </section>`;
}

// ---------- war strip (protocol v2): tickets, supplies, caps per faction ----------
function WarStrip({ st, faction }) {
  const names = Object.keys(st.factions ?? {}).filter((f) => (faction === "all" || f === faction) && st.factions[f].situation?.war).sort();
  if (!names.length) return null;
  const cap = (v, c) => html`<b>${dash(v)}</b>${num(c) ? html`<span class="muted"> / ${c}</span>` : ""}`;
  return html`<div class="warstrip">${names.map((f) => {
    const sit = st.factions[f].situation, w = sit.war, col = colorOf(f);
    const tl = num(w.tickets) && num(w.enemyTickets) ? w.tickets / Math.max(1, w.tickets + w.enemyTickets) : null;
    return html`<div class="warcard" style=${{ borderTopColor: col }} key=${f}>
      <div class="wname"><b class=${facClass(f)}>${f}</b><span class="muted">time left ${fmtTime(sit.timeLeftSec)}</span></div>
      <div class="wgrid">
        <div title="War will: 0 = defeat. Enemy in brackets"><span class="muted">tickets</span>${cap(w.tickets)}${num(w.enemyTickets) ? html`<span class="muted"> (enemy ${w.enemyTickets})</span>` : ""}</div>
        <div title="Purchasing power; income per minute"><span class="muted">supplies</span><b>${dash(w.supplies)}</b>${num(w.income) ? html`<span class="muted"> +${w.income}/min</span>` : ""}</div>
        <div title="Live AI soldiers / cap"><span class="muted">AI</span>${cap(w.liveAI, w.aiCap)}</div>
        <div title="Vehicles / cap"><span class="muted">vehicles</span>${cap(w.vehicles, w.vehicleCap)}</div>
      </div>
      ${tl !== null && html`<div class="tbar" title="Share of the combined tickets"><i style=${{ width: Math.round(tl * 100) + "%", background: col }}></i></div>`}</div>`;
  })}</div>`;
}

// ---------- objectives + recent events ----------
function ObjectivesPanel({ st }) {
  const { objs } = publicPicture(st);
  if (!objs.length) return null;
  return html`<section class="panel side"><h2>Objectives <span class="sub">${objs.length} points</span></h2><div class="scroll"><table>
    <thead><tr><th>Id</th><th>Name</th><th>Value</th><th>Owner</th><th>Progress</th><th>Position x, z</th></tr></thead><tbody>
    ${objs.map((o) => html`<tr key=${o.id}><td><b>${o.id}</b></td><td>${dash(o.name)}</td><td>${dash(o.value)}</td>
      <td>${o.owner && o.owner !== "none" ? html`<b class=${facClass(o.owner)}>${o.owner}</b>` : html`<span class="dim">neutral</span>`}${o.contested ? html` <span class="badge warn">contested</span>` : ""}</td>
      <td class="mono">${num(o.progress) ? o.progress + " %" : "-"}</td><td class="mono">${Math.round(o.pos[0])}, ${Math.round(o.pos[1])}</td></tr>`)}
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
  const names = Object.keys(st.factions ?? {}).filter((f) => faction === "all" || f === faction).sort();
  return html`<section class="panel side"><h2>SITREP / contacts</h2><div class="scroll">
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

// ---------- HQ view ----------
const LAYER_KEY = "clw.mapLayers";
const LAYER_DEFAULTS = { image: true, roads: true, buildings: true, objects: true, places: true, grid: true, objectives: true, squads: true, contacts: true, orders: true, labels: true, flip: false, dim: 0.8 };
function loadLayers() {
  try { return { ...LAYER_DEFAULTS, ...JSON.parse(localStorage.getItem(LAYER_KEY) ?? "{}") }; } catch { return { ...LAYER_DEFAULTS }; }
}
function HQ({ st }) {
  const [faction, setFaction] = useState("all");
  const [sel, setSel] = useState(null);
  const [layers, setLayers] = useState(loadLayers);
  const world = useWorld();
  useEffect(() => { try { localStorage.setItem(LAYER_KEY, JSON.stringify(layers)); } catch { /* storage unavailable */ } }, [layers]);
  const names = Object.keys(st.factions ?? {}).sort();
  const rows = useMemo(() => buildRows(st, faction), [st, faction]);
  const tog = (k) => html`<label><input type="checkbox" checked=${layers[k]} onChange=${() => setLayers((l) => ({ ...l, [k]: !l[k] }))} /> ${k}</label>`;
  return html`<main>
    <div class="toolbar">
      <div class="seg">${["all", ...names].map((n) => html`<button class=${faction === n ? "on" : ""} onClick=${() => setFaction(n)}>${n}</button>`)}</div>
      <div class="toggles">${["grid", "objectives", "squads", "contacts", "orders", "labels"].map(tog)}</div>
      <div class="toggles" title="Map background: toggles, image opacity; flip = image y axis inverted (calibration)">
        ${["image", "roads", "buildings", "objects", "places", "flip"].map(tog)}
        <span class="muted" title="Objects key: diamond = base, triangle over a bar = airport, ring = port. Buildings: grey = house, large = bigger than 250 m2, orange = military / industrial">◆ base ▲ airport ◎ port</span>
        <label>dim <input type="range" min="0.1" max="1" step="0.05" value=${layers.dim} onInput=${(e) => setLayers((l) => ({ ...l, dim: Number(e.target.value) }))} style=${{ width: "80px", verticalAlign: "middle" }} /></label>
      </div>
    </div>
    <div class="grid">
      <div class="col"><${WarStrip} st=${st} faction=${faction} /><${MapPanel} st=${st} faction=${faction} rows=${rows} sel=${sel} onSel=${setSel} layers=${layers} world=${world} /><${SquadsTable} rows=${rows} sel=${sel} onSel=${setSel} /></div>
      <div class="col"><${ObjectivesPanel} st=${st} /><${Sitrep} st=${st} faction=${faction} /><${EventsPanel} st=${st} faction=${faction} /><${Log} st=${st} faction=${faction} /></div>
    </div></main>`;
}

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
  return html`<main><div class="cfgwrap"><section class="panel"><h2>Configuration <span class="sub">saved to dev/tools/sidecar-config.json</span></h2>
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
    ${st.config?.provider === "openrouter" && f.provider === "openrouter" && html`<${UsageCard} u=${st.usage} now=${st.now} toast=${toast} refresh=${refresh} />`}
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
  return html`<section class="panel" style=${{ marginTop: "12px" }}><h2>Usage <span class="sub">OpenRouter${d?.label ? " key " + d.label : ""}</span></h2>
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
  const [st, refresh] = useState2();
  const [hash, setHash] = useState(location.hash);
  const [toasts, toast] = useToasts();
  useEffect(() => { const h = () => setHash(location.hash); window.addEventListener("hashchange", h); return () => window.removeEventListener("hashchange", h); }, []);
  const cfg = hash === "#config";
  return html`<div>
    <header class="topbar">
      <div class="brand"><span class="logo"><${Icon} n="logo" /></span>Clausewitz HQ</div>
      <nav><a href="#hq" class=${cfg ? "" : "active"}>HQ</a><a href="#config" class=${cfg ? "active" : ""}>Configuration</a></nav>
      <${Strip} st=${st} toast=${toast} refresh=${refresh} />
    </header>
    ${cfg ? html`<${Config} st=${st} toast=${toast} refresh=${refresh} />` : html`<${HQ} st=${st} />`}
    <div class="toasts">${toasts.map((t) => html`<div class=${"toast " + t.kind} key=${t.id}>${t.text}</div>`)}</div>
  </div>`;
}
render(html`<${App} />`, document.getElementById("app"));
