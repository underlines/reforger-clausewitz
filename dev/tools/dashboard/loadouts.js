// Clausewitz HQ dashboard: Loadouts tab (players' custom loadouts -> server library, compositions, pins, restart) and the admin account card.
// Data: GET /api/loadouts {players[], library[], compositions, pins, restartId, lastSync, factions[], roles[], kinds[], maxActive, bad}
// (sidecar, loadouts.mjs; loadout strings are not sent to the browser). Writes: POST /api/loadouts/copy|reorder|compositions|pins|restart,
// POST/DELETE /api/loadouts/library/<id>. The 7-active-per-faction cap is enforced by the sidecar (HTTP 409 -> toast).
import { html, useState, useEffect, useCallback } from "/vendor/standalone.module.js";
import { t, tk, getLang } from "/i18n.js";

const num = (v) => typeof v === "number" && Number.isFinite(v);
const STATUS_CLS = { ok: "ok", degraded: "warn", invalid: "bad" };
const shortPath = (p) => String(p).split("/").pop() || String(p); // prefab path -> file name (full path in the title)
const roleLabel = (r) => String(r || "").replace(/^ROLE_/, "");
const fmtSaved = (sec) => (num(sec) && sec > 0 ? new Date(sec * 1000).toLocaleString(getLang()) : "-");

async function call(method, url, body) {
  const r = await fetch(url, { method, headers: body === undefined ? {} : { "Content-Type": "application/json" }, body: body === undefined ? undefined : JSON.stringify(body) });
  let j = {};
  try { j = await r.json(); } catch { /* no body */ }
  return { ok: r.ok, status: r.status, j };
}

// ---------- item details (optional per record: a summary comes with the list, the full list is fetched on "Show items") ----------
const GROUPS = [
  ["weapons", ["rifle", "sniper", "machinegun", "launcher", "pistol", "weapon"]],
  ["ammo", ["magazine"]],
  ["attachments", ["attachment"]],
  ["clothing", ["headwear", "torso", "legs", "footwear", "handwear", "clothing"]],
  ["gear", ["vest", "backpack", "equipment"]],
  ["medical", ["medical"]],
  ["explosives", ["throwable", "explosive"]],
];
const GROUP_ORDER = [...GROUPS.map(([g]) => g), "other"];
const groupOf = (type) => (GROUPS.find(([, ts]) => ts.includes(type)) ?? ["other"])[0];
const itemName = (i) => i.name || i.key || (i.path ? shortPath(i.path).replace(/[.][a-z]+$/i, "") : "") || i.id; // the GUID is the last resort
const FILTER_FROM = 12; // the filter box shows from this many distinct items
function groupCounts(types) {
  const c = {};
  for (const [type, n] of Object.entries(types ?? {})) c[groupOf(type)] = (c[groupOf(type)] ?? 0) + n;
  return GROUP_ORDER.filter((g) => c[g]).map((g) => [g, c[g]]);
}

// compact summary + toggle; records without details show the plain item count
function ItemsCell({ rec, open, toggle }) {
  const s = rec.summary, n = s ? s.items : (rec.items ?? []).length;
  const chips = s ? groupCounts(s.types) : [];
  return html`<div class="lo-sum">
    <div class="lo-sumline"><b>${n}</b> ${t("lo.itemsWord")}${s && s.totalCost >= 0 ? " · " + t("lo.supplies", s.totalCost) : ""}${s?.primary ? " · " + t("lo.primary", s.primary) : ""}${rec.missing?.length ? html` <${Missing} list=${rec.missing} />` : ""}</div>
    ${chips.length > 0 && html`<div class="lo-chips">${chips.map(([g, c]) => html`<span class="lo-chip" key=${g}>${c} ${t("lo.grp." + g)}</span>`)}</div>`}
    <button class="btn ghost mini" type="button" aria-expanded=${open} onClick=${toggle}>${open ? t("lo.hideItems") : t("lo.showItems")}</button>
  </div>`;
}

