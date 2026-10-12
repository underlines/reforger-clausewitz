# Vehicle depot research (long custom vehicle list)

Date: 2026-10-13. Current game version seen on Workshop: 1.8.0.x.

## 1. How vanilla SCR_CatalogEntitySpawnerComponent works (read from BI Script-Diff, main)

Sources:
- https://raw.githubusercontent.com/BohemiaInteractive/Arma-Reforger-Script-Diff/main/scripts/Game/UserActions/SCR_CatalogSpawnerUserAction.c
- https://raw.githubusercontent.com/BohemiaInteractive/Arma-Reforger-Script-Diff/main/scripts/Game/Components/Spawner/CatalogSpawner/SCR_CatalogEntitySpawnerComponent.c
- https://raw.githubusercontent.com/BohemiaInteractive/Arma-Reforger-Script-Diff/main/scripts/Game/Components/Spawner/CatalogSpawner/SCR_SpawnerRequestComponent.c
- API: https://community.bistudio.com/wikidata/external-data/arma-reforger/ArmaReforgerScriptAPIPublic/interfaceSCR__CatalogEntitySpawnerComponent.html
- Wiki: https://community.bistudio.com/wiki/Arma_Reforger:Action_Context_Setup

Findings that explain "200 actions, no F prompt" (ranked by likelihood; none is a documented hard count limit, I found NO documented max number of actions per ActionsManagerComponent/context):

