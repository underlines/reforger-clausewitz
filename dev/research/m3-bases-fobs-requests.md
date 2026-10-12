# M3 research: packs (GRS/WCS/RHS) vs arsenals, vanilla bases/services/requests, scriptable GM functions

Date 2026-10-12. Sources: unpacked game paks (E:\Games\steam\...\Arma Reforger, `scripts/Game/...`, `Prefabs/...`) read with a PakVirtualFS dump,
unpacked Workshop paks under `Documents\My Games\ArmaReforger\addons`, Workshop page of GRS Apparel. Labels: [SRC] read in source/prefab,
[WS] Workshop page, [verify] unconfirmed (needs a test or a deeper read). Earlier notes not repeated:
`loadout-editors-arsenals.md`, `mod-agnostic-asset-discovery.md`, `modpacks-factions-testing.md`, `vehicles-and-spawning.md`, `commanding-and-tasks.md`.

## 1. GRS, and do WCS/RHS bypass the catalog manager?

### 1.1 What "GRS" is [SRC][WS]
- No faction pack. GRS = a family of **gear/apparel mods** by "The GRS Mod Team" (contributors Wrottz, Nemesis0998): Crye G3 uniforms, vests, plate
  carriers, modular belts/backpanels/droplegs, bags, boots, headsets, patches, watches. The name is not expanded on the Workshop page.
  Installed here (all in `Documents\My Games\ArmaReforger\addons`, GUID = Workshop id), sizes on disk:

  | Mod | GUID | Size | Needs |
  |---|---|---|---|
  | GRS - Dev Framework | 65DACC64CE785B6C | 0.2 MB | base |
  | GRS - Patches | 657B064AE0E231DF | 123 MB | base [verify exact deps] |
  | GRS - Apparel | 65157D09F042428A | 536 MB | Dev Framework, Patches |
  | GRS - Essentials | 6515672F96701110 | 750 MB | Dev Framework |
  | GRS - Modular Vests & Rigs | 651834C8D77BF86B | 364 MB | Dev Framework, Patches [verify] |
  | GRS - Belts & Bags & Droplegs | 65183608D6DA5728 | 425 MB | Dev Framework [verify] |
  | GRS - Footwear | 66CEA22046CAA731 | 106 MB | catalog for US+USSR |
  | GRS - Watches | 6A4C66E4F04065A3 | 45 MB | catalog for US |
  | **GRS - USSR** | 667018A2DF8C1AA4 | 203 MB | Patches, Vests, DevFramework, Belts, Essentials, Apparel |
  | GRS - Bundle | 6518EA4DDEA42E15 | 0.4 MB | meta-mod: Belts, Essentials, DevFramework, Vests, Apparel, Patches, Watches |

  Whole set about 2.5 GB. Minimum for "GRS vs vanilla": `GRS - Bundle` (pulls 7 mods, ~2.2 GB, adds US items) plus `GRS - USSR` (adds the USSR set).
- **How it gets into arsenals [SRC]:** it overrides the vanilla faction item catalog files
  `Configs/EntityCatalog/US/InventoryItems_EntityCatalog_US.conf` (Dev Framework: Bottoms, Eyewear, Footwear, Handwear, HeadGear, Neckwear, Tops,
  Backpacks, Backpanels, Belts, ...) and `.../USSR/InventoryItems_EntityCatalog_USSR.conf` (GRS - USSR: ~30 `SCR_EntityCatalogMultiListEntry`, each
  entry has a `SCR_ArsenalItem` with `m_eItemType`). So GRS items live in the **vanilla US / USSR ITEM catalogs** and show in vanilla US/USSR
  arsenal boxes. No own factions, no groups/vehicles, no loadout manager, no arsenal prefab.
- Scripts [SRC]: `Apparel_LoadoutTypes.c`, `GRS_LoadoutSlotInfo.c`, `GRS_Patch_Loadout.c`, `GRS_WatchLoadoutSlotInfo.c`, watch components
  (extra loadout/inventory slots for patches, watches, bag attachments). Dev Framework also **overrides vanilla prefabs** (`Character_Base.et`,
  `Jacket_US_BDU_base.et`, grenades, 12.7/14.5/7.62/5.56/5.45 magazines/boxes). => every character in a test session has GRS slots; compare AI
  behaviour/perf with and without. A save/load of arsenal loadouts must round-trip the extra slots (Bacon/WCS editors were not written against them [verify]).
- Consequence for Clausewitz: nothing to discover for forces (no groups/vehicles). GRS only matters as **arsenal/loadout content**. Because the
  catalogs are overridden in place, the GRS items appear for a side only if that side's faction is `US` / `USSR` (or kin through our L2 pool);
  they do NOT appear for RHS_USAF / RHS_AFRF unless the pool adds the US/USSR catalog as kin. Good natural test of the kin pool.
- Conflict between mods [verify]: GRS-USSR and Footwear both override the same USSR catalog path; the later one in the dependency order wins.
  GRS-DevFramework's USSR file lists only 2 sub-lists (Backpacks, Radios) while GRS-USSR lists ~30, so with both loaded only the winner's lists exist.
  Check the item count per faction in the log (`sides catalog` line) when loading GRS - USSR + GRS - Footwear together.

