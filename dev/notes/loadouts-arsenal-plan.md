# Loadouts and arsenals: plan (draft 2026-10-12, not started)

Research: `research/loadout-editors-arsenals.md`; earlier general idea note: `research/mod-agnostic-asset-discovery.md` (reviewed below).
Goal: vanilla, Bacon and WCS loadout editors work in Clausewitz without dependencies (JXP and the Bacon forks come along for free), every
side's arsenal has its full item set (pack items for kin sides, never the enemy's), factions from packs get proper role loadouts, and later
loadouts can be managed from the web dashboard.

## Decision: one arsenal, no arsenal-type setting
All editors hook the vanilla `SCR_ArsenalComponent` / `ArsenalBox_Base.et` / `SCR_PlayerArsenalLoadout`. So we spawn vanilla-derived
arsenal boxes and make the item pool and loadout entries right; we never integrate a specific editor in code.

On the owner's either/or: neither "pick the detected arsenal mod" nor "all items everywhere". The **kin pool** below: each side's arsenal
shows its own faction catalog plus the catalogs of factions that are friendly to it (vanilla friendly lists, e.g. RHS_USAF <-> US), minus anything
from the enemy side. That is the data-driven version of "RU weapons only for RU".

**Custom arsenals:** anything that asks `SCR_EntityCatalogManagerComponent` for faction items (all editors found, most arsenal mods) gets the pool
automatically, because the hook sits there. Arsenals with their own item lists (`m_OverwriteArsenalConfig` or their own configs) are out of scope:
no integration code per mod.

## Review of `mod-agnostic-asset-discovery.md` (earlier research)
Taken over:
- **Faction entity catalogs as the primary source**, overrides that *augment* discovery, content separate from doctrine. This is already how
  `CLW_ForceCatalog` works for groups/vehicles (discovery + classifier + `overrides.json`); items follow the same pattern.
- **Layered discovery with a prefab-scan fallback** for content that is in no catalog (standalone weapon packs, e.g. the ARMA-RY series). Feasible
  at runtime: `ResourceDatabase.SearchResources(SearchResourcesFilter{rootPath, fileExtensions, searchStr, tags, recursive})`, every addon ships a
  `resourceDatabase.rdb`. [verify on a packed dedicated server]
- **Filter list** for the scan (wrecks, test/prototype, `_base`/abstract prefabs, damaged and editor-only variants).

Rejected or changed:
- **One "HC Asset Registry" for everything:** no rewrite. `CLW_ForceCatalog` stays the registry for groups/vehicles; items get a small separate
  `CLW_ArsenalPool` that reuses its helpers and the overrides file. The LLM does not need item-level data (at most kit tags for missions, L5).
- **Loadouts built from doctrine templates** ("rifleman: assault_rifle, 7 mags, armor required" resolved against loose items): rejected. The hard
  part is compatibility (magazine wells, attachments, carrying capacity) and visual coherence (mixing camo from three packs). Factions already
  ship coherent kits: their **CHARACTER catalog prefabs carry `ROLE_*` labels** (`ROLE_RIFLEMAN, ROLE_ANTITANK, ROLE_MEDIC, ROLE_MACHINEGUNNER,
  ROLE_LEADER, ROLE_GRENADIER, ROLE_SHARPSHOOTER, ...`, verified on vanilla in `entity-catalog-classification.md`). A player role loadout is just
  `SCR_FactionPlayerLoadout` pointing at such a prefab. See L3.
- **Guessing the faction of an orphan prefab** from components or names: no reliable signal. Only one rule is data-driven: an orphan whose
  **prefab parent** (or ancestor) is in a faction's catalog belongs to that faction (camo/variant children). Everything else is "unassigned":
  available only in pool mode `SIDE`/`ALL` or through an override.
- **Prefab introspection for semantics** (tracked + cannon = MBT): already done for vehicles by the classifier. For items it is only needed
  to fill in the missing `SCR_ArsenalItem` type/mode of an orphan (weapon / magazine / clothing area from components).

New constraint the earlier note misses: **clients compute arsenal contents too** (`SCR_ArsenalComponent.DelayedOnArsenalUpdatedClient` ->
`GetAvailablePrefabs` -> `GetFilteredArsenalItems` runs on the client). The pool must give the same answer on every machine: inputs (pool mode,
side factions, overrides) replicated, the hook on clients deterministic, and the orphan scan either run identically on clients or its result
sent by the server. Same for the friendly/hostile flags `CLW_Sides` sets at war start: check they replicate.

