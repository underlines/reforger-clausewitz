// Clausewitz HQ dashboard. Preact + htm (vendored, offline). All game text is rendered through Preact
// (auto-escaped); no dangerouslySetInnerHTML anywhere.
import { html, render, useState, useEffect, useRef, useMemo, useCallback } from "/vendor/standalone.module.js";

const WORLD = 4096, GRID = 512, STALE_MS = 30000;
const COLORS = { US: "#4aa3ff", USSR: "#ff5c5c" };
const colorOf = (f) => COLORS[f] ?? "#f0b429";
const enemyColor = (f) => (f === "US" ? COLORS.USSR : f === "USSR" ? COLORS.US : "#f0b429");
const facClass = (f) => (f === "US" || f === "USSR" ? "f-" + f : "f-other");
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

// ---------- data ----------
function useState2() {
  const [st, setSt] = useState({ offline: true, factions: {}, exchanges: [], counters: {}, config: {} });
  const poll = useCallback(async () => {
    try {
      const r = await fetch("/api/state", { cache: "no-store" });
      setSt({ ...(await r.json()), offline: false });
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
      const obj = s.objective && typeof s.objective === "object" ? s.objective : o ? { command: o.command, pos: o.pos, reason: o.reason } : null;
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
      });
    }
  }
  return rows;
}