// expanded row: items grouped by type (name, count, cost), sorted, filter box for long lists; raw prefab ids when the game sent no details
function ItemDetails({ kind, recKey, stamp }) {
  const [data, setData] = useState(null);
  const [err, setErr] = useState(null);
  const [q, setQ] = useState("");
  useEffect(() => {
    let off = false;
    call("GET", "/api/loadouts/" + kind + "/" + encodeURIComponent(recKey) + "/items")
      .then((r) => { if (off) return; if (r.ok) { setData(r.j); setErr(null); } else setErr(r.j.error ?? String(r.status)); })
      .catch((e) => { if (!off) setErr(e.message); });
    return () => { off = true; };
  }, [kind, recKey, stamp]);
  if (err) return html`<div class="muted">${t("common.failed", err)}</div>`;
  if (!data) return html`<div class="muted">${t("common.loading")}</div>`;
  const info = data.itemInfo ?? [];
  if (!info.length) return html`<div><small class="muted">${t("lo.noDetails")}</small><br /><${Items} list=${data.items} /></div>`;
  const needle = q.trim().toLowerCase();
  const shown = needle ? info.filter((i) => (itemName(i) + " " + i.type + " " + i.id).toLowerCase().includes(needle)) : info;
  const byGroup = new Map(GROUP_ORDER.map((g) => [g, []]));
  for (const i of shown) byGroup.get(groupOf(i.type)).push(i);
  const sum = info.reduce((a, i) => a + (i.cost >= 0 ? i.cost * i.count : 0), 0);
  return html`<div class="lo-details">
    ${info.length >= FILTER_FROM && html`<input type="search" class="lo-filter" value=${q} placeholder=${t("lo.filter")} onInput=${(e) => setQ(e.target.value)} />`}
    <div class="lo-itemwrap"><table class="lo-itemtable">
      <thead><tr><th>${t("lo.col.name")}</th><th class="num">${t("lo.col.qty")}</th><th class="num">${t("lo.col.cost")}</th></tr></thead>
      ${GROUP_ORDER.filter((g) => byGroup.get(g).length).map((g) => html`<tbody key=${g}>
        <tr class="lo-grp"><th colSpan="3">${t("lo.grp." + g)} <span class="muted">(${byGroup.get(g).reduce((a, i) => a + i.count, 0)})</span></th></tr>
        ${[...byGroup.get(g)].sort((a, b) => itemName(a).localeCompare(itemName(b), getLang()) || a.id.localeCompare(b.id)).map((i) => html`<tr key=${i.id}>
          <td title=${i.id + (i.path ? "\n" + i.path : "")}>${itemName(i)} <span class="muted lo-type">${tk("lo.type", i.type)}</span></td>
          <td class="num">${i.count > 1 ? "x" + i.count : "1"}</td><td class="num">${i.cost >= 0 ? i.cost : "-"}</td></tr>`)}
      </tbody>`)}
    </table></div>
    ${shown.length === 0 && html`<div class="muted">${t("lo.noMatch")}</div>`}
    <small class="muted">${t("lo.itemsFoot", sum, data.totalCost >= 0 ? data.totalCost : "-")}</small>
  </div>`;
}
const DetailRow = ({ span, ...props }) => html`<tr class="lo-detail"><td colSpan=${span}><${ItemDetails} ...${props} /></td></tr>`;

const StatusBadge =({ s }) => html`<span class=${"badge " + (STATUS_CLS[s] ?? "")}>${s ?? "-"}</span>`;
const Items = ({ list }) => (list?.length
  ? html`<span class="lo-items">${list.map((p) => html`<span class="mono lo-item" title=${p}>${shortPath(p)}</span>`)}</span>`
  : html`<span class="muted">-</span>`);

// ordered list of values (roles or library ids) edited with selects; saved explicitly
function ListEditor({ value, options, onSave, saveLabel, resetLabel }) {
  const key = JSON.stringify(value);
  const [draft, setDraft] = useState(value);
  const [dirty, setDirty] = useState(false);
  useEffect(() => { if (!dirty) setDraft(value); }, [key]);
  const set = (i, v) => { setDraft(draft.map((x, j) => (j === i ? v : x))); setDirty(true); };
  const del = (i) => { setDraft(draft.filter((_, j) => j !== i)); setDirty(true); };
  const add = () => { if (options.length) { setDraft([...draft, options[0].v]); setDirty(true); } };
  const save = async (list) => { if (await onSave(list)) { setDirty(false); } };
  return html`<div class="lo-list">
    ${draft.map((v, i) => html`<span class="lo-pick" key=${i}><select value=${v} onChange=${(e) => set(i, e.target.value)}>
      ${options.some((o) => o.v === v) ? "" : html`<option value=${v}>${v}</option>`}
      ${options.map((o) => html`<option value=${o.v}>${o.label}</option>`)}</select>
      <button class="btn ghost mini" type="button" title=${t("lo.remove")} onClick=${() => del(i)}>x</button></span>`)}
    <button class="btn ghost mini" type="button" onClick=${add} disabled=${!options.length}>+</button>
    <button class="btn mini" type="button" disabled=${!dirty} onClick=${() => save(draft)}>${saveLabel}</button>
    ${resetLabel && html`<button class="btn ghost mini" type="button" disabled=${!draft.length && !value.length} onClick=${() => { setDraft([]); save([]); }}>${resetLabel}</button>`}
  </div>`;
}