## L1 - Arsenal at every HQ (later FOB)
- `Prefabs/Bases/CLW_Arsenal.et`: child of `ArsenalBox_Base.et` (keeps every editor's action), `SCR_FactionAffiliationComponent`, save type
  `FRIENDLY_AND_FACTION_ITEMS_ONLY` (needed for kin items). Prefab per side is data (scenario/side slot attribute, default `CLW_Arsenal.et`),
  so a pack's own arsenal prefab can replace it.
- `CLW_Bases` spawns it next to the HQ spawn point (reuse `CLW_Placement`; the battle-size layout moves HQs at war start, so spawn after
  `ApplyLayout`), `Rekey` sets its faction with the rest of the base. Log: `CLW: arsenal side N faction X items=N`.
- Deploy menu: show the faction's `SCR_PlayerArsenalLoadout` (the saved kit) next to the role loadouts; auto spawn keeps skipping it.

## L2 - Item pool (layered, at catalog-manager level)
- Hook: `modded SCR_EntityCatalogManagerComponent` -> `GetFilteredArsenalItems(.., faction, ..)` and `GetFactionArsenalItems(.., faction)` return
  the pool of the faction, de-duplicated by prefab; the first layer that has an item wins (keeps its cost/rank data). One choke point, so Bacon's
  cache, vanilla save checks and every editor see the same items. Only active while a Clausewitz war runs.
- Layers, in order:
  1. own faction ITEM catalog;
  2. non-faction (generic) ITEM catalogs of the catalog manager [verify whether vanilla already merges them];
  3. kin factions' ITEM catalogs (`KinOf(f)`);
  4. orphan scan (L2b, off by default): prefabs in no catalog, faction by catalogued ancestor, else unassigned;
  5. overrides (`overrides.json` gets an `items` section: `match`, `faction`, `enabled`, `type`, `cost`; scenario `m_aItemOverrides`).
- `KinOf(f)`: pool mode from the scenario/war config, `OWN` | `FRIENDLY` (default) | `SIDE` (all playable non-enemy factions) | `ALL`
  (sandbox, includes unassigned orphans); plus per-faction-profile `m_aArsenalKin` / `m_aArsenalExclude`. Never a faction of the other side
  except in `ALL`.
- Startup log per side: count per layer, duplicates dropped, orphans unassigned.
- [verify] supply cost and rank of kin items (see research Open); fallback: copy the item's `SCR_ArsenalItem` data when it is missing in the own catalog.

### L2b - Orphan scan (separate step, only if L2 test shows real gaps)
- `ResourceDatabase.SearchResources` over `Prefabs/Weapons`, `Prefabs/Characters/{Uniforms,HeadGear,Vests,Backpacks,...}`, `Prefabs/Items`,
  `.et` only, filter list above, minus every prefab already in a catalog. Inspect candidates (`Resource.Load` + `BaseContainer`, no spawning)
  for weapon / magazine / cloth / item components, synthesize `SCR_ArsenalItem` (needs a `modded SCR_ArsenalItem` setter).
- Cost: tens of thousands of prefabs with RHS+WCS. Run once per server start in slices (call queue), cache in `$profile:Clausewitz/item-scan.json`
  keyed by the loaded addon list + versions, send the result to clients rather than scanning there.

## L3 - Loadouts for every playing faction
- **Role loadouts:** for a side faction with no (or only a rifleman) `SCR_FactionPlayerLoadout`, create one per role from its CHARACTER catalog:
  pick one prefab per `ROLE_*` label (prefer `SIZE_M`, skip `ROLE_GUARD`, crew/pilot traits, de-duplicate camo variants like the vehicle
  classifier), name it from the prefab's UI info. Fixes the known issue "editor loadout manager has only a rifleman for pack factions"
  (PROGRESS) and covers RHS_ION, which has none.
- **Saved-kit slots:** for each side faction that has no `SCR_PlayerArsenalLoadout`, or fewer slots than the richest faction (the WCS case),
  clone the template entries of a kin faction (or any faction) via `BaseContainer` copy, then set `m_sAffiliatedFaction` and the default prefab
  (the rifleman above); unknown attributes like `m_sWCSSlotId` are kept. No compile-time reference to WCS/Bacon.
- Both need a small `modded SCR_LoadoutManager` to append entries at runtime (`m_aPlayerLoadouts` is protected); the loadout manager exists on
  clients too, so the additions must be made identically there (deterministic from replicated side factions) [verify the deploy menu reads them].

## L4 - Test matrix (one Workbench session at the end, like M2)
New test project `dev/testpacks/ClausewitzEditors` (junctions as in `testpacks/README.md`). Runs: vanilla / +Bacon / +WCS_LoadoutEditor+WCS_Arsenal
/ +RHS+Bacon, US-vs-USSR and RHS_USAF-vs-RHS_AFRF. Check per run: editor action on the HQ arsenal, item list has kin items and no enemy
items (on the server AND a client view), save + respawn with the saved kit from the deploy menu, role loadouts for RHS factions, supply cost
not 0, logs clean. L2b gets its own run with one standalone weapon pack (an ARMA-RY pack).

## L5 - Vision: loadouts from the web dashboard
- Common currency: the vanilla loadout string (`SCR_PlayerArsenalLoadout.ReadLoadoutString` JSON), plus metadata (name, faction, role, cost,
  author identity). Stored by the sidecar (`dev/tools/loadouts/` or the server profile).
- Step 1 (cheap): **publish from game**. "Send kit to HQ" action on the arsenal posts the current kit JSON to the sidecar; the dashboard lists,
  renames, assigns role/faction and marks kits as server presets. The game pulls presets (existing REST channel) and exposes them as
  `CLW_ServerLoadout : SCR_PlayerArsenalLoadout` entries in the deploy menu and as "Load preset" on the arsenal.
- Step 2: per-player kits keyed by `BackendApi.GetPlayerIdentityId` (same key Bacon uses); dashboard login needed first on a public server.
- Step 3 (expensive): full editing in the browser. Needs the item pool with slot compatibility exported by the game (like `/world`), validated
  in game by `ApplyLoadoutString` + the arsenal's own item check before use.
- Tie-ins: HQ/LLM can attach a recommended kit to a mission (AT kit for an anti-armour mission; kit tags come from the role labels of L3);
  kit cost counts against faction supplies.

## Order and size
L1 + L2 (layers 1-3, 5) + L3 first (one sprint), L4 at the end of that sprint, L2b only if the test shows real gaps, L5 after M3.
Implementation by subagents, code only, one e2e at the end.
