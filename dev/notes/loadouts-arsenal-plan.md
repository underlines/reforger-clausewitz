# Loadouts and arsenals: plan (draft 2026-10-12, rev. 2026-10-14 after owner review)

**The one plan for arsenals, spawn-screen loadouts, the player's custom loadout and AI squad loadouts.**
Research: `research/loadout-editors-arsenals.md`, `research/arsenal-packs.md`, `research/entity-catalog-classification.md` (labels),
`research/mod-agnostic-asset-discovery.md` (reviewed below).

Status 2026-10-14: A1 (arsenal at HQ/FOB) and A2 layers 1 + 3 + pack rules (`CLW_ArsenalPool`, `Configs/CLW/ArsenalPackRules.conf`) shipped
in M3/M4 (see PROGRESS). Everything from R0 on is not started.

## Owner requirements
- Every side's arsenal has its full item set (pack items for kin sides, never the enemy's); vanilla, Bacon and WCS loadout editors work
  without dependencies (2026-10-12).
- 2026-10-14:
  1. Spawn screen per faction: **7 server loadouts** (managed in the dashboard loadout editor) + the player's **one custom saved loadout**.
  2. No load/save in the HQ menu (would be cheating). The custom loadout is saved and loaded **only at an arsenal** (any arsenal if possible).
     The 7 server loadouts are NOT loadable at arsenals, only spawnable.
  3. Dashboard loadout editor: sees every player's saved custom loadout (by faction), copies one into the library as a new server loadout.
     Library per faction: unlimited, **max 7 active**.
  4. Admin assigns server loadouts to squads; AI squads get proper roles/loadouts. Stay close to official Reforger concepts where it does not hurt.
  5. Server loadouts are set up once, rarely changed: applied at war/session (re)start, no live changes. Dashboard gets a restart button.
  6. Dashboard gets a proper admin login (persists across tabs, password changeable, default in README).

## Decisions
- **One arsenal, no arsenal-type setting.** All editors hook the vanilla `SCR_ArsenalComponent` / `ArsenalBox_Base.et` /
  `SCR_PlayerArsenalLoadout`. We spawn vanilla-derived arsenal boxes and make the item pool and loadout entries right; we never integrate a
  specific editor in code. No custom arsenal UI either: items, prices and faction gate come from the pool hook, which the vanilla UI, WCS and
  Bacon all use.
- **Kin pool**, not "pick the detected arsenal mod" or "all items everywhere": each side's arsenal shows its own faction catalog plus the
  catalogs of factions friendly to it (vanilla friendly lists, e.g. RHS_USAF <-> US), minus anything from the enemy side.
- **Custom arsenals:** anything that asks `SCR_EntityCatalogManagerComponent` for faction items gets the pool automatically. Arsenals with
  their own item lists (`m_OverwriteArsenalConfig` or own configs) are out of scope: no integration code per mod.
- **Kits come from factions, not doctrine templates.** Building "rifleman: assault rifle, 7 mags" from loose items is rejected (magazine
  wells, attachments, capacity, camo coherence). Factions ship coherent kits as CHARACTER catalog prefabs with `ROLE_*` labels; server
  loadouts are real kits saved in game (vanilla loadout string).
- **Official roles, our squad types.** Characters carry `ROLE_*` labels (`LEADER, RIFLEMAN, MACHINEGUNNER, ANTITANK, GRENADIER, SHARPSHOOTER,
  MEDIC, RADIOOPERATOR, SCOUT, AMMOBEARER, SAPPER, GUARD`), packs too (RHS: ROLE_SCOUT 21, ROLE_SHARPSHOOTER 16). Official squad types barely
  exist (`GROUPTYPE_*` almost unused, `SCR_EGroupType` only on ambient patrols), but group prefabs' member slots (`m_aUnitPrefabSlots`) are
  role-labelled characters (`Group_US_Team_LAT` holds AT soldiers). Squad types are our `CLW_ForceKind`.
- **Load at the arsenal is ours.** Vanilla only saves (`SCR_ArsenalManagerComponent.SetPlayerArsenalLoadout` -> the spawn screen's saved
  loadout). Public servers get "load" from Bacon (5 slots/faction + 10 admin server loadouts) or WCS_LoadoutEditor (deploy-menu slots). We build
  a small save/load and coexist with both.