function PlayersTable({ faction, rows, d, act }) {
  const [copyKey, setCopyKey] = useState(null);
  const [name, setName] = useState("");
  const [role, setRole] = useState("");
  const startCopy = (p) => { setCopyKey(p.identityId); setName(p.playerName || ""); setRole(""); };
  const doCopy = async (p) => { if (await act("POST", "/api/loadouts/copy", { identityId: p.identityId, faction, name, role }, t("lo.copied"))) setCopyKey(null); };
  if (!rows.length) return html`<div class="empty">${t("lo.noPlayers")}</div>`;
  return html`<div class="scroll"><table>
    <thead><tr><th>${t("lo.col.player")}</th><th>${t("lo.col.savedAt")}</th><th>${t("lo.col.items")}</th><th>${t("lo.col.addons")}</th><th>${t("col.status")}</th><th></th></tr></thead>
    <tbody>${rows.map((p) => html`<${PlayerRow} key=${p.identityId} p=${p} d=${d} open=${copyKey === p.identityId} startCopy=${startCopy} cancel=${() => setCopyKey(null)} doCopy=${doCopy} name=${name} setName=${setName} role=${role} setRole=${setRole} />`)}</tbody>
  </table></div>`;
}
function PlayerRow({ p, d, open, startCopy, cancel, doCopy, name, setName, role, setRole }) {
  const bad = p.status === "degraded" || p.status === "invalid";
  const [showItems, setShowItems] = useState(false);
  return [html`<tr class=${bad ? "lo-bad" : ""} key="row">
      <td><b>${p.playerName || p.identityId}</b> <span class="muted mono" title=${p.identityId}>${p.basePrefab ? shortPath(p.basePrefab) : ""}</span></td>
      <td class="muted">${fmtSaved(p.savedAt)}</td>
      <td title=${(p.items ?? []).join("\n")}><${ItemsCell} rec=${p} open=${showItems} toggle=${() => setShowItems(!showItems)} /></td>
      <td><${Addons} list=${p.addons} d=${d} /></td>
      <td><${StatusBadge} s=${p.status} /></td>
      <td>${open
        ? html`<span class="lo-pick"><input type="text" value=${name} maxlength="80" placeholder=${t("lo.col.name")} onInput=${(e) => setName(e.target.value)} />
            <select value=${role} onChange=${(e) => setRole(e.target.value)}><option value="">${t("lo.noRole")}</option>${d.roles.map((r) => html`<option value=${r}>${roleLabel(r)}</option>`)}</select>
            <button class="btn mini" type="button" onClick=${() => doCopy(p)}>${t("lo.copyConfirm")}</button>
            <button class="btn ghost mini" type="button" onClick=${cancel}>${t("common.cancel")}</button></span>`
        : html`<button class="btn ghost mini" type="button" onClick=${() => startCopy(p)}>${t("lo.copy")}</button>`}</td>
    </tr>`, showItems && html`<${DetailRow} key="detail" span=${6} kind="player" recKey=${p.identityId + "|" + p.faction} stamp=${p.savedAt} />`];
}

const Missing = ({ list }) => html`<span class="badge bad" title=${list.join("\n")}>${t("lo.missingN", list.length)}</span>`;
// addons at save time; the ones that are not loaded in the last synced war are flagged red
function Addons({ list, d }) {
  const loaded = d.lastSync ? new Map((d.lastSync.addons ?? []).map((a) => [a.guid, a.name])) : null;
  if (!list?.length) return html`<span class="muted">-</span>`;
  return html`<span class="lo-items">${list.map((g) => {
    const gone = loaded && !loaded.has(g);
    return html`<span class=${"mono lo-item" + (gone ? " lo-gone" : "")} title=${gone ? t("lo.addonGone", g) : g}>${(loaded && loaded.get(g)) || g.slice(0, 8)}</span>`;
  })}</span>`;
}

