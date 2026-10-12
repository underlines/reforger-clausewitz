# Loadouts and arsenals: plan (draft 2026-10-12, not started)

Research: `research/loadout-editors-arsenals.md`. Goal: vanilla, Bacon and WCS loadout editors work in Clausewitz without dependencies
(JXP and the Bacon forks come along for free), every side's arsenal has its full item set (RHS items for RHS-kin sides, never the enemy's), and later
loadouts can be managed from the web dashboard.

## Decision: one arsenal, no arsenal-type setting
All editors hook the vanilla `SCR_ArsenalComponent` / `ArsenalBox_Base.et` / `SCR_PlayerArsenalLoadout`. So we spawn vanilla-derived
arsenal boxes and make the item pool and loadout entries right; we never integrate a specific editor in code.

On the owner's either/or: neither "pick the detected arsenal mod" nor "all items everywhere". The **kin pool** below: each side's arsenal
shows its own faction catalog plus the catalogs of factions that are friendly to it (vanilla friendly lists, e.g. RHS_USAF <-> US), minus anything
from the enemy side. That is the data-driven version of "RU weapons only for RU".

## L1 - Arsenal at every HQ (later FOB)
- `Prefabs/Bases/CLW_Arsenal.et`: child of `ArsenalBox_Base.et` (keeps every editor's action), `SCR_FactionAffiliationComponent`, save type
  `FRIENDLY_AND_FACTION_ITEMS_ONLY` (needed for kin items). Prefab per side is data (scenario/side slot attribute, default `CLW_Arsenal.et`),
  so a pack's own arsenal prefab can replace it.
- `CLW_Bases` spawns it next to the HQ spawn point on a navmesh-free flat spot (reuse `CLW_Placement`); `Rekey` sets its faction with the
  rest of the base. Log: `CLW: arsenal side N faction X items=N`.
- Deploy menu: show the faction's `SCR_PlayerArsenalLoadout` (the saved kit) next to the role loadouts; auto spawn keeps skipping it.

## L2 - Kin item pool
- Hook at catalog-manager level (one choke point, so Bacon's cache, vanilla save checks and every editor see the same items):
  `modded SCR_EntityCatalogManagerComponent` -> `GetFilteredArsenalItems(.., faction, ..)` and `GetFactionArsenalItems(.., faction)` return the
  union over `CLW_ArsenalPool.KinOf(faction)`, de-duplicated by prefab, first faction's entry wins (keeps its cost/rank data).
  Only active while a Clausewitz war is running, so the hook does nothing in other scenarios that load the mod.
- `KinOf(f)`: mode from the scenario/war config, `OWN` | `FRIENDLY` (default) | `SIDE` (all playable non-enemy factions); plus per-faction
  overrides in the faction profile `.conf` (`m_aArsenalKin` add, `m_aArsenalExclude` prefab/label blacklist). Never a faction of the other side.
- Startup log per side: own count, added per kin faction, duplicates dropped.
- [verify] supply cost and rank of kin items (see research Open); fallback: copy the item's `SCR_ArsenalItem` data when it is missing in the own catalog.

## L3 - Loadout-slot coverage for every playing faction
- At war start, for each side faction that has no `SCR_PlayerArsenalLoadout` (or fewer slots than the richest faction, the WCS case), clone the
  template entries of a kin faction (or any faction) via `BaseContainer` copy, then set `m_sAffiliatedFaction`, the default prefab (the faction's
  rifleman from its role loadouts) and keep unknown attributes like `m_sWCSSlotId`. No compile-time reference to WCS/Bacon.
- Needs a small `modded SCR_LoadoutManager` to append entries at runtime (`m_aPlayerLoadouts` is protected). [verify] the deploy menu picks
  them up after start (it queries per menu open).
- Same pass can add the RHS role loadouts (PROGRESS known issue: editor loadout manager has only a rifleman for pack factions) by discovering
  `SCR_FactionPlayerLoadout` configs, if cheap.

## L4 - Test matrix (one Workbench session at the end, like M2)
New test project `dev/testpacks/ClausewitzEditors` (junctions as in `testpacks/README.md`). Runs: vanilla / +Bacon / +WCS_LoadoutEditor+WCS_Arsenal
/ +RHS+Bacon, US-vs-USSR and RHS_USAF-vs-RHS_AFRF. Check per run: editor action on the HQ arsenal, item list has kin items and no enemy
items, save + respawn with the saved kit from the deploy menu, supply cost not 0, logs clean.

## L5 - Vision: loadouts from the web dashboard
- Common currency: the vanilla loadout string (`SCR_PlayerArsenalLoadout.ReadLoadoutString` JSON), plus metadata (name, faction, role, cost,
  author identity). Stored by the sidecar (`dev/tools/loadouts/` or the server profile).
- Step 1 (cheap): **publish from game**. "Send kit to HQ" action on the arsenal posts the current kit JSON to the sidecar; the dashboard lists,
  renames, assigns role/faction and marks kits as server presets. The game pulls presets (existing REST channel) and exposes them as
  `CLW_ServerLoadout : SCR_PlayerArsenalLoadout` entries in the deploy menu and as "Load preset" on the arsenal.
- Step 2: per-player kits keyed by `BackendApi.GetPlayerIdentityId` (same key Bacon uses); dashboard login needed first on a public server.
- Step 3 (expensive): full editing in the browser. Needs the item catalog with slot compatibility exported by the game (like `/world`), validated
  in game by `ApplyLoadoutString` + the arsenal's own item check before use.
- Tie-ins: HQ/LLM can attach a recommended kit to a mission (AT kit for an anti-armour mission); kit cost counts against faction supplies.

## Order and size
L1 + L2 first (the user-visible win, one sprint), L3 next, L4 at the end of the sprint, L5 after M3. Implementation by subagents, code only,
one e2e at the end.