- **Client determinism.** Clients compute arsenal contents too (`SCR_ArsenalComponent.DelayedOnArsenalUpdatedClient` -> `GetAvailablePrefabs`
  -> `GetFilteredArsenalItems`), and the loadout manager list exists on every client. Pool inputs and spawn-screen entries must be built from
  data every machine has (friendly lists, rules file, replicated side factions). Hence fixed slot entries per side (7 server + 1 custom),
  filled from replicated data, instead of entries added at runtime; that is also the technical reason the cap of 7 is good.

Review of `mod-agnostic-asset-discovery.md`: taken over are faction catalogs as the primary source with augmenting overrides, layered
discovery with a prefab-scan fallback (A2b), and its filter list. Rejected: one "HC Asset Registry" for everything (`CLW_ForceCatalog` stays
the group/vehicle registry, items get `CLW_ArsenalPool`), doctrine templates (above), guessing an orphan prefab's faction from components or
names (only rule: a catalogued prefab ancestor decides; else "unassigned"), item semantics by introspection (only to fill a missing
`SCR_ArsenalItem` type/mode of an orphan).

## A1 - Arsenal at every HQ and FOB (shipped)
`CLW_Arsenal.et` (child of `ArsenalBox_Base.et`, keeps every editor's action, save type `FRIENDLY_AND_FACTION_ITEMS_ONLY`) as a base service
composition; prefab per side is data, RHS prop sets put RHS boxes at RHS bases; FOB kit box. Kit cost against nearby containers.

## A2 - Item pool at catalog-manager level (shipped: layers 1, 3 + pack rules)
- Hook: `modded SCR_EntityCatalogManagerComponent` -> `GetFactionArsenalItems` and the ITEM `GetEntryWithPrefabFromFactionCatalog` (price and
  check). Only active while a Clausewitz war runs. Log prefix `CLW: services arsenal pool`.
- Shipped layers: own faction ITEM catalog, kin factions' ITEM catalogs, `ArsenalPackRules.conf` (catalog X items go to faction Z, path filters,
  global block list). Rules never pull the enemy side.
- Open, only if a test shows the need: layer 2 generic (non-faction) ITEM catalogs [verify whether vanilla merges them]; item overrides in
  `overrides.json` (`items`: `match`, `faction`, `enabled`, `type`, `cost`); pool mode `OWN | FRIENDLY (default) | SIDE | ALL`.

### A2b - Orphan scan (only if tests show real gaps)
- `ResourceDatabase.SearchResources` over `Prefabs/Weapons`, `Prefabs/Characters/{Uniforms,HeadGear,Vests,Backpacks,...}`, `Prefabs/Items`,
  `.et` only, minus catalogued prefabs and the filter list (wrecks, test/prototype, `_base`, damaged, editor-only). Inspect without spawning,
  synthesize `SCR_ArsenalItem` (needs a `modded SCR_ArsenalItem` setter). Faction = catalogued ancestor, else unassigned.
- Tens of thousands of prefabs with RHS+WCS: once per server start in slices, cache `$profile:Clausewitz/item-scan.json` keyed by addon list +
  versions, result sent to clients (no client scan). [verify `SearchResources` on a packed DS]

## R0 - Investigate + spike (first, one DS session)
1. **Why are squad members identical today?** Log, per spawned group, slot prefabs + their `ROLE_*` labels: our spawn path (player squad
   recruits in `CLW_PlayerSquad`, cargo groups in `CLW_ForceSpawner`) duplicating one prefab, or pack groups with identical slots.
2. **AI with an applied kit:** spawn a group on the DS, `ApplyLoadoutString` per member server-side right after spawn. Check: primary fires,
   grenadier uses the GL, AT fires and reloads, magazines recognised, gear visible on a client, nothing dropped. Try before/after AI init.
   If unreliable: AI use role prefabs only (R2), server loadouts stay player-only. Either way squads get proper roles.
3. Vanilla saved kit: persisted across restarts or per session only? Arsenal action class used for "Save"; can we add actions to every
   `ArsenalBox_Base.et` child (prefab override in our addon vs. a script hook on `SCR_ArsenalComponent`); how WCS/Bacon actions look next to ours.