function LibraryTable({ faction, rows, d, act }) {
  const active = rows.filter((e) => e.active).sort((a, b) => a.order - b.order);
  const move = (e, dir) => act("POST", "/api/loadouts/library/" + e.id, { order: e.order + dir });
  const del = (e) => { if (window.confirm(t("lo.deleteConfirm", e.name))) act("DELETE", "/api/loadouts/library/" + e.id, undefined, t("lo.deleted")); };
  if (!rows.length) return html`<div class="empty">${t("lo.noLibrary")}</div>`;
  const sorted = [...rows].sort((a, b) => (b.active - a.active) || (a.order - b.order) || a.id.localeCompare(b.id, "en", { numeric: true }));
  return html`<div class="scroll"><table>
    <thead><tr><th>${t("lo.col.active")}</th><th>${t("lo.col.order")}</th><th>${t("lo.col.name")}</th><th>${t("lo.col.role")}</th><th>${t("col.status")}</th><th>${t("lo.col.items")}</th><th>${t("lo.col.addons")}</th><th>${t("lo.col.source")}</th><th></th></tr></thead>
    <tbody>${sorted.map((e) => html`<${LibraryRow} key=${e.id} e=${e} active=${active} d=${d} act=${act} move=${move} del=${del} />`)}</tbody>
  </table></div>`;
}
function LibraryRow({ e, active, d, act, move, del }) {
  const bad = e.status === "degraded" || e.status === "invalid";
  const [showItems, setShowItems] = useState(false);
  return [html`<tr class=${bad ? "lo-bad" : ""} key="row">
        <td><label class="switch"><input type="checkbox" checked=${e.active} onChange=${(ev) => act("POST", "/api/loadouts/library/" + e.id, { active: ev.target.checked })} /></label></td>
    <td class="nowrap">${e.active ? html`<b>${e.order}</b>
      <button class="btn ghost mini" type="button" disabled=${e.order <= 1} onClick=${() => move(e, -1)} title=${t("lo.up")}>^</button><button class="btn ghost mini" type="button" disabled=${e.order >= active.length} onClick=${() => move(e, 1)} title=${t("lo.down")}>v</button>` : html`<span class="muted">-</span>`}</td>
    <td><input type="text" class="lo-name" value=${e.name} key=${e.id + e.name} maxlength="80" onBlur=${(ev) => { const v = ev.target.value.trim(); if (v && v !== e.name) act("POST", "/api/loadouts/library/" + e.id, { name: v }); }} /></td>
    <td><select value=${e.role} onChange=${(ev) => act("POST", "/api/loadouts/library/" + e.id, { role: ev.target.value })}><option value="">${t("lo.noRole")}</option>${d.roles.map((r) => html`<option value=${r}>${roleLabel(r)}</option>`)}</select></td>
    <td><${StatusBadge} s=${e.status} /></td>
    <td class="wrap"><span title=${(e.items ?? []).join("\n")}><${ItemsCell} rec=${e} open=${showItems} toggle=${() => setShowItems(!showItems)} /></span>${e.missing?.length ? html` <${Items} list=${e.missing} />` : ""}</td>
    <td class="wrap"><${Addons} list=${e.addons} d=${d} /></td>
    <td class="muted">${e.createdFrom?.playerName || e.createdFrom?.identityId || "-"}</td>
    <td><button class="btn ghost mini" type="button" onClick=${() => del(e)}>${t("lo.delete")}</button></td>
      </tr>`, showItems && html`<${DetailRow} key="detail" span=${9} kind="library" recKey=${e.id} stamp=${e.id + ":" + (e.summary?.items ?? 0) + ":" + (e.totalCost ?? "")} />`];
}