// ---------- status strip ----------
function Strip({ st }) {
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
    <div class=${"chip" + (c.errors ? " bad" : "")}>errors <b>${c.errors ?? 0}</b></div>
    <div class="chip">tokens in/out <b>${c.inputTokens ?? 0} / ${c.outputTokens ?? 0}</b></div>
    ${bud && bud.level !== "ok" && html`<div class=${"chip " + (bud.level === "warn" ? "warn" : "bad")} title="OpenRouter key budget (see Configuration)"><span class="dot"></span>${bud.level === "reached" ? "budget limit reached" : `budget ${bud.pct.toFixed(0)}% used`}</div>`}
    <div class=${"chip " + (stale ? "bad" : "ok")} title="Red when the game has not contacted the sidecar for 30 s"><span class="dot"></span>last request <b>${ageTxt}</b></div>
  </div>`;
}

// ---------- map (canvas renderer with pan/zoom, tooltips, layers) ----------
function MapPanel({ st, faction, rows, sel, onSel, layers }) {
  const box = useRef(), cv = useRef();
  const view = useRef({ s: 0.1, ox: 0, oy: 0, fitted: false });
  const hits = useRef([]);
  const drag = useRef(null);
  const latest = useRef({});
  const [tip, setTip] = useState(null);
  latest.current = { st, faction, sel, layers };

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
    const hasData = x0 <= x1;
    if (!hasData) { x0 = 0; x1 = WORLD; z0 = 0; z1 = WORLD; }
    const cx = (x0 + x1) / 2, cz = (z0 + z1) / 2, span = hasData ? Math.max(x1 - x0, z1 - z0, 400) : WORLD;
    const wx = hasData ? Math.max(x1 - x0, 400) : WORLD, wz = hasData ? Math.max(z1 - z0, 400) : WORLD;
    v.s = Math.min(r.width / (wx * 1.3), r.height / (wz * 1.3));
    v.ox = r.width / 2 - cx * v.s; v.oy = r.height / 2 - (WORLD - cz) * v.s;
    v.fitted = hasData; v.user = false;
  };

  const draw = useCallback((t) => {
    const c = cv.current;
    if (!c) return;
    const { st, faction, sel, layers } = latest.current, v = view.current;
    const r = c.getBoundingClientRect(), dpr = window.devicePixelRatio || 1;
    if (!r.width) return;
    if (c.width !== Math.round(r.width * dpr) || c.height !== Math.round(r.height * dpr)) { c.width = Math.round(r.width * dpr); c.height = Math.round(r.height * dpr); }
    if (!v.fitted) fit(); // refits every frame until the first squad/contact arrives, then once
    const ctx = c.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    const sx = (x) => v.ox + x * v.s, sy = (z) => v.oy + (WORLD - z) * v.s;
    ctx.fillStyle = "#0a0d12"; ctx.fillRect(0, 0, r.width, r.height);
    const g = ctx.createLinearGradient(sx(0), sy(WORLD), sx(WORLD), sy(0));
    g.addColorStop(0, "#131b27"); g.addColorStop(1, "#0f1620");
    ctx.fillStyle = g; ctx.fillRect(sx(0), sy(WORLD), WORLD * v.s, WORLD * v.s);
    if (layers.grid) {
      ctx.lineWidth = 1; ctx.font = "10px sans-serif";
      for (let k = 0; k <= WORLD; k += GRID) {
        ctx.strokeStyle = k % 1024 === 0 ? "rgba(140,160,190,.22)" : "rgba(140,160,190,.10)";
        ctx.beginPath(); ctx.moveTo(sx(k), sy(0)); ctx.lineTo(sx(k), sy(WORLD)); ctx.stroke();
        ctx.beginPath(); ctx.moveTo(sx(0), sy(k)); ctx.lineTo(sx(WORLD), sy(k)); ctx.stroke();
        if (k < WORLD) { ctx.fillStyle = "#586377"; ctx.fillText(String(k), sx(k) + 3, sy(0) - 4); ctx.fillText(String(k), sx(0) + 3, sy(k) - 3); }
      }
    }
    ctx.strokeStyle = "rgba(140,160,190,.4)"; ctx.lineWidth = 1.5; ctx.strokeRect(sx(0), sy(WORLD), WORLD * v.s, WORLD * v.s);
    const h = [];
    ctx.font = "11px system-ui, sans-serif"; ctx.textBaseline = "middle";
    const shown = Object.keys(st.factions ?? {}).filter((f) => faction === "all" || f === faction);
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
      if (layers.orders) for (const o of Array.isArray(e.orders) ? e.orders : []) {
        const s = byId.get(o.squad);
        if (!s || !Array.isArray(o.pos) || !num(o.pos[0]) || !num(o.pos[1])) continue;
        const x0 = sx(s.pos[0]), y0 = sy(s.pos[2]), x1 = sx(o.pos[0]), y1 = sy(o.pos[1]);
        const col = colorOf(f), a = Math.atan2(y1 - y0, x1 - x0);
        ctx.strokeStyle = col; ctx.fillStyle = col; ctx.lineWidth = 2; ctx.globalAlpha = .85;
        ctx.setLineDash(o.command === "MOVE" ? [6, 5] : []);
        ctx.beginPath(); ctx.moveTo(x0, y0); ctx.lineTo(x1, y1); ctx.stroke(); ctx.setLineDash([]);
        ctx.beginPath(); ctx.moveTo(x1, y1); ctx.lineTo(x1 - 10 * Math.cos(a - .4), y1 - 10 * Math.sin(a - .4)); ctx.lineTo(x1 - 10 * Math.cos(a + .4), y1 - 10 * Math.sin(a + .4)); ctx.closePath(); ctx.fill();
        ctx.globalAlpha = 1;
        if (layers.labels) { ctx.font = "9px system-ui, sans-serif"; ctx.fillStyle = "rgba(221,227,236,.75)"; ctx.fillText(o.command, x1 + 8, y1 + 12); ctx.font = "11px system-ui, sans-serif"; }
        h.push({ x: x1, y: y1, r: 8, title: `${o.command} order`, lines: [String(o.squad), String(o.reason ?? "")] });
      }
      if (layers.squads) for (const s of squads) {
        const x = sx(s.pos[0]), y = sy(s.pos[2]), key = f + "/" + s.id, col = colorOf(f);
        if (s.humanLed) {
          const ph = ((t / 1200) % 1);
          ctx.strokeStyle = "#fff"; ctx.lineWidth = 2; ctx.globalAlpha = 1 - ph;
          ctx.beginPath(); ctx.arc(x, y, 9 + ph * 14, 0, Math.PI * 2); ctx.stroke(); ctx.globalAlpha = 1;
        }
        if (sel === key) { ctx.strokeStyle = "#ffe36e"; ctx.lineWidth = 3; ctx.beginPath(); ctx.arc(x, y, 14, 0, Math.PI * 2); ctx.stroke(); }
        ctx.fillStyle = col; ctx.shadowColor = col; ctx.shadowBlur = 10;
        ctx.beginPath(); ctx.arc(x, y, 7, 0, Math.PI * 2); ctx.fill(); ctx.shadowBlur = 0;
        if (s.humanLed) { ctx.strokeStyle = "#fff"; ctx.lineWidth = 2; ctx.stroke(); }
        if (layers.labels) {
          ctx.fillStyle = "#dde3ec";
          ctx.font = "600 11px system-ui, sans-serif"; ctx.fillText(shortId(s.id) + (s.humanLed ? " ★" : ""), x + 9, y - 11); ctx.font = "11px system-ui, sans-serif";
        }
        h.push({ x, y, r: 11, key, title: String(s.id), lines: [`${f}, ${s.agents}/${s.startAgents} agents, ${s.mode ?? "-"}`, `objective: ${s.objective?.command ?? (byOrder.get(s.id)?.command ?? "-")}`, s.humanLed ? "human-led" : "AI-led", `at ${gridTxt(s.pos)}`] });
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
    <h2>Map <span class="sub">${WORLD} x ${WORLD} m, x east / z north</span></h2>
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
const COLS = [["id", "Squad"], ["faction", "Faction"], ["type", "Type"], ["size", "Size"], ["strength", "Strength"], ["wounded", "Wnd"], ["objective", "Objective"],
  ["state", "State"], ["movement", "Movement"], ["leader", "Leader"], ["position", "Position x, z"], ["order", "Last order"]];
const SORTKEY = { id: (r) => r.id, faction: (r) => r.faction, type: (r) => String(r.type), size: (r) => r.agents ?? -1, strength: (r) => r.pct ?? -1, wounded: (r) => r.wounded ?? -1,
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
          <td><b>${r.id}</b></td><td class=${facClass(r.faction)}>${r.faction}</td><td>${r.type}</td><td class="mono">${r.size}</td>
          <td>${r.pct === null ? "-" : html`<span class="bar"><i style=${{ width: Math.min(100, r.pct) + "%", background: col }}></i></span>${r.pct}%${r.health !== null && html` <span class="muted">hp ${Math.round(r.health * 100)}%</span>`}`}</td>
          <td>${r.wounded === null ? "-" : r.wounded}</td>
          <td class="wrap">${o ? html`<span class=${"cmd " + o.command}>${o.command}</span> ${Array.isArray(o.pos) && num(o.pos[0]) ? html`<span class="mono">${Math.round(o.pos[0])}, ${Math.round(o.pos[1])}</span>` : ""} ${o.reason && html`<span class="muted">${o.reason}</span>`}` : "-"}</td>
          <td><span class=${"badge " + stateBadge(r.state)}>${r.state}</span></td><td class="mono">${r.move}</td>
          <td>${r.human ? html`<span class="badge human">human</span>` : "AI"}</td><td class="mono">${r.posTxt}</td>
          <td>${r.orderAge === null ? "-" : r.orderAge + " s"}</td></tr>`;
      })}</tbody></table>
      ${!rows.length && html`<div class="empty">No squads reported yet.</div>`}</div>
  </section>`;
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
        ${ct.length ? ct.map((c) => html`<tr><td>${dash(c.desc)}</td><td class="mono">${gridTxt(c.pos)}</td><td>${dash(c.ageSec)} s</td></tr>`) : html`<tr><td colspan="3" class="dim">no contacts</td></tr>`}
        </tbody></table></div>`;
    })}
    ${!names.length && html`<div class="empty">No data yet.</div>`}</div></section>`;
}

// ---------- exchange log ----------
const J = (o) => JSON.stringify(o, null, 2);
function Log({ st, faction }) {
  const ex = (st.exchanges ?? []).filter((x) => faction === "all" || x.faction === faction);
  return html`<section class="panel log"><h2>Orders / log <span class="sub">newest first, last ${st.exchanges?.length ?? 0}</span></h2><div class="scroll">
    ${ex.map((x) => html`<details class="ex" key=${x.ts + x.faction}><summary>
      <span class="mono muted">${new Date(x.ts).toLocaleTimeString()}</span><b class=${facClass(x.faction)}>${x.faction}</b>
      <span class="muted">${x.provider}${x.model ? " / " + x.model : ""}</span>
      ${x.ok && String(x.reason ?? "").startsWith("skipped") ? html`<span class="badge info">${x.reason}</span>` : x.ok ? html`<span class=${"badge " + (x.orders.length ? "ok" : "warn")}>${x.orders.length ? "ok" : "ok, no orders"}</span>` : html`<span class="badge bad">fallback: ${x.reason}</span>`}
      ${x.dropped?.length ? html`<span class="badge warn">${x.dropped.length} dropped</span>` : ""}
      <span>${x.orders.length} orders</span><span class="muted">${x.latencyMs} ms${x.usage ? ` , ${x.usage.input}/${x.usage.output} tok` : ""}${num(x.usage?.cost) ? ` , $${x.usage.cost.toFixed(4)}` : ""}</span></summary>
      <div class="body"><h3>Orders</h3><pre>${J(x.orders)}</pre>
      ${x.dropped?.length ? html`<h3>Dropped (with reason)</h3><pre>${J(x.dropped)}</pre>` : ""}
      ${x.rawReply != null ? html`<h3>Raw LLM reply</h3><pre>${x.rawReply}</pre>` : ""}
      <h3>Request</h3><pre>${J(x.request)}</pre></div></details>`)}
    ${!ex.length && html`<div class="empty">No exchanges yet.</div>`}</div></section>`;
}

// ---------- HQ view ----------
function HQ({ st }) {
  const [faction, setFaction] = useState("all");
  const [sel, setSel] = useState(null);
  const [layers, setLayers] = useState({ grid: true, squads: true, contacts: true, orders: true, labels: true });
  const names = Object.keys(st.factions ?? {}).sort();
  const rows = useMemo(() => buildRows(st, faction), [st, faction]);
  const tog = (k) => html`<label><input type="checkbox" checked=${layers[k]} onChange=${() => setLayers((l) => ({ ...l, [k]: !l[k] }))} /> ${k}</label>`;
  return html`<main>
    <div class="toolbar">
      <div class="seg">${["all", ...names].map((n) => html`<button class=${faction === n ? "on" : ""} onClick=${() => setFaction(n)}>${n}</button>`)}</div>
      <div class="toggles">${["grid", "squads", "contacts", "orders", "labels"].map(tog)}</div>
    </div>
    <div class="grid">
      <div class="col"><${MapPanel} st=${st} faction=${faction} rows=${rows} sel=${sel} onSel=${setSel} layers=${layers} /><${SquadsTable} rows=${rows} sel=${sel} onSel=${setSel} /></div>
      <div class="col"><${Sitrep} st=${st} faction=${faction} /><${Log} st=${st} faction=${faction} /></div>
    </div></main>`;
}

// ---------- configuration view ----------
async function postJson(url, body) {
  const r = await fetch(url, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
  let j = {}; try { j = await r.json(); } catch { /* ignore */ }
  return { ok: r.ok, j };
}
function Config({ st, toast, refresh }) {
  const [f, setF] = useState(null);
  const [keyMeta, setKeyMeta] = useState({ set: false, last4: "" });
  const [busy, setBusy] = useState(false);
  const [test, setTest] = useState(null);
  const load = useCallback(async () => {
    try {
      const c = await (await fetch("/api/config", { cache: "no-store" })).json();
      setF({ provider: c.provider, model: c.model, baseUrl: c.baseUrl, apiKey: "", timeoutSec: c.timeoutSec, maxTokens: c.maxTokens, enabled: !!c.enabled, minIntervalSec: c.minIntervalSec ?? 0, skipUnchanged: c.skipUnchanged !== false, stopAtLimit: c.stopAtLimit !== false });
      setKeyMeta({ set: c.apiKeySet, last4: c.apiKeyLast4 });
    } catch (e) { toast("Could not load config: " + e.message, "err"); }
  }, []);
  useEffect(() => { load(); }, [load]);
  if (!f) return html`<main><div class="empty">Loading...</div></main>`;
  const set = (k, v) => setF((o) => ({ ...o, [k]: v }));
  const body = () => {
    const b = { provider: f.provider, model: f.model, baseUrl: f.baseUrl, timeoutSec: Number(f.timeoutSec), maxTokens: Number(f.maxTokens), enabled: f.enabled, minIntervalSec: Number(f.minIntervalSec), skipUnchanged: f.skipUnchanged, stopAtLimit: f.stopAtLimit };
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
      <small class="muted">Batch APIs (24 h latency) don't fit a live war, so we cut call frequency instead. Skipped requests answer "keep current orders"; applies to real providers only (not fake).</small>
      ${f.provider === "openrouter" && html`<label class="switch"><input type="checkbox" checked=${f.stopAtLimit} onChange=${(e) => set("stopAtLimit", e.target.checked)} />
        <span>Stop provider calls at the key's spending limit <span class="muted">(/orders then answers no orders; resumes when budget is available again)</span></span></label>`}
      <label class="switch"><input type="checkbox" checked=${f.enabled} onChange=${(e) => set("enabled", e.target.checked)} />
        <span>Enabled <span class="muted">(off: /orders returns no orders, the game uses its rule commander)</span></span></label>
      <div class="btns"><button class="btn" type="submit" disabled=${busy}><${Icon} n="save" /> Save</button>
        <button class="btn ghost" type="button" disabled=${busy} onClick=${doTest}><${Icon} n="plug" /> Test connection</button></div>
      ${test && html`<pre>${test.pending ? "testing with the form values (nothing is saved)..." : J(test)}</pre>`}
    </form></section>
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
      <${Strip} st=${st} />
    </header>
    ${cfg ? html`<${Config} st=${st} toast=${toast} refresh=${refresh} />` : html`<${HQ} st=${st} />`}
    <div class="toasts">${toasts.map((t) => html`<div class=${"toast " + t.kind} key=${t.id}>${t.text}</div>`)}</div>
  </div>`;
}
render(html`<${App} />`, document.getElementById("app"));