### 1.2 RHS and WCS: any arsenal outside `SCR_EntityCatalogManagerComponent`? [SRC]
- **RHS Status Quo (`595F2BF2F44836FB`)**: factions `RHS_US_USMC` (key `RHS_USAF`), `RHS_RF_MSV` (`RHS_AFRF`), `RHS_ION`, plus `_Campaign` variants,
  `RHS_CIV`, `FreeForAll`; faction ITEM catalogs `Configs/EntityCatalog/{USMC,RHS_MSV,ION,FFA}/*_InventoryItems.conf`; loadouts `Configs/Loadouts/{USAF/USMC/MEF,AFRF/MSV/VKPO_S}/*`
  (8 roles each: AR, GL, LAT/AT, Medic, MG, Rifleman, Sniper/Sharpshooter, Base). Arsenal boxes `Prefabs/Systems/Arsenal/ArsenalBoxes/{US/ArsenalBox_USMC,USSR/ArsenalBox_RHS,ION/ArsenalBox_RHS_ION}.et`
  inherit vanilla `ArsenalBox_US/_Base` (`faction affiliation "RHS_USAF"`, `m_eSupportedArsenalItemModes 246`, **resource consumer `m_fBuyMultiplier 0`
  and generator multiplier 0 = items cost nothing**). They use the faction ITEM catalog (no overwrite).
  Only ONE `m_OverwriteArsenalConfig` in the pack: `PrefabsEditable/Auto/Systems/Arsenal/.../E_...` vehicle ammo crate with one 122 mm 2A18 round
  (`m_iSupplyCost 60`, rank CORPORAL) - irrelevant for infantry arsenals.
  RHS mods `SCR_ArsenalManagerComponent` (saves RHS storage components: `RHS_ClothNodeStorageComponent`, `RHS_EquipmentStorageComponent`,
  `RHS_HelmetNodeStorageComponent`) and `SCR_PlayerArsenalLoadout.OnLoadoutSpawned` (stores weapon slot/attachment/slide data under key
  `RHS_ArsenalLoadoutData`). Not a bypass; still the vanilla path with extra save data. => the RHS kit round-trips only through vanilla/RHS save code.
  RHS ContentPack01/02 are multi-pak (`data.pak` + `data001-003.pak`); readpak only sees `data.pak`, so their files were not listed (6.0 GB + 2.1 GB).
  [verify] ContentPack01/02 `ammo/weapon` catalog files go to which factions (expected: RHS factions' ITEM catalogs).
- **WCS_Arsenal (`615CC2D870A39838`, 4.2 MB)**: data only (plus a wristwatch component). Overrides `Configs/EntityCatalog/{US,USSR,CIV}` per category
  (Ammunition/Attachments/Clothing/Deployable/Equipment/Explosives/Other/Throwables/Vehicles/Weapons, files suffixed `_Reforger/_RHS/_WCS/_Custom`).
  => adds WCS **and RHS** weapons (the `_RHS` lists) to vanilla US/USSR/CIV catalogs. Entries for RHS prefabs exist with or without RHS loaded
  [verify: missing prefab = silently skipped or ERROR].
- **WCS_LoadoutEditor, Bacon**: vanilla arsenal hook (already in `loadout-editors-arsenals.md`). **WCS_Core** changes Conflict only (modded campaign building
  manager/provider, supply allocation, XP, `US_Campaign`/`USSR_Campaign` faction confs, `WCS_Core_CampaignSupplyRestrictedSpawnPoint`): no effect without Conflict.
- Verdict: none of RHS/WCS/GRS bypasses `SCR_EntityCatalogManagerComponent` for infantry arsenals. Hook point in `notes/loadouts-arsenal-plan.md` L2 stands.
  Special cases: (a) arsenal prefabs with an `m_OverwriteArsenalConfig` (RHS vehicle ammo crate; vanilla `HeliWeapons_ArsenalBox_*`, ammo boxes) are untouched
  by the pool, correct; (b) RHS factions' item catalogs are separate from US/USSR's, so RHS_USAF does NOT see WCS_Arsenal/GRS items (they sit in the US catalog)
  unless the kin pool adds `US`.

## 2. What decides arsenal / loadout-editor content, and a one-session test plan

### 2.1 Decision chain [SRC]
1. **Faction of the box**: `SCR_ArsenalComponent.GetAssignedFaction()` = `SCR_FactionAffiliationComponent` current faction; if none and
   `m_bGetDefaultIfNoFaction` (default 1) the component's default faction; `m_bAlwaysUseDefaultFaction` ignores runtime changes. Changing the faction at
   runtime re-runs `RefreshArsenal` (clients compute the list too).
2. **Catalog**: `SCR_EntityCatalogManagerComponent.GetFilteredArsenalItems(types, modes, gameModeType, faction)` -> `GetFactionArsenalItems` = the faction's
   `EEntityCatalogType.ITEM` catalog, entries that carry `SCR_ArsenalItem` data. With `faction == null` it reads the **manager's own generic ITEM catalog**
   (not merged into faction lists automatically: answers the open "layer 2" question in the plan; those items show only in arsenals without a faction).
3. **Filters**: `m_eSupportedArsenalItemTypes` (flags, `SCR_EArsenalItemType`, ArsenalBox_Base = 2097150) and `m_eSupportedArsenalItemModes`
   (`DEFAULT=2, WEAPON=4, WEAPON_VARIANTS=8, AMMUNITION=16, CONSUMABLE=32, ATTACHMENT=64, SUPPORT_STATION=128, PYLON=256`; ArsenalBox_Base = 254).
   **Game mode type**: `SCR_ArsenalManagerComponent.m_eArsenalGameModeType` (RplProp). `GameMode_Base.et` leaves it at -1 = unrestricted; `GameMode_Campaign.et`
   sets `CAMPAIGN` (+ `m_bRankLockedItems 1`, `m_fLoadoutSpawnSupplyCostMultiplier 1`, `m_fRefundDifferentFactionItemMultiplier 0.4`). Items carry
   `m_eArsenalGameModeTypes` (default all flags). CLW_GameMode inherits Base => unrestricted, rank lock off.
4. **Overwrite**: `m_OverwriteArsenalConfig` (`SCR_ArsenalItemListConfig`, standalone items) replaces the catalog; `m_bCheckFactionForOverwriteArsenalConfig` (0)
   optionally hides items not of the box faction.
5. **Supplies**: an arsenal "uses supplies" iff its `SCR_ResourceComponent` has the SUPPLIES type enabled (`IsArsenalUsingSupplies`; global switch
   `BaseGameMode.m_aDisabledResourceTypes`, default empty = enabled). Cost = `SCR_ArsenalItem.GetSupplyCost(box.GetSupplyCostType())` taken from the box's
   consumer (resource range 185 m on ArsenalBox_Base, i.e. containers within range). Free arsenal = box consumer `m_fBuyMultiplier 0` (the RHS trick) or
   SUPPLIES disabled in the game mode. **Risk for CLW (our mode never defines supplies containers):** a plain `ArsenalBox_US` may show items greyed with
   "not enough supplies" [verify]; plan for `CLW_Arsenal.et` with buy multiplier 0 (or a supply container fed from `CLW_WarState`).
6. **Save/restore of kits** (`CanSaveArsenal`): `m_eArsenalSaveType` of the box: `FACTION_ITEMS_ONLY` (ArsenalBox_Base default; items must be in the
   player's faction ITEM catalog), `FRIENDLY_AND_FACTION_ITEMS_ONLY` (also catalogs of `playerFaction.GetFriendlyFactions()`; whether the own faction is in that
   list is [verify], without it the own items could be refused), `IN_ARSENAL_ITEMS_ONLY`, `NO_RESTRICTIONS`. A kin pool at manager level (plan L2) makes
   save checks via `GetFactionArsenalItems` consistent, but `CanSaveArsenal` reads `GetFactionEntityCatalogOfType` directly: **patch or use NO_RESTRICTIONS**.
7. **Loadout editor / deploy menu**: a faction needs `SCR_PlayerArsenalLoadout` entries in the `SCR_LoadoutManager` (WCS: its own entries for US/USSR/FIA only;
   RHS has role loadouts but no WCS slot entries); extra mod slots (GRS patches/watches) are saved only if the editor serialises them [verify].

### 2.2 E2E checklist (one Workbench session; extends `notes/loadouts-arsenal-plan.md` L4 and `testpacks/README.md`)
Setup: a test project that depends on Clausewitz + RHS (3) + WCS_Arsenal/Core/LoadoutEditor + GRS Bundle + GRS-USSR (new `ClausewitzEditors`, junctions as
in the README). Hotfix: put one `CLW_Arsenal` box per base (L1) and log per box `faction`, `items=N`, `supplies=on/off`.
1. Boot, vanilla factions US vs USSR. Log lines: items per side. Expect GRS apparel on both, WCS/RHS weapons (WCS_Arsenal) on both; **no** USSR-only
   item (`AK*`, `PKM`) in the US box and vice versa (spot check 3 prefabs each way).
2. Open the box: items visible? If greyed/"supplies": confirm point 5 above, then retest with buy multiplier 0.
3. Pick `RHS_USAF` vs `RHS_AFRF`: box re-keyed at war start (`bases re-keyed`); RHS items show; GRS/WCS_Arsenal items (US catalog) do NOT show unless pool
   mode FRIENDLY adds `US`/`USSR` (kin). Then with the kin pool: they show on the right side only; the enemy side never gets them.
4. Mixed: `RHS_USAF` vs `USSR` (cross-pack): USSR box without RHS_AFRF items; USAF box without USSR items.
5. Client view: join as second client (or listen server client) and compare item count in the arsenal UI with the server log (clients compute their own lists).
6. Loadout editors: vanilla "save loadout" with an item of the own catalog (ok), with a kin item (FRIENDLY save type), with an enemy item (refused);
   Bacon and WCS editor actions appear on the box; slots exist for RHS factions (WCS: expected none).
7. Respawn with the saved kit from the deploy menu; check GRS patch/watch slots and RHS storage nodes survive (point 7).
8. Role loadouts: US/USSR 6 roles (done), RHS 8 roles from RHS' own loadout confs appear in the deploy menu (`sides faction RHS_USAF has N player loadout(s)`).
9. Logs clean: no `ERROR` for missing RHS prefabs (WCS_Arsenal `_RHS` lists), no "has no ITEM catalog" for RHS_ION/FFA.
10. Faction swap at runtime: GM changes box faction (`SCR_ArsenalFactionEditorAttribute`) -> list refreshes on server and client.

## 3. Vanilla Conflict building / FOBs / services / requests

### 3.1 Class map [SRC]
| Piece | Classes / prefabs | Conflict-only? |
|---|---|---|
| Base object | `SCR_MilitaryBaseComponent` (generic; faction, radius, registered by the global `SCR_MilitaryBaseSystem`, listed in `Configs/Systems/ChimeraSystemsConfig.conf`) ; prefab `Prefabs/Systems/MilitaryBase/TestMilitaryBase.et`, `Compositions/BaseLogic/MilitaryBaseLogic_base.et` | Generic. `SCR_CampaignMilitaryBaseComponent` (+`SCR_CampaignSeizingComponent`, `SCR_CampaignSuppliesComponent`, `SCR_CoverageRadioComponent`) is Conflict |
| Service point (base) | `SCR_ServicePointComponent : SCR_MilitaryBaseLogicComponent`; `m_eType` (`SCR_EServicePointType`: ARMORY, LIGHT/HEAVY_VEHICLE_DEPOT, FIELD_HOSPITAL, BARRACKS, RADIO_ANTENNA, HELIPAD, FUEL_DEPOT, REPAIR_DEPOT, SUPPLY_DEPOT), `m_eBuildingLabel`, `m_sDelegatePrefab` (`Prefabs/Systems/Services/ServicePointDelegate_*.et`, map/UI proxy), RplProp state OFFLINE/ONLINE/UNDER_CONSTRUCTION/BROKEN/DELETED, `SpawnAsOffline(bool)` static, `RegisterService()` registers at the base system | Generic (registers at any `SCR_MilitaryBaseComponent`; only 1 base auto-adopted outside Campaign: `OnBaseRegistered` takes `m_aBases.Count()==1`) |
| Vehicle depot / barracks / "request table" | **`SCR_CatalogEntitySpawnerComponent : SCR_SlotServiceComponent : SCR_ServicePointComponent`** + `SCR_EntitySpawnerSlotComponent` slots + `SCR_CatalogSpawnerUserAction`s + `SCR_SpawnerRequestComponent` on the player + `SCR_SpawnerAIGroupManagerComponent` on the game mode (already in `GameMode_Base.et`). Prefabs: `Props/Military/Compositions/{US,USSR,FIA}/VehicleService_*` (e.g. `VehicleService_US` {5753634EAFFBA2F6}, Medium/Large/Small, `labels VEHICLE_TRUCK/APC/CAR...`), `PersonnelService_*` (infantry/groups), `MP/Campaign/RequestTable_{US,USSR}[_Barracks].et`, `Sign_VehicleRepair_*_Campaign_*Depot.et`, slot prefabs `Prefabs/Systems/Compositions/EntitySpawnerSlot/EntitySpawnerSlot_Vehicle_{base,Small,Medium,Large}.et`, `Systems/Logic/{EntitySpawnerSlot,CharacterSpawnerSlot}.et` | Mostly generic; rank + cooldown checks only when `SCR_GameModeCampaign.GetInstance()` exists; supply-radius search for `SCR_CampaignSuppliesComponent` only in Campaign (else `SCR_ResourceComponent` / `m_iCustomSupplies`) |
| Armory | `ArsenalBox_Base` is itself an `SCR_ServicePointComponent` (type ARMORY, delegate `ServicePointDelegate_Armory`); `SCR_ArmoryComponent` (ChangeOwningFaction for all affiliation comps in a composition); `SCR_CampaignArmoryStorageComponent` | box generic; armory storage Conflict |
| Field hospital / repair / fuel / ammo | **Support stations** (`SCR_SupportStationComponent` family: `SCR_HealSupportStationComponent`, `SCR_RepairSupportStationComponent`, resupply, fuel; prefabs `Prefabs/Systems/SupportStations/Static_HealSupportStation.et`, `PrefabsEditable/SupportStationSystems/E_SupportStationSystem_{Heal,Repair,Repair_Medium,Fuel}.et`, tents `Structures/Military/Camps/Tent{US,USSR,FIA}*Medical*`) with user actions `SCR_HealSupportStationAction`, `SCR_RepairAtSupportStationAction`, `SCR_ResupplySupportStationAction` ... | Generic (drain `SCR_ResourceComponent` supplies; not Campaign) |
| Radio relay / antenna | `SCR_AntennaServicePointComponent`, `SCR_CoverageRadioComponent`, `SCR_RadioRelayBuildingDisassemblyUserAction`; composition `Antenna_S_{US,USSR,FIA}_01.et`; HQ radio `MP/Campaign/Assets/CampaignHQRadio*`, `CampaignRadioTruck*`, `CampaignRadioBox` | Coverage graph is `SCR_GameModeCampaign.GetBaseManager()` (Conflict) |
| HQ / command post | Composition `Slotted/SlotFlatSmall/Headquarters_S_Conflict_{US,USSR}_01.et` (BuildableComposition_Base + `MilitaryBaseLogic_US.et` + `CampaignHQRadioBase` + tent `Tent_CommandPost_Conflict_*`); `MP/Campaign/Bases/Campaign_HQ_US.et` (message board + radio box + `Sign_CommandPost_US_01` with `SCR_CampaignBuildingStartUserAction`); `SCR_CampaignBuildingPlacingHQBaseEditorComponent` | Conflict (HQ registration in `SCR_CampaignBuildingManagerComponent.TryAddHQ`) |
| Supply depot | `SupplyCache_S_{US,USSR}_01.et` (service SUPPLY_DEPOT), `MP/Campaign/SupplyDepots/*` , `Vehicle_EngineerBox_Base.et` (a building provider in a truck, `SCR_ResourceContainer` max 550) | generic parts + Conflict wrappers |
| Mobile FOB | `SCR_CampaignMobileAssemblyComponent` (+`...StandaloneComponent`) on `MP/Campaign/Assets/CampaignMobileAssembly{West,East}.et`; `SCR_CampaignDeployMobileAssemblyUserAction`; spawns `CampaignMobileAssemblySpawnpoint.et` (an `SCR_SpawnPoint`) | uses `SCR_GameModeCampaign.GetInstance()` at 6 places (radio coverage, supplies); copy the idea, not the class |
| Free-roam building | `SCR_CampaignBuildingManagerComponent` (game mode component; configs: `m_sPrefabsToBuildResource` = `SCR_PlaceableEntitiesRegistry`, outline manager, trigger prefabs), `SCR_CampaignBuildingProviderComponent : SCR_MilitaryBaseLogicComponent` (on depots/service props; budgets, cooldown by rank, `m_bRegisterAtBase`, `m_aAvailableTraits`), `SCR_CampaignBuildingNetworkComponent` (already on `DefaultPlayerControllerMP.et`), editor mode `Prefabs/Editor/Modes/EditorModeBuilding.et` (`SCR_CampaignBuildingEditorComponent`, `...PlacingEditorComponent`), user actions `SCR_CampaignBuildingStartUserAction` / `...BuildUserAction` / `...DisassemblyUserAction`, composition `SCR_CampaignBuildingCompositionComponent`, layout `SCR_CampaignBuildingLayoutComponent` | Manager is on `GameMode_Campaign(HQC).et` and `GameMode_Editor_Full.et`; refund/supply budget code is guarded by `SCR_GameModeCampaign.GetInstance()` |

### 3.2 How vehicles are requested at a depot [SRC]
- Depot prop = `VehicleService_*` composition: `SCR_CampaignBuildingProviderComponent` (build service) + `SCR_CatalogEntitySpawnerComponent` + `SCR_ResourceComponent`
  (`SCR_ResourceConsumerServicePoint`/`...GeneratorServicePoint`) + slots (`EntitySpawnerSlot_Vehicle_*`, size flags `SCR_EEntitySpawnerSlotType`).
- Content: `SetCurrentFactionCatalog()` reads for the spawner's faction (`GetFaction()` = its affiliation) the catalogs listed in `m_aCatalogTypes`
  (`EEntityCatalogType.VEHICLE`, `CHARACTER`, `GROUP`), keeps entries that have **`SCR_EntityCatalogSpawnerData`** (slot size, supply cost `GetSupplyCost()`, entity count,
  rank, variants) and match `m_aAllowedLabels` / not `m_aIgnoredLabels` (e.g. `VEHICLE_TRUCK`, `VEHICLE_APC`, `VEHICLE_CAR`). Then `AssignUserActions()`
  copies entry i into the i-th `SCR_CatalogSpawnerUserAction` on the prop (so the prefab needs enough actions, "Some assets won't appear" warning otherwise).
  => **content is pure faction-catalog data; RHS/WCS vehicles appear for free** if their catalog entries carry `SCR_EntityCatalogSpawnerData`
  (the same catalog we already discover in `CLW_ForceCatalog`) [verify RHS entries have it; WCS_Arsenal `US/Vehicles` has 31 lists].
- Request: client action -> `SCR_SpawnerRequestComponent.RequestCatalogEntitySpawn(index, spawner, user, slot)` -> server RPC `RPC_DoRequestCatalogSpawn` ->
  `SCR_CatalogEntitySpawnerComponent.InitiateSpawn(entry, userId, slot)` (needs `GetPlayerControlledEntity(userId)`) -> `GetRequestState()` (NOT_AVAILABLE if the
  requester faction != spawner faction; supplies; rank/cooldown only in Campaign; for CHARACTER/GROUP: AI limit via `SCR_SpawnerAIGroupManagerComponent.IsAtAILimit`,
  requester must be group leader, `CanRequestAI`) -> `PerformSpawn`: finds a free slot (`GetFreeSlot`, `slot.IsOccupied`), spawns the prefab at the slot
  (`SpawnEntityPrefab`), deducts `AddSpawnerSupplies(-cost)` (consumer of the resource component, or `m_iCustomSupplies`), vehicles: hand brake, `SCR_VehicleSpawnProtectionComponent`
  lock for `m_iVehicleLockedDuration` s (owner = requester), refund action `SCR_ResourceEntityRefundAction` (grace period `m_fGracePeriodTime` 60 s, 250 m, refund
  multiplier 0.25 afterwards); groups: members spawned one by one by `SCR_SpawnerAIGroupManagerComponent` (delay 1.5 s) and added to the requester's group
  (`AddAISoldierToPlayerGroup`) with a move waypoint to the slot's rally point.
- Supplies flow: depot -> base container(s): `OnBaseRegistered` links the service consumer to the base consumer (`SCR_ResourceConsumerMilitaryBase`) so the
  depot spends what the base holds. Without a base it uses its own `SCR_ResourceComponent` or `m_iCustomSupplies` (scenario-set number; `AddSpawnerSupplies()` public).
  Fits our `CLW_WarState` ledger: set `m_bSuppliesConsumptionEnabled 0` on the spawner (free for the player) and charge `TrySpend` ourselves, or keep `m_iCustomSupplies` in sync.
- Public/callable on the server without UI: `GetRequestState(entry, user)`, `InitiateSpawn(entry, userId, slot)`, `AddSpawnerSupplies(float)`, `GetEntryAtIndex`,
  `GetFreeSlot(spawnerData)`, `INSTANCES` (static list), `m_OnEntitySpawned` invoker (`GetOnEntitySpawned` [verify name]). `PerformSpawn`/`SpawnEntity` are `protected` (modded class OK).
  `InitiateSpawn` requires a human `userId` with a controlled entity, so AI-side requests should keep spawning through `CLW_ForceSpawner` and call only `AddSpawnerSupplies`.

### 3.3 Supply trucks, loading/unloading [SRC]
- Supplies = `SCR_ResourceContainer` (`EResourceType.SUPPLIES`, storage type CARGO_VEHICLE on trucks: cargo parts `M923A1_cargo.et`, `Ural4320_cargo.et`; already
  in `vehicles-and-spawning.md` section 5). A vehicle/base `SCR_ResourceComponent` has generators/consumers per `EResourceGeneratorID` (`DEFAULT`, `VEHICLE_LOAD`,
  `VEHICLE_UNLOAD`).
- Player load/unload: `SCR_ResourceContainerVehicleLoadAction` / `...UnloadAction` (`m_fTransferAmount` default 2000, -1 = max) server-side do
  `consumer.RequestConsumtion(x)` on the source and `generator.RequestGeneration(x)` on the truck (reverse for unload). Plain API calls: **a server script can move
  supplies between two `SCR_ResourceComponent`s the same way, no player needed** (no Conflict dependency; spelling "RequestConsumtion" is the real name).
- AI haulers: waypoints `SCR_LoadSuppliesWaypoint`, `SCR_UnloadSuppliesWaypoint` (base `SCR_SuppliesTransferWaypoint`: `m_fTransferAmount`, `m_fTransferDelay`), prefabs
  `AIWaypoint_LoadSupplies/UnloadSupplies`; **hard-typed to `SCR_CampaignMilitaryBaseComponent`** (`SetBase(...)`, auto-discover) and `SCR_SuppliesTransportSystem`
  (GameSystem) / `SCR_TransportUnitComponent` + resupply tasks (`SCR_ResupplyCampaignMilitaryBaseTaskEntity`). => unusable outside Conflict as is. For Clausewitz:
  logistics ledger in `CLW_WarState` + a real truck group that drives A->B (cosmetic, risk of ambush) and on arrival transfers with the two API calls above.
- Conflict also has `SCR_CampaignSuppliesComponent` (`GetSupplies/AddSupplies/GetSuppliesMax`), partly obsolete in favour of the resource system.

### 3.4 Building a composition [SRC]
- Compositions are **ordinary prefabs**: `BuildableComposition_Base.et` {6E9190BC2713551D} + `SCR_CampaignBuildingCompositionComponent` + `ActionsManager` with
  `SCR_CampaignBuildingDisassemblyUserAction` (or radio-relay variant), children (props, tents, `MilitaryBaseLogic_*`, service props) as hierarchy.
  E.g. `Compositions/Slotted/SlotFlatMedium/FieldHospital_M_Conflict_US_01.et`, `VehicleMaintenance_M_Conflict_US_01.et`, `SlotFlatSmall/Headquarters_S_Conflict_US_01.et`,
  `Antenna_S_US_01.et`, `SlotFlatLarge/{Helipad_L,LivingArea_L}_Conflict_*`, `SlotFlatSmall/{AmmoStorage_S,SupplyCache_S}_*`; fortifications `Compositions/Misc/FreeRoamBuilding/*`
  and layouts `.../Layouts/FRB_*.et` (outline/preview prefab: `SCR_CampaignBuildingLayoutComponent`).
- **Instant**: `GetGame().SpawnEntityPrefab(Resource.Load(compositionPrefab), world, params)` on the server spawns the whole finished composition with all children
  (they are part of the prefab); the service props register themselves in `OnPostInit` (`ServicePointComponent.RegisterService`, state ONLINE). This is the "fake it" path
  (VISION 6 `IBuildExecutor`); additionally set faction on every `SCR_FactionAffiliationComponent` (`SCR_ArmoryComponent.ChangeOwningFaction` does this for a composition;
  the prefab variants are per faction key `_US_/_USSR_/_FIA_`, so pick/re-key by data).
- **With progress**: real flow = `SCR_CampaignBuildingStartUserAction` -> `provider.RequestEnterBuildingMode` -> editor mode (`EditorModeBuilding`) places a layout
  (`SCR_CampaignBuildingCompositionComponent.SpawnCompositionLayout`; service state UNDER_CONSTRUCTION) -> each build step `SCR_CampaignBuildingLayoutComponent.AddBuildingValue(v)`
  (`SetBuildingValue`) until `m_iToBuildValue` (= `GetBuildingValue(prefabId)` from the provider budget) -> `SpawnComposition()` (via `SCR_EditorLinkComponent.SpawnComposition()`)
  replaces the outline by the real composition. A script that owns the layout entity can drive it: `layout.AddBuildingValue(n)` every N s = timed build with progress
  (server; layout is replicated; `GetOnAddBuildingValueInt()` invoker). Simpler and robust: show our own progress (marker/chat/%), then spawn the finished prefab.
- **Costs**: provider budgets (`SCR_CampaignBuildingCampaignBudgetToEvaluateData` = supplies, cooldown by rank, prop budgets); refund logic in the manager runs only if `SCR_GameModeCampaign` exists.
  Outside Conflict ignore them and charge `CLW_WarState`.
- Free-roam building by human players outside Conflict: add `SCR_CampaignBuildingManagerComponent` to `CLW_GameMode.et` (+ placeable-prefab registry, outline manager, trigger prefabs),
  providers on depot props, and the editor building mode to the editor manager [verify: `GameMode_Editor_Full.et` shows the manager works without Campaign; the supply/HQ branches are skipped]. Not needed for M3 v1.
- Can **AI build**? No vanilla AI behaviour/waypoint for construction or engineering was found (`scripts/Game/AI/**` has only supplies, heal, repair-request tasks, boarding). "AI builds" = our own fake: group moves to the
  site (waypoint), on arrival we spawn the composition (or advance `AddBuildingValue` while engineers stand within radius).
- `SCR_DefenderSpawnerComponent` (`SCR_SlotServiceComponent`): auto-spawns AI defenders at a base from a catalog, respawn period 300 s, supplies-priced, paused when hostile players within 30 m [SRC]; a ready-made "FOB garrison" if we spawn it ourselves
  [verify it works without Campaign; needs rally point labels].

### 3.5 What is reusable outside Conflict (CLW_GameMode derives GameMode_Base)
Reusable as is (server-side data/prefabs; already in the base chain or addable): `SCR_MilitaryBaseComponent` + `SCR_MilitaryBaseSystem`; `SCR_ServicePointComponent` and all service
props; **`SCR_CatalogEntitySpawnerComponent` depot/request tables + slots + `SCR_SpawnerAIGroupManagerComponent` (in GameMode_Base.et) + `SCR_SpawnerRequestComponent`**;
support stations (heal/repair/fuel/ammo); `ArsenalBox_*` boxes; resource containers/transfer API; all composition prefabs (as static spawns); `SCR_EntitySpawnerSlotComponent`.
Needs a Campaign instance (skip or re-implement): campaign base/seizing/supplies/radio coverage, mobile assembly, supply waypoints and transport system, HQ registration, rank/XP gates,
building refunds. Check list when placing the prefabs: faction key on every affiliation comp (our sides are re-keyed to RHS_* etc.: use `SCR_ArmoryComponent`-style re-key helper),
`RplComponent` present, `m_iCustomSupplies` or buy multiplier set.
[verify] a `SCR_CatalogEntitySpawnerComponent` prop placed by script in `CLW_Arland.ent`: spawner actions populated, request works on a listen server, group request needs a player in a group.

## 4. Scriptable Game Master functions (answers VISION 5a open question)
Editor actions (`SCR_BaseEditorAction`, `SCR_BaseEditorAttribute.WriteVariable`) need an editor manager/player context, but they are thin wrappers over component APIs
that a server script can call directly [SRC]:

| Intent | Server-callable API | Notes |
|---|---|---|
| Spawn any prefab (editable) | `SCR_PlacingEditorComponent.SpawnEntityResource(ResourceName, vector transform[4])` / `(SCR_EditorPreviewParams, Resource, playerID, isQueue, recipient, canBePlayer)` static; returns `SCR_EditableEntityComponent`; server only (`Replication.IsClient()` -> null) | handles slots (`SCR_SiteSlotEntity`), can spawn as player (`canBePlayer`, `SCR_PossessSpawnData`). Plain `SpawnEntityPrefab` is what we use now |
| Delete / destroy | `SCR_EditableEntityComponent.Delete(changedByUser=false, updateNavmesh=false)`, `Destroy(editorPlayerID)` (group: kills all) | `SCR_EntityHelper.DeleteEntityAndChildren` for non-editable |
| Move | `SCR_EditableEntityComponent.SetTransform(vector[4])` | waypoint override clamps/relinks |
| Reparent / put in group | `SetParentEntity(SCR_EditableEntityComponent)` | vanilla group join uses this + `SCR_AIGroup.AddAgent` |
| Waypoints | spawn a waypoint prefab as editable with `recipient` = group (`SpawnEntityResource`) or direct `SCR_AIGroup.AddWaypoint`; `SCR_EditableGroupComponent.ClearWaypoints()`, `EnableCycledWaypoints(bool)`, `GetWaypointCount()` | we already add waypoints directly; command actions (`SCR_WaypointBaseCommandAction`) are UI only |
| Change group faction | `SCR_AIGroup.SetFaction(Faction)` (not for groups with players/slave) | `SCR_AIGroupFactionEditorAttribute` |
| Change entity faction | `SCR_FactionAffiliationComponent.SetAffiliatedFaction(f)` | `SCR_EntityFactionEditorAttribute`; arsenal: `SCR_ArsenalFactionEditorAttribute` |
| Give supplies | Conflict base: `SCR_CampaignMilitaryBaseComponent.SetSupplies(x)`; generic: `SCR_ResourceContainer.SetResourceValue/IncreaseResourceValue`, `generator.RequestGeneration(x)`, `consumer.RequestConsumtion(x)`; catalog spawner `AddSpawnerSupplies(x)` | GM attribute `SCR_SetSuppliesBaseEditorAttribute` only works on Campaign bases; vehicle variant `SCR_SetSuppliesVehicleEditorAttribute` [file not located] |
| Heal / repair | `SCR_HealEntitiesContextAction`, `SCR_HealthEditorAttribute` -> damage manager `FullHeal()`/`SetHealthScaled` [verify exact]; support stations for in-world | |
| Arsenal config | `SCR_ArsenalManagerComponent.SetArsenalGameModeType_S(type, playerID)`, arsenal enabled/types via `SCR_ArsenalComponent` setters (attributes `SCR_ArsenalEnabledTypesEditorAttribute`, `...BaseEditorAttribute`) | |
| Tasks | `SCR_TaskSystem.CreateTask/AssignTask/SetTaskState` (see `commanding-and-tasks.md`) | already used by missions |
| Game mode | `SCR_BaseGameMode.EndGameMode(...)`, spawn point enabling (`SCR_SpawnPoint.SetSpawnPointEnabled_S`) | already used |
| Scenario Framework | `SCR_ScenarioFrameworkAction*` classes (spawn, delete, set faction, set resource enabled `SCR_ScenarioFrameworkActionSetResourceTypeEnabled`...) | usable as objects, optional |

Conclusion for 5a: the tier-2 tool registry can wrap these calls 1:1 (spawn_group, delete, set_transform, set_group_faction, set_entity_faction, grant_supplies, add/clear waypoints, create_task). There is no need for a "raw GM command by name" tier;
the editor action layer is not a callable API (needs `SCR_EditorManagerEntity` + selected entities + cursor), so option A (fixed requests) and B (registry) both build on the same component calls. `SCR_EditableEntityComponent` requires the prefab to carry an editable component, which vanilla prefabs do and many mod prefabs do not [verify per RHS vehicle].

## 5. Ideas: FOBs per war mode (brief)
- **Short conflict** (time-boxed, tickets): FOBs are a tempo tool. HQ orders `ESTABLISH_FOB` once per side near the front; built **instantly (fake build)** after an engineer squad arrives, costs a lump of supplies; gives forward respawn (spawn point + squad-join command post), arsenal box and a small depot (jeep/truck only). Destroyed FOB = ticket penalty. Cap 1-2 per side so the front stays legible.
- **Long conflict** (no clock, attrition): FOBs are the economy. Timed build with progress (needs engineers + truck delivering supplies from HQ via real convoy), service tiers (depot -> heavy depot -> field hospital/repair), per-FOB supply stock, upkeep drain; losing the FOB loses stock; supply lines make convoy ambush/escort missions for human players (VISION 2a).
- **True endless** (persistent ebb and flow): FOBs are semi-disposable and auto-managed: HQ re-establishes at the front line after each push, FOBs act as the ticket-refill and reinforcement points (income = FOB count in contact), unattended FOBs "decay" to dormant and are re-garrisoned by `SCR_DefenderSpawnerComponent`-style defenders; no explicit build UI for players, optional GM-style tool for admins.
- Common: one `IBuildExecutor` (instant | timed | engineer-driven) chosen by mode param; one `CLW_Fob` record (position, services, stock, faction) re-keyed with the sides; services only through the catalog spawner + support stations so RHS/WCS vehicles appear without code.

## 6. Open items for the M3 spike (all [verify])
1. Does a script-placed `VehicleService_US` prop work in `GameMode_Base` (populated actions, request flow, `GetFaction()` after re-key)? Try with `m_bSuppliesConsumptionEnabled 0` first.
2. Do the RHS vehicle catalog entries carry `SCR_EntityCatalogSpawnerData` (otherwise our `CLW_ForceCatalog` stays the source and the depot is bypassed)?
3. Supplies on a plain ArsenalBox_US in CLW_GameMode (greyed items?), `FRIENDLY_AND_FACTION_ITEMS_ONLY` with the own faction.
4. Static spawn of `FieldHospital_M_Conflict_US_01` outside Campaign: `SCR_ServicePointComponent` logs, heal action works, `SpawnAsOffline` default.
5. GRS-USSR + GRS-Footwear catalog override order; item counts per faction.
