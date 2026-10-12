# Loadout editors and arsenals (research, 2026-10-12)

Sources: unpacked Workshop paks from `Documents\My Games\ArmaReforger\addons` (Bacon Loadout Editor 1.6.2, WCS_LoadoutEditor 8.3.0,
WCS_Arsenal 8.3.0, JXPLoadoutEditor 1.1.20, GM Persistent Loadouts 1.0.18, RHStoVanillaArsenals, WCSNATOTOARSENAL) and vanilla
`scripts/Game/Components/Arsenal/SCR_ArsenalComponent.c`. [SRC] = read in the source.

## How vanilla arsenals get their items [SRC]
- `SCR_ArsenalComponent` on the box (`Prefabs/Props/Military/Arsenal/ArsenalBoxes/ArsenalBox_Base.et`, faction children in `US/`, `USSR/`, `FIA/`).
- Items: `GetFilteredArsenalItems()` -> `SCR_EntityCatalogManagerComponent.GetFilteredArsenalItems(types, modes, arsenalGameModeType, GetAssignedFaction())`,
  i.e. the ITEM entity catalog of the box's faction, filtered by `m_eSupportedArsenalItemTypes/Modes`. An `m_OverwriteArsenalConfig` replaces that list.
- Faction: `SCR_FactionAffiliationComponent` on the box (current faction, else default when `m_bGetDefaultIfNoFaction`); `OnFactionChanged` refreshes
  the arsenal, so re-keying a box at runtime works.
- Save types: `SAVING_DISABLED`, `IN_ARSENAL_ITEMS_ONLY`, `FACTION_ITEMS_ONLY`, `FRIENDLY_AND_FACTION_ITEMS_ONLY` (vanilla already knows
  "friendly factions' items"), `NO_RESTRICTIONS`.
- Saved kit: `SCR_PlayerArsenalLoadout` (one per faction in the loadout manager) + `SCR_PlayerArsenalLoadout.ReadLoadoutString / ApplyLoadoutString`
  (JSON of the character's inventory). This JSON is the common format that Bacon and JXP also use.

## The editors: all piggyback the vanilla arsenal [SRC]
| Mod | Hooks | Item source | Storage | Deploy menu |
|---|---|---|---|---|
| Bacon Loadout Editor | action on `ArsenalBox_Base.et`; `Bacon_GunBuilder_LoadoutStorageComponent` on `GameMode_Base.et` (our `CLW_GameMode.et` inherits it); modded `SCR_ArsenalManagerComponent` | `arsenal.GetFilteredArsenalItems()`; server cache per faction from `catalogManager.GetFactionArsenalItems(faction)` (supply/rank info) | JSON `$profile:/BaconLoadoutEditor_Loadouts/1.6.0/<identityId>` with 5 slots per faction; plus 10 admin "SERVER_LOADOUTS" shared by all players | needs one `SCR_PlayerArsenalLoadout` template per faction in the loadout manager (logs an ERROR if none) |
| WCS_LoadoutEditor | overrides `LoadoutManager_Editor.et` (our `CLW_LoadoutManager.et` inherits it) with 4 slot entries for US and USSR (2 for FIA) through a modded `SCR_PlayerArsenalLoadout` with `m_sWCSSlotId`; modded inventory/arsenal UI, save/load actions | vanilla arsenal storage UI | per player UID, up to 64 per faction, synced to the client | slots appear as loadouts in the deploy menu. **RHS factions get no WCS slots** (only US/USSR/FIA confs) |
| JXPLoadoutEditor (game 1.8) | action on `ArsenalBox_Base.et`, modded deploy menu, `SCR_PlayerController` | `arsenal.GetFilteredArsenalItems()` | own service + "KitShare" between players | own `JPLE_DeploySavedLoadout : SCR_BasePlayerLoadout` |
| GM Persistent Loadouts | modded `SCR_ArsenalManagerComponent`, `SCR_LoadoutRequestUIComponent` | vanilla | persistent vanilla arsenal loadout | vanilla |

Other editors on the Workshop (LHS, PLG Loadout Editor Rank, TOLEYN, Deploy Menu Loadout Editor) follow the same pattern
(Bacon forks or vanilla-arsenal based).

**Consequence:** we do not need an "arsenal type" setting or per-mod integrations. If our bases spawn a box that inherits `ArsenalBox_Base.et`
with a working faction, and the loadout manager has `SCR_PlayerArsenalLoadout` entries for the playing factions, every one of these editors
attaches to it by itself.

## "Item to arsenal" packs [SRC]
`RHStoVanillaArsenals`, `WCSNATOTOARSENAL`, `WCS_Arsenal` simply override the vanilla faction catalogs
(`Configs/EntityCatalog/US/InventoryItems_EntityCatalog_US.conf` etc.) to add RHS/WCS items to US/USSR. Players install them because the
arsenal only shows the box faction's own catalog. RHS factions bring their own catalogs (`RHS_USAF`, `RHS_AFRF`, `RHS_ION`), and their
`FactionManager` entries list vanilla kin as friendly (`RHS_USAF` friendly to `US`, `RHS_AFRF` to `USSR`; see `modpacks-factions-testing.md`).

## Clausewitz today
- No arsenal anywhere: bases (`CLW_Bases.c`) and the world have no arsenal box.
- `CLW_LoadoutManager.et` (child of `LoadoutManager_Editor.et`): 6 role loadouts each for US and USSR; other factions only have the editor
  Rifleman + its `SCR_PlayerArsenalLoadout`. `CLW_Sides.GetSpawnLoadouts` / `CLW_SpawnLogic` skip arsenal loadouts for auto spawn (correct).
- Sides are re-keyed at war start (`CLW_Sides`, `CLW_Bases.Rekey`): an arsenal box must be re-keyed the same way.

## Open / [verify]
- Supply cost of an item that only exists in a kin faction's catalog (arsenal manager / WCS `GetCharacterItemSupplyCost(.., faction)` look up
  the player's faction): could be 0, or the item could be refused.
- WCS slot entries for factions WCS doesn't know (RHS): does WCS accept a cloned `SCR_PlayerArsenalLoadout` with a new faction + `m_sWCSSlotId`?
- Bacon's per-faction server cache uses `GetFactionArsenalItems(faction)` from the catalog manager, not the arsenal: kin items must be added
  at catalog-manager level, or Bacon shows them without cost/rank data.
