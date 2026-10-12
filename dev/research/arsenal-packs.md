# Workshop gear packs (WCS, GRS) in the arsenal / loadout editor

Date 2026-10-12. Tags: [SRC] read from a packed mod (`data.pak` via the MCP PAK reader), [WEB] Workshop page read 2026-10-12, [LOG] our logs, [hyp] untested.
Game 1.8.0.13 (built 2026-08-21). Local Workshop folder: `C:\Users\Jan\Documents\My Games\ArmaReforger\addons`.

## 1. How the packs expose their items [SRC]
Neither WCS nor GRS adds a faction. They put their items into the ITEM catalogs of the VANILLA factions (US, USSR, FIA, CIV) by shipping files at
the vanilla path (merged onto the vanilla file) or by overriding the faction config:

| Pack | Mechanism |
|---|---|
| `WCS_Arsenal` 8.3.0 (20 deps, 05.10.2026, game 1.8.0.13 [WEB]) | overrides `Configs/Factions/{US,USSR,FIA}.conf` (+ `_Campaign`): `m_aEntityCatalogs` gets `WCS_InventoryItems_EntityCatalog_<F>.conf` and `WCS_Vehicles_...`; these list ~190 per-category multilists (`Weapons_Rifles_{Custom,Reforger,WCS,RHS}.conf`, `Clothing_*`, `Ammunition/*`, `Attachments/*`, `Equipment`, `Throwables`, `Explosives`, `Vehicles/*`). So WCS NATO/RU gear, WCS clothing and the RHS items reach the vanilla US / USSR / FIA arsenals. Also `GameMode_Base.et` (loadout-save blacklist holder), `GameMode_Campaign.et`, `CampaignFactionManager.et`, CIV catalogs, call-in configs. "Catalogs tailored for the WCS servers". |
| `WCS_Armament` 8.3.0 | partial `Configs/EntityCatalog/{US,USSR,FIA,CIV}/*_EntityCatalog_*.conf` (Characters, InventoryItems, Vehicles, WeaponTripod): launchers, deployable parts, vehicle weapon variants |
| `WCS_Core` 8.3.0 | campaign / XP / logistics scripts (95 modded classes), `Factions/US_Campaign.conf`, ranks. Only needed as dependency of WCS_Arsenal |
| `WCS_NATO`, `WCS_RU`, `WCS_Weapons`, `WCS_Clothing(_Assets)`, `WCS_Attachments`, `WCS_Scopes` | prefabs/assets only, NO catalog files: nothing shows without an integration pack (this was the empty result of the last test) |
| `WCS_LoadoutEditor` | the loadout editor UI + `Configs/Actions/Arsenals/WCS_PlayerArsenalLoadout_{CIV,FIA,US,USSR}.conf` (per vanilla faction); items come from the faction catalogs |
| `WCSNatotoUS` 1.0.8 (game 1.6.0.119, 24.02.2026 [WEB]) | partial `Configs/EntityCatalog/US/InventoryItems_EntityCatalog_US.conf`: WCS NATO weapons into the vanilla US catalog; needs RHS. OUTDATED, overlaps WCS_Arsenal |
| `WCSNATOTOARSENAL` 1.0.0 (game 1.4.0.45, 07.07.2025 [WEB]) | same US file, sets `m_eRequiredRank PRIVATE` on those entries ("for Cia Easy"). OUTDATED, depends on WCSNatotoUS |
| `GRS-DevFramework` 1.0.55 | partial `EntityCatalog/US/InventoryItems_EntityCatalog_US.conf` + `USSR/...` with ~30 multilists `Configs/GRS_*.conf` (Apparel, Belts, Pouches, Plates, Patches, Radios, Rifles, Pistols, ...) and the inventory scripts (modded SCR_ArsenalManagerComponent, SCR_InventoryMenuUI, ...) |
| `GRS-USSR` 2.0.5 (game 1.7.0.54, 28.06.2026 [WEB]) | partial `EntityCatalog/USSR/InventoryItems_EntityCatalog_USSR.conf` with ~30 `GRS - USSR - *.conf` lists |
| `GRS-Watches` 1.0.5 | adds `GRS_Watches.conf` to the US catalog + watch scripts |
| `GRS-Footwear` 1.0.9 (game 1.6.0.119) | two tiny catalog fragments; its Workshop page returns 404 [WEB]. Not used |
| `GRS-Bundle` | empty meta pack (dependency list only) |

Consequence: with `WCS_Arsenal` + GRS loaded, the pack items sit in the catalogs of US and USSR (and FIA). Our kin pool (RHS_USAF lists `US` as
friendly, RHS_AFRF lists `USSR`) then hands them to the RHS arsenals. The enemy side stays out. So no per-pack code is needed for these packs.