1. Component doc: "Requires ActionManager with enough SCR_CatalogSpawnerUserAction attached (they cannot be generated through script) and SCR_EntitySpawnerSlotComponent in hierarchy or vicinity". Without a free slot of the right type, `CanBePerformedScript` returns false (reason "BuildBlocked"); with NO slot at all `GetFreeSlot` returns null. Locked is shown (gamepad) but on KB/M locked actions are not shown or not usable.
2. `CanBeShownScript` hides EVERY action unless ALL hold: `m_EntitySpawner` found (Init walks owner + parents for the component), `m_EntityData` set, user is `SCR_ChimeraCharacter` whose `GetFaction() == m_EntitySpawner.GetFaction()` (faction comes from the spawner's FactionAffiliation, so an entity with no/other faction shows nothing), and `GetRequestState != NOT_AVAILABLE`.
3. `GetRequestState` returns NOT_AVAILABLE if the entry is not in `m_aAssetList`. `m_aAssetList` is built ONLY from `faction.GetFactionEntityCatalogOfType(m_aCatalogTypes)` filtered by `m_aAllowedLabels`/`m_aIgnoredLabels` (+ `m_bNeedAllLabels` default true = entry needs ALL allowed labels) and requiring `SCR_EntityCatalogSpawnerData` on the entry. Our merged list is only seen if it lives in the faction's SCR_EntityCatalog (catalog type VEHICLE) of the spawner's faction; a custom list in our own conf is invisible to this component. Empty list -> `AssignUserActions` returns early -> nothing shown.
4. Index mapping gotcha: `AssignUserActions` iterates ALL actions in `ActionsManagerComponent.GetActionsList()` (not only catalog ones) and assigns `m_aAssetList[i]` to the action at list position i; extra actions beyond the asset count get `SetSpawnerData(null)` (hidden). A leading non-catalog action (build action, door, etc.) shifts the mapping by one. Request uses `GetActionID()` as the index into the same list (`RPC_DoRequestCatalogSpawn(index)`), so IDs and list positions must coincide.
5. Supplies: if `m_bSuppliesConsumptionEnabled`, needs SCR_ResourceComponent with SUPPLIES consumer or `m_iCustomSupplies` >= cost, else NOT_ENOUGH_SUPPLIES. In Conflict (`SCR_GameModeCampaign.GetInstance()`) `RankCheck` (unless `CanRequestVehicleWithoutRank`) and `CooldownCheck` also apply.
6. Wiki Action Context Setup: entity needs an `RplComponent`, a physical representation (MeshObject with valid mesh, RigidBody/"Model Geometry" so the interaction trace hits it), contexts need `Position` PointInfo (+radius/height) and UIInfo; actions need the context name in "Parent Context List" and visibility range. Wrong context position/radius (e.g. ctx not near the crosshair trace) = no prompt at all. Actions referencing a context name that does not exist are dropped.
7. Engine UI: the interaction prompt shows the first/selected action and a scroll list (mouse wheel) of the context's actions; there is no documented cap, but 200 `CanBeShown`/`CanBePerformed` script calls per frame on the client (each calling GetRequestState + slot lookup) is a real cost; vanilla max is ~20 (RequestTable_*). Treat >~30 actions as untested territory.

Conclusion: the one-action-per-vehicle design cannot work for a merged 50-170 vehicle list of mixed factions (needs catalog membership, faction equality, slots). Not a viable base.

### How newer Conflict versions present vehicles (1.6 - 1.8)
- Still action-per-vehicle at depots (Light Vehicle Depot / vehicle service build + SCR_CatalogSpawnerUserAction). Evidence: BI tracker ARGF-300 (Aug 2026, v1.7.0.54, "FIA light vehicle depot doesn't link supplies"): https://report.bistudio.com/issues/ARGF-300 ; FIA playable in Conflict since 1.7 (https://www.youtube.com/watch?v=BLpZqEVZ6Zw), Steam experimental notes (vehicle spawners for FIA): https://steamcommunity.com/app/1890860/allnews/ .
- I did NOT find any vanilla vehicle-request menu/list UI (`SCR_VehicleSpawnUI` etc. do not appear in the API). Vanilla only has the preview ghost + action list. I could not confirm anything beyond 1.7 notes (no changelog line found for a menu).

## 2. Workshop mods (search on reforger.armaplatform.com/workshop?search=...)

None provide "request a vehicle from a long faction-filtered, supply-cost list menu" for a Conflict-like mode. Closest:

| Mod | Author | ID | License | Version / date | How |
|---|---|---|---|---|---|
| Shop System | ekudmada | 5D2D1436D1FA5A13 | APL (Workshop); repo LICENSE = APL-SA | 1.0.18, game 1.3.0.130, last mod 2025-03-28 (GitHub has "1.3 hotfix", newer), 566k downloads | `ADM_ShopAction` (single "View Shop" action) opens `MenuBase` `ChimeraMenuPreset.ADM_ShopMenu`; list UI of merchandise (`ADM_MerchandiseVehicle` = prefab + PointInfo spawn pos), payment currency/item, server `Deliver` does `SpawnEntityPrefab` + clean-area check. Works with any prefab (modded OK). |
| TLL_Traders | XirisX | 6A453053DC960A4E | not read | no further updates (dev discontinued) | vehicle buy/sell traders, "vanilla and supported modded assets"; depends on its own economy; skip |
| Vehicle PC Shop v1.0 | KI-Modder | 69B4C248AB548815 | APL | 1.0.4, 1.7.0.54 | key shop only; deps Shop System, Building System 2, CarKey2; no spawn yet |
| bonz_FactionGarage | lannon_hycannus | 68C400549EB4B6CB | APL | 1.0.3, 1.8.0.10 | garage = park/unpark (despawn+respawn), 2 actions; not a catalog |
| GTG_Garage | Maj0rrush | 1CF644CD7D02B7F2 | APL | 1.0.5, 1.7.0.54 | persistent garage via sign, REST group source |
| Vehicle Spawn GM | Ferchook343 | E7FE48992A854D1F | APL | 1.0.1 (2026-08-29), 24.8 KiB, 0 deps | GM-oriented vehicle spawn; trivial, 25 downloads |
| EE_Simple Vehicle Spawner | EverElizaldeGaming | 66322C38B8DF9DAE | APL | 1.0.5, 1.4.0.53 | placed respawner with prefab list, no menu |
| ARCL Vehicle spawner | MinDBlanK | 671F16224900E252 | APL | 1.0.1, 1.6.0.95 | server-only delay respawner |
| STW Light Vehicle Spawner | Captain Sprocket | 6A268552E2B571AF | APL-ND | 1.0.133, 1.8.0.13 (2026-10-12) | AI armed light vehicle compositions; not a player menu |
| WKI_PlaygroundFramework | WallBangZ | 68F65E34B79F4C97 | not read | - | unchecked, possibly relevant |

Compatibility with modded vehicles: Shop System (prefab-based) yes; garages require "Editable" vehicles and resource-name lookup (bonz notes failures).

Not found: a faction-filtered depot with supply cost for arbitrary mod vehicles. Depending on Shop System for a game mode pulls a currency/economy model that does not match supply cost; the README asks for attribution and "do not rename class prefixes" if code is copied; recommended use is as dependency.

## 3. Open-source menu examples
- https://github.com/ekudmada/Reforger-Shop-System (APL-SA; 13 stars; files: `Scripts/Game/UI/Menu/ShopSystem/ADM_ShopUI.c`, `ADM_CustomPagingButton.c`, `ADM_ModdedChimeraMenu.c` (enum extension for the preset), layouts `UI/Layouts/Menus/ShopMenu/*.layout` (ShopMain/ShopList/ListBoxElement_ShopItem/Cart), `ADM_PlayerShopManager.c` (player-controller component doing the server RPC), `ADM_MerchandiseVehicle.c`). Pattern: ScriptedUserAction PerformAction (local effect, `HasLocalEffectOnlyScript`=true, `CanBroadcastScript`=false) -> `GetGame().GetMenuManager().OpenMenu(preset)` -> menu fills ListBox from a component's array -> RPC to server via a PlayerController component -> server validates + spawns.
- BI samples: https://github.com/BohemiaInteractive/Arma-Reforger-Samples (not inspected in detail).
- BI vanilla reference for the RPC path: `SCR_SpawnerRequestComponent.RequestCatalogEntitySpawn` / `RPC_DoRequestCatalogSpawn` (above).
- Seebie90 docs (short Enforce techniques, not read in detail): https://docs.seebie90gaming.com/
- Our own `CLW_HqMenuPlayerController.c` already implements the same pattern (Rpc `CLW_RpcAsk_HqMenuOpen/Action/Data`, owner reply) and `CLW_HqMenuDialog` (SCR_ConfigurableDialogUi).
- Engine docs: wiki "Menu System" (MenuManager, MenuPreset = layout + script class + action context) and "Layout Creation" (Display vs Menu vs Dialog) via enfusion-mcp `wiki_read`.

## 4. Recommendation

Build our own depot on top of the existing HQ menu pattern; copy the pattern, do not depend on a mod.

1. One single `ScriptedUserAction` on the depot prefab (ours: e.g. `CLW_DepotOpenAction`, `HasLocalEffectOnlyScript` true) - replaces the 200 actions; its context is trivial to make visible (cf. vanilla build action). Requires on the prefab: RplComponent, mesh + collision, one UserActionContext with Position PointInfo, parent context list set, visibility range.
2. Open the list in `CLW_HqMenuDialog` (or a sibling dialog) with a ListBox/scroll list, faction + category filter, supply cost label; paging/search needed for ~170 rows. Build rows from our own merged data (`.conf` + runtime catalog discovery; no vanilla hard-wiring).
3. Request path: `CLW_HqMenuPlayerController` Rpc (RplChannel.Reliable, RplRcver.Server) with prefab index/ResourceName hash + depot RplId; server re-validates (user faction, distance to depot, supplies via `SCR_ResourceComponent`/`SCR_ResourceConsumer` SUPPLIES or our own pool, cooldown) and spawns.
4. Spawn: either reuse vanilla slots (`SCR_EntitySpawnerSlotComponent.IsOccupied()`, `SCR_CatalogEntitySpawnerComponent.GetFreeSlot`-like logic) or `GetGame().SpawnEntityPrefab(Resource.Load(prefab), world, EntitySpawnParams)` at our own PointInfo with a clear-area check like `ADM_Utils.IsSpawnPositionClean` (copy idea, not code) and `entity.GetPhysics().SetActive(true)`.
5. If vanilla preview/supply UX is wanted for a short subset, keep vanilla `SCR_CatalogSpawnerUserAction` ONLY for <= ~20 entries and put the extra vehicles into the faction's vehicle `SCR_EntityCatalog` (with `SCR_EntityCatalogSpawnerData`, labels matching the spawner); this does not scale to 170.

### Concrete API names
- Actions: `ScriptedUserAction` (`PerformAction`, `CanBeShownScript`, `CanBePerformedScript`, `HasLocalEffectOnlyScript`, `CanBroadcastScript`, `GetActionNameScript`), `ActionsManagerComponent`, `UserActionContext`, `BaseActionsManagerComponent.GetContextList`, `BaseUserAction.GetActionID`.
- Vanilla spawner: `SCR_CatalogEntitySpawnerComponent` (`GetRequestState`, `GetFreeSlot`, `GetEntryAtIndex`, `InitiateSpawn`, `GetSpawnerResourceValue`, `IsSuppliesConsumptionEnabled`, `GetFaction`), `SCR_CatalogSpawnerUserAction`, `SCR_EntitySpawnerSlotComponent`, `SCR_EntityCatalogEntry`, `SCR_EntityCatalogSpawnerData` (`GetSupplyCost`, `GetOverwriteName`, `HasRequiredRank`, `CanSpawnInSlot`), `SCR_EEntityRequestStatus`, `SCR_SpawnerRequestComponent.RequestCatalogEntitySpawn`, `SCR_Faction.GetFactionEntityCatalogOfType(EEntityCatalogType.VEHICLE)`, `SCR_EntityCatalog.GetFullFilteredEntityList`.
- UI: `MenuBase`, `GetGame().GetMenuManager().OpenMenu(preset)`, `ChimeraMenuPreset` (extend via enum), `SCR_ConfigurableDialogUi`, `SCR_ListBoxComponent`/ListBox widgets.
- Replication: `[RplRpc(RplChannel.Reliable, RplRcver.Server)]`, `RplComponent.Id()`, `Replication.FindItem`, `Replication.FindItemId`.
- Resources: `SCR_ResourceComponent`, `SCR_ResourceConsumer.GetAggregatedResourceValue`, `EResourceType.SUPPLIES`.

## Unverified / gaps
- No source found stating a numeric limit on actions per context; the failure is most likely caused by points 1-3 above (faction / catalog / slot gating), not a count. To confirm, check the DS log for the vanilla warning "There is not enough of SCR_CatalogSpawnerUserAction attached to actionManager" and test the same prefab with 3 actions.
- Reddit/Bohemia forum threads: web search backend timed out repeatedly, so no forum evidence was collected.
- TLL_Traders and WKI_PlaygroundFramework licenses/internals not inspected.
- GitHub code search returned nothing within the time box.