4. Side-resolved slot entries: does a loadout entry that resolves its faction at runtime satisfy the deploy menu, WCS and Bacon (Bacon logs an
   ERROR without one `SCR_PlayerArsenalLoadout` per faction)? Does the preview show the applied kit or only the base prefab (cosmetic)?
5. Restart path: does `CLW_WarRuntime.RematchRestart` (war restart in the running session) let clients see changed slot names/kits, or is a
   session restart needed (find the API for a scenario/session restart from script)?

## R1 - Player custom loadout (one per player per faction)
- Spawn screen: one `SCR_PlayerArsenalLoadout`-type entry per side in `CLW_LoadoutManager.et`, faction resolved from the side at runtime (R0.4).
  Covers pack factions without their own entry (RHS_ION) without cloning entries per faction; WCS slot entries stay as they are.
- Arsenal actions "Save my loadout" / "Load my loadout" on every arsenal: our HQ/FOB arsenals for sure; all `ArsenalBox_Base.et` children
  (vanilla, RHS, WCS boxes) through the mechanism R0.3 picks.
- Save = the vanilla saved kit (`SetPlayerArsenalLoadout`), plus our copy in `$profile:Clausewitz/loadouts.json` keyed by
  `BackendApi.GetPlayerIdentityId` + faction, so it survives restarts and the dashboard sees it. Restored into the vanilla slot on connect.
- Load at the arsenal: replace the living player's gear with the saved kit; costs supplies like taking the items (`ComputeSuppliesCost`), old
  gear is removed (not dropped), only items of the side's current pool (others dropped -> warning to the player).
- Coexistence: with WCS/Bacon loaded their own slots and actions stay; ours are extra. No code reference to either mod.

## R2 - Role picker (official roles, no config needed)
- Per faction and `ROLE_*`: one CHARACTER catalog prefab (prefer `SIZE_M`, skip `ROLE_GUARD` and crew/pilot traits, de-duplicate camo
  variants like the vehicle classifier), named from its UI info. Kin factions fill missing roles.
- Consumers: default content of the 7 spawn slots (R3) and AI squad members by role (R4). Fixes "editor loadout manager has only a rifleman
  for pack factions" (PROGRESS known issue).

## R3 - Server loadouts (library, 7 spawn slots)
- **Library** (sidecar, per faction, unlimited): `{id, name, role (ROLE_*), faction, active, order, loadout (vanilla loadout string),
  basePrefab, items [prefabs], addons [loaded at save], gameVersion, createdFrom {identityId}, status}`. Max 7 active per faction, `order` =
  slot order on the spawn screen. Source: copy of a player's custom loadout (R1); later browser editing.
- **Game side:** `CLW_LoadoutManager.et` gets 7 `CLW_ServerLoadout` slot entries per side, replacing today's 6 static US/USSR role loadouts.
  Each slot resolves its faction from the side at runtime, shows name + base prefab (replicated) and applies the loadout string in
  `OnLoadoutSpawned` on the server (like `SCR_PlayerArsenalLoadout`). Strings never go to clients.
- **Content per slot at war start:** the active library entry for that faction/slot if valid; else the R2 role prefab for the default role
  order `leader, rifleman, machinegunner, antitank, grenadier, medic, sharpshooter`; unused slots hidden. So the spawn screen is never empty.
- The game reads `$profile:Clausewitz/loadouts.json`, which the sidecar writes; the game never needs the sidecar running.
- Spawn cost (owner 2026-10-14): spawning with a server loadout costs supplies like an arsenal kit (vanilla loadout cost), switchable on the
  setup screen (own toggle `serverKitCost` next to `kitCost`, default on). AI never pays per kit.

## R4 - Squad roles and loadouts (AI)
Zero-config default first, admin control on top:
1. **Default (official):** each member keeps its group prefab's slot role (`ROLE_*` label of the slot character). Pack groups keep their
   designed composition.
2. **Squad-type compositions** for groups whose slots carry no role or are all identical (the current "all the same" case), and wherever the
   admin overrides: per `CLW_ForceKind` (infantry, recon, mech, motorized, ...) an ordered role list, shipped defaults in
   `Configs/CLW/SquadCompositions.conf`, e.g. infantry `leader, machinegunner, antitank, grenadier, rifleman, medic, rifleman...`, recon
   `leader, sharpshooter, scout, radiooperator`. Longer than the group: truncated after the leader; shorter: the last role repeats.