function CompositionsPanel({ d, act }) {
  const roleOpts = d.roles.map((r) => ({ v: r, label: roleLabel(r) }));
  return html`<section class="panel"><h2>${t("lo.compositions")} <span class="sub">${t("lo.compositionsSub")}</span></h2>
    <div class="cfgbody">
      <small class="muted">${t("lo.compositionsNote")}</small>
      ${d.kinds.map((k) => html`<div class="lo-kind" key=${k}><b>${k}</b>${!(d.compositions[k]?.length) && html` <span class="muted">${t("lo.defaults")}</span>`}
        <${ListEditor} value=${d.compositions[k] ?? []} options=${roleOpts} saveLabel=${t("common.save")} resetLabel=${t("lo.resetDefaults")}
          onSave=${(roles) => act("POST", "/api/loadouts/compositions", { kind: k, roles }, t("lo.saved"))} /></div>`)}
    </div></section>`;
}

function PinsBlock({ faction, rows, d, act }) {
  const opts = rows.map((e) => ({ v: e.id, label: `${e.name}${e.role ? " (" + roleLabel(e.role) + ")" : ""}` }));
  if (!rows.length) return null;
  return html`<div class="cfgbody lo-pins"><b>${t("lo.pins")}</b> <small class="muted">${t("lo.pinsNote")}</small>
    ${d.kinds.map((k) => html`<div class="lo-kind" key=${k}><span class="lo-kindname">${k}</span>
      <${ListEditor} value=${d.pins[faction]?.[k] ?? []} options=${opts} saveLabel=${t("common.save")} resetLabel=${t("lo.unpin")}
        onSave=${(ids) => act("POST", "/api/loadouts/pins", { faction, kind: k, ids }, t("lo.saved"))} /></div>`)}
  </div>`;
}

function FactionSection({ faction, d, act }) {
  const players = d.players.filter((p) => p.faction === faction).sort((a, b) => (b.savedAt ?? 0) - (a.savedAt ?? 0));
  const lib = d.library.filter((e) => e.faction === faction);
  const nActive = lib.filter((e) => e.active).length, bad = lib.filter((e) => e.status === "degraded" || e.status === "invalid").length;
  const inWar = !d.lastSync || (d.lastSync.factions ?? []).some((f) => f.key === faction) || !(d.lastSync.factions ?? []).length;
  return html`<section class="panel"><h2>${faction}
    <span class="sub"><span class=${"badge " + (nActive >= d.maxActive ? "warn" : "info")}>${t("lo.activeCount", nActive, d.maxActive)}</span>
      ${bad > 0 && html` <span class="badge bad">${t("lo.badCount", bad)}</span>`}${!inWar && html` <span class="badge">${t("lo.notInWar")}</span>`}</span></h2>
    <div class="cfgbody">
      <h3 class="cfgh">${t("lo.players")}</h3>
      <${PlayersTable} faction=${faction} rows=${players} d=${d} act=${act} />
      <h3 class="cfgh">${t("lo.library")}</h3>
      <${LibraryTable} faction=${faction} rows=${lib} d=${d} act=${act} />
    </div>
    <${PinsBlock} faction=${faction} rows=${lib} d=${d} act=${act} />
  </section>`;
}

export function LoadoutsView({ toast }) {
  const [d, setD] = useState(null);
  const [err, setErr] = useState(null);
  const load = useCallback(async () => {
    try { const r = await call("GET", "/api/loadouts"); if (r.ok) { setD(r.j); setErr(null); } else setErr(r.j.error ?? String(r.status)); }
    catch (e) { setErr(e.message); }
  }, []);
  useEffect(() => { load(); const iv = setInterval(load, 4000); return () => clearInterval(iv); }, [load]);
  // run a write call; toast the result (errors show the sidecar message, e.g. the 7-active cap); true on success
  const act = useCallback(async (method, url, body, okText) => {
    try {
      const r = await call(method, url, body);
      if (r.ok) { if (okText) toast(okText, "ok"); await load(); return true; }
      toast(r.j.error ?? t("err.request"), "err");
    } catch (e) { toast(t("err.generic", e.message), "err"); }
    await load();
    return false;
  }, [load, toast]);
  if (!d) return html`<main class="view"><div class="empty">${err ? t("common.failed", err) : t("common.loading")}</div></main>`;
  const restart = async () => { if (window.confirm(t("lo.restartConfirm"))) { const r = await call("POST", "/api/loadouts/restart", {}); if (r.ok) { toast(t("lo.restarted", r.j.restartId), "ok"); load(); } else toast(r.j.error ?? t("err.request"), "err"); } };
  const sync = d.lastSync;
  const facs = [...d.factions].sort();
  return html`<main class="view">
    <section class="panel"><h2>${t("lo.restartTitle")} <span class="sub">${sync ? t("lo.lastSync", new Date(sync.time).toLocaleString(getLang()), sync.round, sync.gameVersion || "-") : t("lo.noSync")}</span></h2>
      <div class="cfgbody">
        <div class="btns"><button class="btn" type="button" onClick=${restart}>${t("lo.restartBtn")}</button><span class="muted" style=${{ alignSelf: "center" }}>${t("lo.restartId", d.restartId)}</span></div>
        <small class="muted">${t("lo.restartNote")}</small>
        ${d.bad > 0 && html`<div class="banner bad">${t("lo.badBanner", d.bad)}</div>`}
      </div></section>
    ${facs.length === 0 && html`<div class="empty">${t("lo.empty")}</div>`}
    ${facs.map((f) => html`<${FactionSection} key=${f} faction=${f} d=${d} act=${act} />`)}
    <${CompositionsPanel} d=${d} act=${act} />
  </main>`;
}