## 2. Why the last test showed nothing / crashed
- WCS: `WCS_Arsenal` was not loaded; the other WCS packs have no catalog files (above). 
- GRS "Wrong GUID/name" and "Unknown keyword m_sAllowedMagazineWellClass" [LOG, logs_2026-10-12_10-59-23]: version skew inside the GRS family.
  The local copies are from 16-17.09.2026 except `GRS-DevFramework` (10.10.2026). The Workshop has a whole new set from 03-04.10.2026 [WEB]:
  DevFramework 1.0.55, Essentials **1.0.70** (local 1.0.69), Apparel **1.0.68** (1.0.67), Belts **1.0.59** (1.0.58), Modular **1.0.45** (1.0.44),
  Patches **1.0.27** (1.0.26), Watches 1.0.6 (1.0.5), Bundle 1.0.12 (1.0.11); all game 1.8.0.13. Findings in the packs:
  - DevFramework's US catalog inherits `GRS_Rifles/Pistols/Stocks/Foregrips/Magazines/Parts/Accesories.conf`, which exist in no installed pack (a GRS weapons pack that is not part of the bundle) => the "inherited-name" errors.
  - GRS-USSR references `GRS_Gustav_*`, `GRS_Flag_*` prefabs with GUIDs that the installed Essentials/Belts do not have (the 1.0.58/1.0.69 files predate them) => "Wrong GUID" for flags, plates, Gustav backpacks.
  - `GRS_V2_MFCR_*` (GRS-USSR "Added MFCR chest rigs") uses `m_sAllowedMagazineWellClass`, a field the older script classes do not have.
  The client then crashed (access violation replicating `Character_USSR_LAT`): the USSR loadout now holds half-resolved GRS items with a mismatched DevFramework storage layer. Cause not proven, but a stale GRS family is the first thing to remove [hyp].
- WCS_LoadoutEditor, WCS_Armament and GRS-DevFramework all `modded class` the same vanilla classes (SCR_ArsenalManagerComponent, SCR_InventoryMenuUI, SCR_InventorySlotUI, SCR_InventoryStorageManagerComponent, SCR_InventoryStoragesListUI, SCR_ResourcePlayerControllerInventoryComponent; Armament also SCR_RadioComponent, SCR_ArmorDamageManagerComponent, fuze actions). They stack by load order; a missing `super` in any of them would break the other [hyp]. No author documents WCS + GRS together.

## 3. Decision
1. Pack items reach the sides through the packs' own vanilla-faction catalogs. Recommended integration pack: `WCS_Arsenal` (current, 8.3.0). `WCSNatotoUS` / `WCSNATOTOARSENAL` are outdated and clash with it: do not load.
2. Our mod stays pack-independent. For packs whose items are in a catalog that is not on the friendly list (or which should be filtered by prefab path), a DATA rule file `Configs/CLW/ArsenalPackRules.conf` (`CLW_ArsenalPackRules`) is applied by `CLW_ArsenalPool` (see 4).
3. Pack list: `_dedicated/packs-e2e-gear.txt` (RHS + WCS_Arsenal closure + GRS-USSR closure), fallback `packs-e2e-gear-nogrs.txt`.

## 4. Rule format (implemented, `CLW_ArsenalPool.c`)
`Configs/CLW/ArsenalPackRules.conf`, class `CLW_ArsenalPackRules`, loaded on every machine (the arsenal list is computed on clients too):
```
CLW_ArsenalPackRules {
 m_aBlockedPathContains { "grs_gustav" }        // prefab path texts removed from EVERY arsenal (own items too), case-insensitive
 m_aRules {
  CLW_ArsenalPackRule {
   m_sName "civilian clothing"                    // log label
   m_bEnabled 0                                   // shipped disabled (example)
   m_aTargetFactions { "*" }                      // faction keys whose arsenal gets the items; empty or "*" = all
   m_aSourceFactions { "CIV" }                    // faction keys whose ITEM catalog is the source; "*" = all but the enemy (clients: friendly only)
   m_aPathContains { }                            // optional: only items whose prefab path contains one of these
   m_aPathExcludes { }                            // optional: never items containing one of these
  }
 }
}
```
Applied after the kin pool: own items first, then kin, then each applicable rule; duplicates by prefab path are dropped; the enemy side (server: scenario's other side) is never a source, and a rule cannot add items that no catalog has (an item needs an `SCR_ArsenalItem` entry in some faction catalog; packs without catalog files need their integration pack). Missing file = kin only (warning in the log).
Typical uses: a pack faction that lists no friendly vanilla faction (`m_aSourceFactions {"US"}` for target `"MYPACK"`), GRS-only for one side (`m_aSourceFactions {"US"}`, `m_aPathContains {"grs"}`), cutting broken pack items (`m_aBlockedPathContains`).

## 5. Log
`CLW: services arsenal pool <faction>: own=<n> kin=<n> rules=<n> blocked=<n> total=<n> | <kinFaction>=<added> ... [<rule>:<source>=<added>]`
Counted over the whole catalog (no filters) and only once the faction's own ITEM catalog has items (earlier calls are retried silently, up to 40, then the empty result is logged). Also `CLW: services arsenal pool rules: <n> active, <n> blocked path texts` once per session.

## 6. Open / risks
- Not run in game (lead tests on the DS): the rules file path is a plain path (like `DEFAULT_SCENARIO`); the `.meta` has a fixed GUID, the resource DB may need a Workbench refresh.
- `WCS_Core` (+ `WCS_Arsenal` `GameMode_Base/Campaign` prefab overrides) bring the WCS campaign systems into our world: untested next to `CLW_GameMode`.
- GRS must be refreshed from the Workshop before the next test (versions in section 2).