3. **Role -> kit:** a member with role R gets an active server loadout of its faction with role R (several -> random pick); none -> the R2
   role prefab; neither -> the slot's own prefab. The admin can pin specific server loadouts to a squad type (ordered list, same slot
   semantics), which wins over role matching.
- Members spawn as the R2 role prefab (official character, correct AI setup), then the kit is applied if R0.2 proved it reliable.
- Log once per group: `CLW: squad roles <unitId> <kind> roles=[...] kits=[...] fallback=N`.

## R5 - Dashboard: admin login, loadout editor, restart
- **Admin login:** one admin account. Password hashed (scrypt + salt) in `sidecar-config.json`; default password in the README, with a
  "change password" banner while the default is set. Session = random token in an `HttpOnly; SameSite=Strict` cookie, stored server-side in a
  file so it survives new tabs and sidecar restarts; 30 days, logout clears it; login rate-limited. All dashboard pages and every write
  endpoint require it. The game's own channel (mod <-> sidecar) gets a shared token from the server config instead. HTTPS when exposed beyond
  localhost (reverse proxy, documented in README).
- **Loadouts tab:** per faction: players' custom loadouts (player name, items, addons, status) with "Copy to library"; library table (name,
  role, active toggle with the 7-cap enforced, order, status, missing items/addons); squad-type compositions editor and per-type pinned kits.
- **Apply = restart:** "Restart war" sends the restart through the existing channel -> `RematchRestart`, which re-reads `loadouts.json`; if
  R0.5 shows the spawn screen needs a new session, the button does a session restart instead. Changing mod packs is never possible from here
  (server restart with a new addon list); the dashboard only flags it.

## R6 - Validation and warnings (mod pack / faction changes)
- At war start, per library entry and custom loadout: faction exists, every item prefab loads (`Resource.Load(..).IsValid()`), items in the
  faction's pool, loadout string parses (store `format`/`gameVersion`, never crash).
- Status ok / degraded (missing items dropped, kept if the primary weapon survives) / invalid (slot falls back to R2, squads to role prefabs) /
  inactive (faction not in this war).
- In game: one system message per faction at war start ("2 server loadouts unavailable: mod pack changed, defaults used"); a player whose
  custom loadout is degraded is told at the arsenal. Dashboard: badge on the tab + red rows with missing prefabs and addons (`addons` at save
  vs loaded now). Log: `CLW: loadouts validate <faction> ok=N degraded=N invalid=N inactive=N`.

## Test matrix (DS + one client, end of each sprint)
Packs: vanilla / +Bacon / +WCS_LoadoutEditor+WCS_Arsenal / RHS + WCS gear + WCS Loadout Editor (`packs-e2e-full.txt`); US-vs-USSR and
RHS_USAF-vs-RHS_AFRF. Per run: arsenal lists kin items and no enemy items (server AND client view), supply cost not 0, save + load custom
loadout at an HQ arsenal and a third-party box, respawn with it, 7 slots on the spawn screen (defaults, then configured), squads with mixed
roles and (if R0.2) server kits, restart button applies changes, logs clean. Once with RHS removed after configuring RHS kits: warnings in game
and on the dashboard. A2b gets its own run with one standalone weapon pack (an ARMA-RY pack).

## Risks / open
- R0.2 decides whether AI get server kits or only role prefabs.
- R0.3 adding actions to third-party arsenal boxes; fallback: only our HQ/FOB arsenals (and RHS prop-set boxes we spawn) have them.
- WCS slot entries add spawn-screen rows we do not control (US/USSR/FIA only).

## Later
- Kit editing in the browser (needs the item pool with slot compatibility exported by the game; validated in game before use).
- HQ/LLM recommends a server loadout for a mission by role; soldier tablet (VISION 12a) shows the player's own loadout.

## Order and size
R0 (one DS session) -> R2 + R4 default (roles, no dashboard) -> R1 -> R3 + R5 + R6; A2 open items and A2b only when a test shows the need.
R2/R4 alone already fixes "all members identical". Subagents code only on disjoint files (game scripts / sidecar / dashboard); the test
matrix runs once per sprint.