// ---------- admin account (Usage & Config tab) ----------
export function AccountCard({ toast, cfg, defaultPassword, refresh }) {
  const [oldPw, setOld] = useState(""), [newPw, setNew] = useState(""), [again, setAgain] = useState("");
  const [token, setToken] = useState("");
  const [busy, setBusy] = useState(false);
  const change = async (e) => {
    e.preventDefault();
    if (newPw.length < 8) return toast(t("acct.tooShort"), "err");
    if (newPw !== again) return toast(t("acct.mismatch"), "err");
    setBusy(true);
    const r = await call("POST", "/api/password", { oldPassword: oldPw, newPassword: newPw });
    setBusy(false);
    if (r.ok) { toast(t("acct.changed"), "ok"); setOld(""); setNew(""); setAgain(""); refresh(); }
    else toast(r.status === 403 ? t("acct.oldWrong") : (r.j.error ?? t("err.request")), "err");
  };
  const saveToken = async (value) => {
    const r = await call("POST", "/api/config", { gameToken: value });
    if (r.ok) { toast(value ? t("acct.tokenSaved") : t("acct.tokenCleared"), "ok"); setToken(""); refresh(); }
    else toast(r.j.error ?? t("err.request"), "err");
  };
  return html`<section class="panel"><h2>${t("acct.title")}</h2>
    <div class="cfgbody">
      ${defaultPassword && html`<div class="banner warn">${t("auth.defaultBanner")}</div>`}
      <form class="cfgbody nopad" onSubmit=${change}>
        <label class="field"><span>${t("acct.old")}</span><input type="password" autocomplete="current-password" value=${oldPw} onInput=${(e) => setOld(e.target.value)} /></label>
        <div class="pair">
          <label class="field"><span>${t("acct.new")}</span><input type="password" autocomplete="new-password" value=${newPw} onInput=${(e) => setNew(e.target.value)} /></label>
          <label class="field"><span>${t("acct.confirm")}</span><input type="password" autocomplete="new-password" value=${again} onInput=${(e) => setAgain(e.target.value)} /></label></div>
        <small class="muted">${t("acct.pwNote")}</small>
        <div class="btns"><button class="btn" type="submit" disabled=${busy || !oldPw || !newPw}>${t("acct.change")}</button></div>
      </form>
      <h3 class="cfgh">${t("acct.gameToken")}</h3>
      <small class="muted">${t("acct.gameTokenNote")}</small>
      <div class="muted">${cfg?.gameTokenSet ? t("acct.tokenIsSet") : t("acct.tokenNone")}</div>
      <label class="field"><span>${t("acct.tokenNew")}</span><input type="password" autocomplete="off" value=${token} onInput=${(e) => setToken(e.target.value)} /></label>
      <div class="btns"><button class="btn" type="button" disabled=${!token.trim()} onClick=${() => saveToken(token.trim())}>${t("common.save")}</button>
        <button class="btn ghost" type="button" disabled=${!cfg?.gameTokenSet} onClick=${() => saveToken("")}>${t("acct.tokenClear")}</button></div>
    </div></section>`;
}
