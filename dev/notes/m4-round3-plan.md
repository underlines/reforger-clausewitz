# M4 round 3 plan (handover, 2026-10-12)

Written for a fresh session with no prior context. Read this file, then `dev/PROGRESS.md` (M4 section), `dev/notes/m4-design.md`.

## 1. Context in 12 lines
- Mod: Clausewitz (Arma Reforger, Enforce script + Node LLM sidecar `dev/tools/llm_sidecar.mjs` with web dashboard on http://127.0.0.1:8788/).
  Repo `D:\GitHub\ReforgerMods\Clausewitz` (own git repo, nested in the workspace repo). Branch **`feature/m4-any-map`** (not merged to `main`; release via `reforger-release` skill only).
- Runtime test = local dedicated server (DS), skill `reforger-workbench-test`. Workspace tools `D:\GitHub\ReforgerMods\_dedicated\`:
  `start-server.ps1 -World <world> -Packs <file> [-Extra "-clwTakeover"]`, `stop-server.ps1`, `start-client.ps1 -Packs <same file>` (the owner's game client, joins 127.0.0.1).
  Server log: `_dedicated\profile\logs\<latest>\console.log`; client log: `C:\Users\Jan\Documents\My Games\ArmaReforger\logs\<latest>\console.log`.
- Current owner test setup: **GM takeover** on vanilla Game Master Everon: `start-server.ps1 -World worlds/GameMaster/GM_Eden.ent -Packs packs-e2e-full.txt -Extra "-clwTakeover"`
  (`packs-e2e-full.txt` = RHS + WCS gear + WCS_Arsenal + GRS + WCS Loadout Editor). Setup preset in `_dedicated\profile\profile\Clausewitz\overrides.json`
  (RHS_USAF vs RHS_AFRF, random start, `autoStartSec` 300). Sidecar: `node dev/tools/llm_sidecar.mjs` (cwd `dev/tools`, OpenRouter config).
- Build stamp before each server run: `pwsh D:\GitHub\ReforgerMods\_tools\stamp-build.ps1 -Mod Clausewitz -Tag CLW` (pwsh 7 only: Windows PowerShell writes a BOM -> compile error).

## 1b. Owner decisions (2026-10-12)
- Rematch in place after victory (no world reload). Keep per-base supply stock: FOB arsenals use FOB-local stock, convoys carry it.
- Vehicle depot may stay at its current distance; everything else 30 % tighter. FOB shown with a base marker + arrows, never as an objective.
- HQ memory: compact journal (WP5), togglable in the dashboard. Last step: modern vehicle packs (WP8).
- At handover the DS, the owner's client and the sidecar may still be running: , close the client, kill the node sidecar before the first build.

## 2. Working rules (owner's, learned the hard way)
- Implementation by **Sonnet subagents** (`model: sonnet`), code only (no Workbench/DS/e2e); disjoint file ownership per work package; I merge, compile and test on the DS.
- **Create each subagent's worktree myself**: `git -C D:\GitHub\ReforgerMods\Clausewitz worktree add .claude/worktrees/<wp> -b wp/<wp> feature/m4-any-map` and pass the absolute path.
  Never `isolation:"worktree"` from a cwd outside the Clausewitz repo (it worktrees the outer repo; the harness deleted one with a whole refactor in it).
- Tell subagents not to kill node processes (one killed the owner's sidecar).
- Enforce gotchas: `.claude/skills/reforger-script-dev/SKILL.md` (no `func` params, never `modded class` a vanilla [BaseContainerProps] class used by prefabs, ...).
- Monitors: stream only errors; milestones via one-shot waits. No vanilla hard-wiring (content in `.conf` / runtime discovery).

## 3. Owner test results (2026-10-12 evening, GM Everon takeover, RHS sides, builds #80-#81)
OK: HQ menu; `/request vehicle` refusal on missing supplies; `/report`; GM Everon takeover; arsenal prices, kit cost deducted, WCS weapons (SPEAR) priced
and purchasable; all own-side map markers; start objectives (1 per side); garrison; base tighter than before; FOB ordered by LLM after the 6 min gate,
toward the front, built by a walking builder (23:08 -> 23:14); convoys deliver to FOBs (FOB arsenal uses FOB-local stock: **keep as is**).
Server-side seen by me: no new script errors (only third-party WCS `SCR_AISwitchMagazine` VM exceptions).
Problems / feedback:
- P1 group names cryptic (`RHS_USAF_Inf2 - Infantry Squad (Atlas Red 4)`); garrison not recognisable.
- P2 depot shows only vanilla-looking vehicles (M151A2 x6, M998 x2, M1025 x3, M997): RHS_USAF's own catalog is mostly vanilla models; depot uses the faction catalog only, not the side pool.
- P3 base still too spread: ~10 m between items. HQ tent (spawn) + command post should almost touch, arsenals next to them. Owner: **30 % tighter than now**.
- P4 field hospital useless: remove it everywhere.
- P5 FOB order not visible on dashboard map (no planned site, no builder arrow).
- P6 after victory: client kicked `JIP_ERROR` (server reloads the world ~30 s after war over; the reconnect fails). Owner decision: **rematch in place**.
- P7 depot supply check logged too early (1.8 s after spawn: "consumer sees 0").
- P8 HUD: show faction supplies next to the tickets.
- P9 economy starvation: supplies drained to ~40 and stayed; convoy CV2 loaded the last 61 (HQ stock 0), CV1 failed "nothing to load"; side at AI cap 46/45.
- P10 no idle stand-down in 15 min with `idleDespawnMin` 2: verify it fires (or that no squad idled).
- P11 kit purchases not attributable in the log (only per-minute supply totals).
- P12 HQ agent has no memory between calls (stateless; only current orders + reasons, events since the last call, FOBs/convoys in the situation).
- P13 wanted: subtle flank nudge for the LLM.
- P14 client: local faction flips RHS_USAF/RHS_AFRF ~10x in 15 s while in the deploy menu (marker restore handler fires each time; harmless so far).

## 4. Work packages
Order: WP0 docs, then WP1-WP7 in parallel worktrees (file ownership below), merge, one DS compile + server-side check, owner client test, then WP8.

### WP0 Docs (me, first, small)
- `dev/PROGRESS.md`: round-3 results (section 3), the new open list, the owner checklist (section 6).
- `dev/VISION.md`: logistics decision (per-base stock, FOB arsenals on local stock, convoys carry the stock; physical crates later); HQ memory (WP5); rematch-in-place loop.
- `README.md` (Clausewitz): how to run GM takeover on the DS, packs files, dashboard; link this plan.

### WP1 Readable names + HUD (owner P1, P8)
- Display names for every CLW squad: `1st Rifle Squad`, `2nd Rifle Squad`, `Recon Team 1`, `Motorized Squad 1`, `Mechanized Squad 1`, `Armor 1`, `Supply Truck 1`, `FOB Team 1`, garrison `HQ Garrison`.
  Ordinal per side and kind; kinds come from the catalog kind (data), no faction text. Shown in the in-game group list (vanilla callsign replaced: check `SCR_AIGroup` custom name API), map, HQ menu, SITREP, LLM situation, dashboard. Internal ids stay for logs and as the LLM's order key (send `{id, name}`).
- HUD (top widget with the tickets): own faction supplies (`CLW_WarStatus` RPC already broadcasts the war state; add supplies per side, show own only).
- Files: squad naming (`CLW_SquadRegistry.c`, `CLW_ForceSpawner.c` naming part, `CLW_BaseWatch.c` garrison name), `CLW_WarStatus.c` + HUD client file(s), situation name field (`CLW_Situation.c` names only).

### WP2 Compact base, no medical, depot (P2 partly, P3, P4, P7)
- Layout: anchor = command post at the HQ point; spawn tent touching it (gap 0.5-1 m), arsenal(s) 2-4 m from them, supply 5-8 m.
  All current `PLACE_*` radii/gaps in `CLW_Services.c` x0.7 as the new default (owner: 30 % tighter) for everything EXCEPT the vehicle depot: the depot keeps
  today's distance (owner OK, vehicles need room). Authored slot offsets per service kind in the faction profile data. Keep collision/slope checks.
- Remove `service.medical` from base services, FOB upgrade (tier 2), faction profiles, `PropSets.conf`; nothing may still reference it.
- Depot list = the side pool (own + kin catalogs, same as `/request vehicle`, `CLW_ForceCatalog`), not only the faction catalog. If the vanilla spawner component cannot take a custom list, set the depot to the faction with the most side-pool vehicles and log the gap.
- Depot/arsenal supply check: log 10 s after spawn (P7); add a `CLW: kit <player> <side> took kit cost <n> at <base> (stock a -> b)` line (P11).
- Files: `CLW_Services.c`, `Configs/CLW/Factions/*.conf`, `Configs/CLW/Props/PropSets.conf`, `CLW_Fob.c` (upgrade tier only), `CLW_SupplyLink.c` (kit log).

### WP3 Maps: FOB + convoy visibility (P5, P14)
- Owner intent: show the FOB visually, NOT as an objective (no objective marker, no capture logic). Use the same general base marker style as the HQ
  (an HQ/FOB base icon), with the state in the label: `FOB1 (planned)` -> `FOB1 (building 45 %)` -> `FOB1`; removed when the FOB is lost/refunded.
- Units tasked with a FOB are shown with an arrow on the map: builder (and escort) -> FOB site; convoy truck -> destination base. In game: own side only,
  an arrow/line marker if the marker API offers one (vanilla map has line/arrow drawing for commanders: check `SCR_MapMarker*` / map draw APIs), else a
  marker on the unit labelled `FOB Team 1 -> FOB1` that follows it (update every ~5 s). Dashboard: arrows builder -> site and truck -> destination.
- Dashboard map: FOB base icon + status label, arrows as above (data from the situation `fobs[]` / `convoys[]`; add builder/escort/truck positions if missing).
- P14: the client marker restore runs only when the LOCAL player's faction really changed (compare with the last applied key).
- Files: `CLW_StructureMarkers.c`, `CLW_MapMarkerClient.c`, `dev/tools/dashboard/*`, situation fob/convoy fields (`CLW_Situation.c` fob/convoy part only: coordinate with WP1 via separate functions).

### WP4 Rematch in place (P6)
- After WAR OVER: results stay ~60 s (scoreboard/announcement as now), then back to SETUP in the same session, without the engine game end / world reload:
  delete war entities (CLW squads + vehicles, services, FOBs, markers, spawn points we made), reset runtime statics/state, re-run the setup phase (preset auto start still works).
- First reproduce P6 unattended: preset `timeLimitMin` 1-2, start the client myself (`start-client.ps1`), watch `JIP_ERROR`. If the reload must stay possible (admin), also check for statics that survive a world reload in `CLW_WarRuntime` (stale instance -> item table mismatch).
- Files: `CLW_WarRuntime.c`, `CLW_WarState.c` / war-end path, `CLW_SetupServer.c`, cleanup helpers in the owning managers (one `Reset()` per manager).

### WP5 HQ campaign memory ("war journal") + flank nudge (P12, P13) - sidecar, game only supplies outcome data
Why goals/todos are not enough: long-term play needs (1) an intent to commit to, (2) knowledge of what was tried and how it went,
(3) lessons drawn from that, (4) a model of the enemy, (5) rules for when to change the plan. Small models do not keep these by themselves;
they need them as short, fixed-slot text, and the bookkeeping (counting tries/failures) must be done by code, not by the model.

Journal per faction (sidecar memory, reset at war start; dashboard shows it and lets the owner edit it). Fixed slots, hard caps, ~250 tokens total:
| slot | written by | cap | purpose |
|---|---|---|---|
| `intent` | model | 1 x 120 chars | how we win: "Hold B+C, flank E via north FOB, bleed enemy" |
| `phase` | model | enum opening / build-up / main effort / consolidate / last stand | sets the tempo |
| `goals` | model | 3 x 40 chars, each `{text, by: <war min>, status: open/done/failed}` | measurable, with a deadline |
| `plan` | model | 5 x 40 chars, each linked to a goal | next concrete steps (todos) |
| `beliefs` | model | 3 x 50 chars + confidence low/med/high | enemy model: "enemy masses at D", "no enemy armour seen" |
| `lessons` | model, seeded by code | 5 x 60 chars | "frontal on C failed 2x: needs flank/armour" |
| `commit` | model | `{until: <war min>, unless: <trigger>}` | anti flip-flop: keep the plan until then unless a trigger fires |
| `ledger` | **code** | last 8 entries, ~25 chars | `ATTACK C x2 -> FAIL (-6 inf)`, `FOB1 -> built`, `buy armor -> lost 2 min` |
| `tries` | **code** | per target/action `{tries, fails, lastResult, ageMin}` | the "we tried 2x and failed" fact, never left to the model |
Code side: the sidecar builds `ledger` + `tries` from the orders it sent and the game's outcome events (order failed / waypoints lost / squad lost /
capture / FOB built or refunded / convoy delivered or failed; add missing outcome events to the situation `events`). When a target reaches 2 fails,
the code adds a lesson CANDIDATE ("C: 2 failed attacks") that the next review must keep, rewrite or drop.

Two update paths, both togglable (dashboard settings group `memory`: `mode` off | inline | review | both (default both), `reviewMin` (default 5),
caps; fake provider supports it):
- inline (every commander call): the reply may carry `journal: {intent?, phase?, goalDone?, goalFailed?, planDone?, addPlan?}` (small edits only).
- review (every `reviewMin`, plus on triggers: objective lost, 2nd fail on a target, FOB lost, phase deadline passed): one dedicated small call
  "after-action review" with a fixed 4-question form, answers go into the slots:
  1. What changed since the last review? (-> beliefs) 2. Which goal is behind, and why? (-> goals/lessons) 3. Keep or change the intent? Change only with
  a reason that cites a ledger/tries fact (-> intent/commit) 4. Next 3 steps (-> plan). Output = the full journal JSON, validated against the caps.
Injection: one `HQ JOURNAL` block in the commander prompt and the `/hq` agent prompt, in slot order, ledger/tries compacted to one line each;
plus 2 prompt rules: "Follow your intent until `commit.until` unless its trigger fired; if you change it, say which fact made you change." and
"Do not repeat an action listed 2x failed in `tries` without changing the approach (flank, armour, FOB, more force, other target)."
Flank nudge: one line ("Consider flanks: a FOB or a route off the direct axis can beat a frontal push.") + per objective `axisAngle` (deg off the
HQ-HQ line) and `side: left/centre/right` in the situation, so a small model can name a flank without geometry.
Why not tool calls (owner's first idea): tool schemas cost ~300 tokens per call and small models break multi-tool loops; fixed JSON slots in the normal
reply plus a dedicated review call give the same edits with one simple output format.
Files: `dev/tools/llm_sidecar.mjs`, `dev/tools/agent/*`, dashboard journal view + memory settings (WP5 owns those dashboard files; WP3 owns the map
files), `CLW_Situation.c` objective part (axisAngle, side) + outcome events (coordinate with WP1/WP3: separate functions).

### WP6 Economy guard (P9, P10)
- HQ reserve floor (param `hqReserve`, default 100 supplies): AI purchases and convoys never take the HQ below it (player requests still may, first come).
- Convoys load only `stock - reserve`, skip when that is < a minimum load (e.g. 60); log the skip once.
- Spending when at the AI cap: no purchase attempts that will be refused (check the cap before paying/planning).
- P10: verify the idle stand-down: log once per minute per side `CLW: idle check <side>: candidates n, oldest <s> s` (debug level) and fix if it never sees candidates.
- Files: `CLW_RuleCommander.c`, `CLW_Logistics.c`, `CLW_BaseWatch.c`, `CLW_ActionsCore.c` (buy gate), `CLW_WarMode_ShortConflict.c` (param).

### WP7 (me) merge + DS check
Merge order WP2, WP1, WP3, WP6, WP5, WP4 (WP4 touches the runtime last). One DS run on GM Everon with the full packs: compile clean, takeover steps ok,
log checks for every WP (journal + ledger/tries in GET /api/state with the fake provider, names in the registry log, base distances, no medical, depot list size, kit lines, FOB/convoy markers, reserve floor, idle check),
then the rematch loop (time limit 2 min, own client). Then the owner test (section 6).

### WP8 Modern vehicles test (last)
- Add vehicle packs to a new `_dedicated\packs-e2e-vehicles.txt` = `packs-e2e-full.txt` + M1Abrams `5D1880C4AD410C14`, M2 Bradley `63120AE07E6C0966`, JLTV `5C721177A220B42F`,
  WCS_Vehicles `615A80027C5C9170`, WCS_Helicopters `6146FAED5AAD7C55` + their dependencies SpaceCore `5E389BB9F58B79A6`, WCS_Armament `629B2BA37EFFD577`,
  WCS_VehicleLock `61BA4EB5C886D396`, WCS_Core `64610AFB74AA9842` (dependencies before dependants; junctions in `_dedicated\addons` from `C:\Users\Jan\Documents\My Games\ArmaReforger\addons`).
- Check: catalog build log (`CLW: catalog <side> ...`): which modern vehicles land in US/RHS_USAF and USSR/RHS_AFRF pools (kin), crew probe results (crewless ones only for players),
  AI buys modern armor (an RHS armor role finally exists?), depot list shows them, no new script errors, FPS. Owner: drive one, depot request, AI armour behaviour.

## 5. Open, not planned now
Physical supply crates on trucks; WCS `SCR_AISwitchMagazine` VM exceptions (third-party); GM persistence; RHS own compositions (none exist); mission header + Workshop publish.

## 6. Owner checklist for the round-3 test (client)
1 group names readable, garrison named `HQ Garrison`; 2 HUD shows own supplies; 3 base: spawn tent touches the command post, arsenal next to them, no medical tent;
4 depot lists side-pool vehicles; 5 FOB planned/building/done on the in-game map + dashboard with builder arrow, convoy arrow; 6 victory -> results -> back to setup, no kick;
7 dashboard journal view + memory settings (mode, review interval); does the HQ keep a plan, avoid repeating failed attacks, use flanks; 8 economy: HQ never drained to 0, convoys still run;
9 (WP8) modern vehicles at the depot and in AI use.
