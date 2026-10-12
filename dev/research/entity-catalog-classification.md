# Entity catalog auto-discovery and generic role classification

Question: for factions without a hand-written profile (RHS, WCS, ...), how do we enumerate buyable vehicles/groups
from the faction entity catalogs and classify them (infantry, recon, motorized, light, mech, armor, armored_recon)
without hard-wiring vanilla content? Verified against vanilla scripts/configs (game_read) and local RHS-StatusQuo.
Labels: [SRC] vanilla source/config read, [LOCAL] read in a local Workshop pack, [WIKI] BI wiki/search, [forum], [hyp] untested.

## 1. API sequence (all [SRC] scripts/Game/EntityCatalog/*)

```
SCR_Faction f = SCR_Faction.Cast(GetGame().GetFactionManager().GetFactionByKey(key));
SCR_EntityCatalog veh = f.GetFactionEntityCatalogOfType(EEntityCatalogType.VEHICLE);   // (type, bool printNotFound = true)
SCR_EntityCatalog grp = f.GetFactionEntityCatalogOfType(EEntityCatalogType.GROUP);
// alt: SCR_EntityCatalogManagerComponent.GetInstance().GetFactionEntityCatalogOfType(type, factionKey | faction)
array<SCR_EntityCatalogEntry> list = {};
veh.GetEntityList(list);                 // all ENABLED entries (disabled ones are removed in InitCatalog)
foreach (SCR_EntityCatalogEntry e : list)
{
    ResourceName prefab = e.GetPrefab();
    SCR_EntityCatalogSpawnerData sd = SCR_EntityCatalogSpawnerData.Cast(e.GetEntityDataOfType(SCR_EntityCatalogSpawnerData));
    array<EEditableEntityLabel> labels = {};
    e.GetEditableEntityLabels(labels);   // auto + authored labels, from prefab UI info (see below)
    SCR_EditableVehicleUIInfo ui = SCR_EditableVehicleUIInfo.Cast(e.GetEntityUiInfo());
}
```

- Catalogs are `SCR_EntityCatalogMultiList` (ONLY merged on init: `m_aMultiLists` entries are merged into the flat list, e.g. "Vehicles"
  and "Helicopters" sub-lists in Vehicles_EntityCatalog_US.conf), so `GetEntityList` already returns the flattened list. [SRC]
- `EEntityCatalogType`: NONE=0, OBJECT=10, CHARACTER=20, VEHICLE=30, GROUP=40, COMPOSITION=50, ITEM=60, SYSTEM=70,
  WEAPONS_TRIPOD=80, RESPAWN_LOADOUT=90, SUPPLY_CONTAINER_ITEM=100. [SRC]
- `SCR_EntityCatalog` filters: `GetEntityList`, `GetEntryWithPrefab(prefab)`, `GetCatalogEntry(idx)`, `GetEntityListWithLabel(label, out)`,
  `GetEntityListExcludingLabel`, `GetFullFilteredEntityListWithLabels(out, incl, excl, needsAllIncluded=true)`,
  `GetEntityListWithData(typename, out, out dataList=null)`, `GetEntityListExcludingData`, `GetFullFilteredEntityListWithData`,
  `GetFullFilteredEntityList(out, inclLabels, exclLabels, inclData, exclData, needAllLabels=true, needAllData=true)`. [SRC]
- `SCR_EntityCatalogEntry`: `GetPrefab()`, `IsEnabled()`, `GetCatalogIndex()`, `GetCatalogParent()`, `GetEntityUiInfo()` (SCR_UIInfo),
  `GetEntityName()`, `GetEditableEntityLabels(out)`, `HasEditableEntityLabel(l)`, `HasAnyEditableEntityLabels(arr)`,
  `HasAllEditableEntityLabels(arr)`, `GetEntityDataOfType(typename)` (enabled + IsInherited), `GetEntityDataList(out)`,
  `HasEntityDataOfType(s)`, `HasAll/AnyEntityDataOfTypes(arr)`. [SRC]
- `InitEntry` (called by `InitCatalog`, runs at faction/catalog init, PostInit one frame later via CallLater) loads the prefab
  resource (`Resource.Load`), finds the `SCR_EditableEntityComponent` source and stores the UI info. So **catalog entries expose labels/name
  without spawning**; if no editable component then `GetEntityUiInfo()==null` and all label checks return false. [SRC]
  Timing: before `PostInitCatalog` ran, `SCR_EntityCatalogSpawnerData.GetSupplyCost()` may still be 0; query in or after a delayed call. [hyp]

### Labels without a catalog entry (any prefab, no spawn) [SRC SCR_EditableEntityUIInfo / SCR_EditableEntityComponentClass]
```
SCR_EditableEntityUIInfo ui = SCR_EditableEntityUIInfo.ExtractEditableUIInfoFromPrefab(prefab);   // static; Resource load + component source + GetInfo
ui.GetEntityLabels(out arr); ui.HasEntityLabel(l); ui.GetEntityBudgetCost(out budgets); ui.GetEntityAndChildrenBudgetCost(out budgets);
ui.GetFactionKey();  ui.GetEntityType();
// lower level:
Resource r = Resource.Load(prefab);
IEntityComponentSource src = SCR_EditableEntityComponentClass.GetEditableEntitySource(r);   // overload for Resource or IEntitySource
SCR_EditableEntityUIInfo i = SCR_EditableEntityComponentClass.GetInfo(src);
SCR_EditableEntityComponentClass.GetEntitySourceBudgetCost(src, out budgets) / GetEntitySourceChildrenBudgetCosts(...)
```
- Labels live in `SCR_EditableEntityUIInfo.m_aAuthoredLabels` (hand-set) and `m_aAutoLabels` (plugin-filled: FACTION_*, SIZE_*, GROUPSIZE_*, ROLE_* on characters).
  `GetEntityLabels` = auto + authored (both arrays inserted, may contain duplicates). Both are `private`, but accessible through the BaseContainer
  `src.GetObject("m_UIInfo")` -> `.Get("m_aAuthoredLabels", arr)` (same trick vanilla uses for `m_SlotPrefab`). [SRC]
- `IEntityComponentSource.Get` on a prefab returns the **merged-with-ancestors** value (prefab inheritance is resolved by the engine). [SRC pattern, vanilla relies on it]
- `SCR_EditableVehicleUIInfo` extras (all auto-filled by an editor plugin, so can be stale or empty in 3rd-party packs): `m_aOccupantFillCompartmentTypes`
  (`ECompartmentType` PILOT / CARGO / TURRET), `m_aCrewEntityBudgetCost` and `m_aPassengerEntityBudgetCost` (AI-budget value = number of default crew / default passengers). [SRC]

## 2. EEditableEntityLabel (scripts/Game/Editor/Enums/EEditableEntityLabel.c) [SRC]
Relevant values (full list in file):
- Vehicle type: `VEHICLE_CAR=50, VEHICLE_HELICOPTER=51, VEHICLE_AIRPLANE=52, VEHICLE_APC=53, VEHICLE_TRUCK=54, VEHICLE_TURRET=55`.
  **There is NO tank / IFV / MBT label.**
- Traits: `TRAIT_ARMOR=20, TRAIT_ARMED=21, TRAIT_UNARMED=22, TRAIT_REFUELING=23, TRAIT_MEDICAL=24, TRAIT_ARMORPIERCING=25, TRAIT_AMPHIBIOUS=26,
  TRAIT_REARMING=27, TRAIT_SERVICE=29, TRAIT_OPTICS=33, TRAIT_SUPPRESSIVE=34, TRAIT_RADIO=35, TRAIT_PASSENGERS_SMALL=36, TRAIT_PASSENGERS_LARGE=37,
  TRAIT_EXPLOSIVE=38, TRAIT_REPAIRING=39, TRAIT_SUPPLYSTORAGE_SMALL=200 / _LARGE=210 / _FULL=220, TRAIT_HELI_CREW=270, TRAIT_VEHICLE_CREW=271,
  TRAIT_MORTAR=281, TRAIT_ESSENTIAL=296, TRAIT_GUARD=297, TRAIT_LOGISTICS=298`.
- Character roles: `ROLE_ANTITANK=40, ROLE_GRENADIER=41, ROLE_LEADER=42, ROLE_MACHINEGUNNER=43, ROLE_MEDIC=44, ROLE_RADIOOPERATOR=45, ROLE_RIFLEMAN=46,
  ROLE_SHARPSHOOTER=47, ROLE_AMMOBEARER=48, ROLE_SCOUT=49, ROLE_SAPPER=140, ROLE_GUARD=141`.
- Groups: `GROUPTYPE_INFANTRY=60, GROUPTYPE_MOTORIZED=61, GROUPTYPE_AIRMOBILE=62, GROUPTYPE_ESSENTIAL=63`; `GROUPSIZE_SMALL=70, _MEDIUM=71, _LARGE=72`.
- Size: `SIZE_XS=120 .. SIZE_XL=124`. Faction: `FACTION_USSR=10, FACTION_US=11, FACTION_FIA=12, FACTION_CIV=13, FACTION_NONE=14`.
  `CONTENT_MODDED=1001`. Mods extend the enum (`modded enum EEditableEntityLabel` in RHS-StatusQuo adds e.g. `FACTION_RHS_AFRF`), so never compare against faction labels. [LOCAL]
- Entity type: `ENTITYTYPE_OBJECT=1 .. ENTITYTYPE_GROUP=4 ..`.

### Labels actually carried by vanilla vehicles (merged ancestor chain, prefab read) [SRC]
| Prefab | authored labels | auto labels | cargo/turret | AI budget crew / passengers | CAMPAIGN cost (supply) |
|---|---|---|---|---|---|
| M923A1_transport (US truck) | TRAIT_PASSENGERS_LARGE, TRAIT_SUPPLYSTORAGE_LARGE (analog Ural, verified for Ural) | none | Pilot only listed; cargo via base | 1 / 14 | 200 |
| Ural4320_transport | TRAIT_PASSENGERS_LARGE, TRAIT_SUPPLYSTORAGE_LARGE | none | | 1 / 14 | 200 |
| M151A2_M2HB | ENTITYTYPE_VEHICLE, TRAIT_ARMED, TRAIT_REPAIRING | none | fill types: TURRET | 2 / 1 | 280 |
| M1025 (unarmed) | **TRAIT_ARMOR** (armored Humvee, not a tank!) | none | Pilot + 3 Cargo | 1 / 3 | 240 |
| M1025_armed_M2HB | TRAIT_ARMOR + TRAIT_ARMED; fill types TURRET (turret in Roof slot) | none | Pilot + 3 Cargo + turret | | 475 |
| UAZ469_PKM | TRAIT_ARMED | none | Pilot + Cargo | 2 / 1 | 280 |
| LAV25 (via LAV25_base) | TRAIT_ARMOR TRAIT_ARMED TRAIT_PASSENGERS_LARGE TRAIT_AMPHIBIOUS **VEHICLE_APC** TRAIT_SUPPLYSTORAGE_SMALL | FACTION_US FACTION_NONE SIZE_M | fill CARGO PILOT TURRET | 2 / 7 | 1800 |
| BTR70 | TRAIT_ARMOR TRAIT_ARMED TRAIT_PASSENGERS_LARGE TRAIT_SUPPLYSTORAGE_SMALL TRAIT_AMPHIBIOUS VEHICLE_APC | FACTION_USSR SIZE_M | Pilot + 7 Cargo (+turret) | 3 / 6 | 975 |
| BRDM2 (via BRDM2_base) | TRAIT_ARMOR TRAIT_ARMED **TRAIT_PASSENGERS_SMALL** TRAIT_SUPPLYSTORAGE_SMALL TRAIT_AMPHIBIOUS VEHICLE_APC | FACTION_NONE SIZE_M | fill CARGO PILOT TURRET; own list 1 CargoCompartmentSlot "Passenger" | 3 / 1 | 900 |
Notes: M923A1_transport values partly inferred from the Ural identical structure; I read the Ural fully and M923 crew/passenger budget (1/14, 200). Helicopter catalogs sit in the same VEHICLE catalog ("Helicopters" sub-list); UH1H etc. carry VEHICLE_HELICOPTER presumably [hyp] (not read).
- Merged-view caveat: prefab-inspect only shows the leaf override for arrays tagged `+`; always read ancestors (game_read) or use the engine's merged `BaseContainer.Get`.
- The CAMPAIGN cost is the first `SCR_EntityBudgetValue` ID {5EDC86E4AF8908B6} declared in `Vehicle_Base.et` with `m_BudgetType CAMPAIGN` (default enum value is PROPS=0, so a budget entry without explicit type means PROPS in the leaf; leaf prefabs override only `m_Value` by ID). Minimum rank is a `RANK_*` budget entry. [SRC]

### Labels carried by vanilla groups [SRC]
Groups are `SCR_AIGroup` with `SCR_EditableGroupComponent` -> `SCR_EditableGroupUIInfo`. They carry **only** traits + auto GROUPSIZE; GROUPTYPE_* is almost unused.
| Group | authored | auto | CAMPAIGN cost | AI children budget |
|---|---|---|---|---|
| Group_US_RifleSquad | TRAIT_ARMORPIERCING TRAIT_SUPPRESSIVE TRAIT_EXPLOSIVE | GROUPSIZE_LARGE | 675 | 9 |
| Group_US_MachineGunTeam | TRAIT_SUPPRESSIVE TRAIT_REARMING | GROUPSIZE_SMALL | 150 | 2 |
| Group_US_Team_LAT | TRAIT_ARMORPIERCING TRAIT_SUPPRESSIVE | GROUPSIZE_MEDIUM | 300 | 4 |
| Group_US_ReconTeam | none | GROUPSIZE_SMALL | **1** (placeholder-like) | 2 |
| Group_US_Transport | TRAIT_ESSENTIAL TRAIT_LOGISTICS GROUPTYPE_ESSENTIAL | GROUPSIZE_MEDIUM | 150 | 2 |
- Group role is therefore NOT in labels. Use: (a) member list `SCR_AIGroup.m_aUnitPrefabSlots` (array<ResourceName> of character prefabs, read via `IEntitySource`/BaseContainer
  `Get("m_aUnitPrefabSlots", arr)`), (b) each character prefab's `SCR_EditableCharacterComponent` UI info labels (verified: Character_US_LAT auto `ROLE_ANTITANK`, authored `TRAIT_ARMORPIERCING`, `SIZE_M`),
  (c) `SCR_GroupIdentityComponent.m_MilitarySymbol` (`m_Icons` flags: INFANTRY, ANTITANK, SUPPLY, 8193 = MG-like combo; `m_Amplifier` SQUAD/TEAM) (enum names not fully verified [hyp]), (d) group prefab name / `Name` string (weak).
- Hired "ambient" groups (`Prefabs/Groups/.../AmbientPatrols/*_NotSpawned.et`) are in a "Not Spawned" multi list and have only `SCR_EntityCatalogAmbientPatrolData` (no SpawnerData): exclude from buyable lists. `SCR_EGroupType` (TEAM_SENTRY, TEAM_MG, TEAM_AT, FIRETEAM, SQUAD_RIFLE, TEAM_SNIPER, TEAM_MG_ELITE, PARTISAN_SNIPER, PARTISAN_TEAM) there is a handy role hint but only for these. [SRC]

## 3. Catalog entry data classes [SRC]
- `SCR_BaseEntityCatalogData` (base: `IsEnabled()`, InitData/PostInitData).
- `SCR_EntityCatalogSpawnerData` (the "buyable via spawner/Conflict vendor" marker): `GetSupplyCost()` (= CAMPAIGN budget of the prefab's `GetEntityAndChildrenBudgetCost`, set in `InitData`),
  `GetMinimumRequiredRank()` (`SCR_ECharacterRank`, from `RANK_*` budgets, default PRIVATE) / `HasRequiredRank(rank)`, `GetValidSlotSizes()` / `CanSpawnInSlot(SCR_EEntitySpawnerSlotType)`
  (`m_eSlotTypes` flags; groups use 7, small vehicles 24, `VEHICLE_MEDIUM`, `VEHICLE_LARGE` for APC/trucks/helis), `GetEntityCount()`, `GetOverwriteName()`,
  `GetVariantDataList` / `GetVariantPrefabList` / `GetRandomDefaultOrVariantPrefab()` (variants like MERDC camo, `ESpawnerVariantType.VEHICLE_MERC`).
- `SCR_EntityCatalogEditorData`: `GetValidEditorModes()` / `IsValidInEditorMode(EEditorMode)`; `m_eValidEditorModes 33` = edit-type modes, `BUILDING` = Conflict build only.
- `SCR_EntityCatalogAmbientPatrolData`, `SCR_EntityCatalogLoadoutData`, `SCR_EntityCatalogSpawnerOverrideData`, `SCR_EntityCatalogSupportStationResupplyData`,
  `SCR_NonArsenalItemCostCatalogData`, `SCR_ResourceContainerItemData`, `SCR_EntityCatalogIdentityItemData` exist (not needed). No `SCR_EntityCatalogAIGroupData` class; no "playable/AI" flag.
- Catalog pitfalls in vanilla US/USSR vehicle catalogs:
  - Several entries lack SpawnerData (editor-only); several are duplicates (`*_MERDC`, `Conflict_Variants/*_Conflict.et` with EditorData `BUILDING`). Dedupe by (labels, seats, cost) or base name; prefer entries having SpawnerData.
  - Support vehicles (tanker, ammo, repair, command, arsenal, ambulance, engineer) share the transport hull: classify by labels `TRAIT_MEDICAL/REFUELING/REARMING/REPAIRING/SERVICE/ARSENAL/LOGISTICS` and exclude from "motorized".
  - Costs are in supply units: derive from `SCR_EntityCatalogSpawnerData.GetSupplyCost()` when present, else from the prefab's CAMPAIGN budget (`GetEntityAndChildrenBudgetCost`), else a role fallback table.

## 4. Seats and weapons without spawning
Possible with caveats. Prefab sources via BaseContainer ([SRC] structure confirmed by reading prefabs; exact BaseContainer calls [hyp]):
1. Cheap path: `SCR_EditableVehicleUIInfo` plugin-filled fields (accessible via `src.GetObject("m_UIInfo")`):
   - `m_aOccupantFillCompartmentTypes` contains `TURRET` -> has a turret seat; `CARGO` -> has cargo; `PILOT`.
   - `m_aPassengerEntityBudgetCost` AI value = default passenger (cargo) count; `m_aCrewEntityBudgetCost` AI value = crew count (BTR70: crew 3, passengers 6; LAV25: 2 / 7; Ural 1 / 14; UAZ_PKM 2 / 1).
   - Caveats: plugin-generated, may be empty/stale in 3rd-party packs (M1025 and Ural show empty fill types despite seats); not every vehicle has the values. [SRC observation]
2. Exact path: walk `SCR_BaseCompartmentManagerComponent` source: `CompartmentSlots` list items by class name `PilotCompartmentSlot` / `CargoCompartmentSlot` / `TurretCompartmentSlot`
   (`SeatType`: gunner turret slot = 5, commander cargo slot = 1; commander slot may be CargoCompartmentSlot with `AutoConnectToTurret`). **Turrets are separate sub-prefabs**:
   `SlotManagerComponent.Slots` entries (`RegisteringComponentSlotInfo Turret/Roof`) with `Prefab`, `RegisterCompartments 1`, `RegisterWeapons 1`. Compartments of the turret prefab
   (own `SCR_BaseCompartmentManagerComponent`, e.g. LAV25_turret_base: TurretCompartmentSlot "TurretCompartment" + CargoCompartmentSlot "Commander") count toward the vehicle only via these slots.
   Weapon presence: turret prefab has `WeaponSlotComponent` (with `WeaponTemplate` resource) and `TurretControllerComponent`/`TurretComponent`/`MultiFireWeaponManagerComponent`.
   Recursion needed: vehicle -> slots with RegisterCompartments/RegisterWeapons -> sub-prefab compartments + WeaponSlotComponent. Also `UserActionContext`/`m_DefaultOccupantData.m_sDefaultOccupantPrefab` per slot (default crew prefab).
   Weapon in plain vehicle components (not turret): look for `WeaponSlotComponent` / `BaseWeaponManagerComponent` on the vehicle itself.
3. Cheapest armed test: label `TRAIT_ARMED` (authored; absent on LAV25_turret-only sub-prefab, but present on LAV25/BTR70/BRDM2/UAZ_PKM/M151/M1025_armed; **absent in unlabelled mod vehicles possible**).
   Combine: armed = TRAIT_ARMED || any WeaponSlotComponent in registered sub-prefabs || TURRET in fill types.
4. Counting after spawning is always available (`SCR_BaseCompartmentManagerComponent.GetCompartments(array<BaseCompartmentSlot>)`, `BaseCompartmentSlot.GetType()` -> ECompartmentType; `GetFreeCompartmentsOfType`). Use as fallback/verification only for the chosen entries (spawn+delete cost).

## 5. RHS / WCS packs [LOCAL, read from the pak, plus [WIKI]/[forum]]
- RHS-StatusQuo: faction conf `Configs/Factions/RHS_RF_MSV.conf` is `SCR_Faction` (FactionKey `RHS_AFRF`, `m_FactionLabel FACTION_RHS_AFRF`) with
  `m_aEntityCatalogs { SCR_EntityCatalogMultiList ... CHARACTER / VEHICLE / GROUP / WEAPONS_TRIPOD / ITEM }` referencing
  `Configs/EntityCatalog/RHS_MSV/MSV_Vehicles.conf`, `MSV_Groups.conf`, etc.: identical structure and the same data classes (EditorData + SpawnerData) as vanilla. [LOCAL]
  So `GetFactionEntityCatalogOfType` works for RHS factions (same for `RHS_US_USMC`, `RHS_ION`).
- RHS vehicle prefabs use the same labels (counts over the pack: VEHICLE_TRUCK 23, VEHICLE_CAR 20, VEHICLE_APC 8, TRAIT_ARMOR 10, TRAIT_ARMED 8, ROLE_SCOUT 21,
  ROLE_SHARPSHOOTER 16, GROUPTYPE_* 0). Example T90M2_base: `ENTITYTYPE_VEHICLE TRAIT_ARMOR TRAIT_ARMED CONTENT_MODDED`, auto `FACTION_USSR SIZE_M` (**tank = ARMOR+ARMED without VEHICLE_APC and without passenger label**). [LOCAL]
  RHS groups carry the same trait style (TRAIT_ARMORPIERCING / TRAIT_SUPPRESSIVE / TRAIT_REARMING, GROUPSIZE_*), no GROUPTYPE. [LOCAL]
- WCS: `WCS_Vehicles` / `WCS_Core` do NOT make own catalogs; they extend vanilla US/USSR catalogs via `Configs/EntityCatalog/US/Vehicles/WCS_M1A1.conf`, `.../WCS_Vehicles_LAV25.conf`, `.../USSR/Vehicles/WCS_Vehicles_BTR70.conf`
  (`SCR_EntityCatalogMultiListEntry` appended; EditorData `EDIT`), and `Configs/Factions/US_Campaign.conf` etc. So WCS content appears inside the vanilla faction catalog (already merged by InitCatalog). [LOCAL]
- BI wiki "Arma_Reforger:Entity_Catalog" describes the same system (catalog per type, entries need an editable entity for name/labels). [WIKI] (page returned 403 via WebFetch; content seen in search snippet only).
- [forum] mods "RHS - Russian Vehicle Override", "CPO Factions" confirm that third parties plug vehicles/groups into faction catalogs (search snippets only).

## 6. Proposed generic classifier

Inputs per vehicle entry: labels L (`GetEditableEntityLabels`), cargo seat count C (passenger budget / compartment walk), crew count K, armed A, tracked/wheeled (not needed), supplyCost S.

Exclusions first (not buyable force): entry without enabled `SCR_EntityCatalogSpawnerData` (when any entry has it), L has any of
`VEHICLE_HELICOPTER, VEHICLE_AIRPLANE, VEHICLE_TURRET, TRAIT_MEDICAL, TRAIT_REFUELING, TRAIT_REARMING, TRAIT_REPAIRING, TRAIT_SERVICE, TRAIT_ARSENAL, TRAIT_LOGISTICS, TRAIT_MORTAR`
(logistics trucks stay "motorized" only if you want supply trucks; otherwise exclude), or `EEditableEntityType` != VEHICLE.

| kind | rule (first match wins) |
|---|---|
| armored_recon | `VEHICLE_APC` (or ARMOR+ARMED) AND armed AND C <= 2 (or `TRAIT_PASSENGERS_SMALL` without `TRAIT_PASSENGERS_LARGE`), e.g. BRDM2 (C=1, PASSENGERS_SMALL) |
| mech | armed AND (`VEHICLE_APC` OR (`TRAIT_ARMOR` AND C >= 3 AND no `VEHICLE_CAR`)) AND (C >= 3 OR `TRAIT_PASSENGERS_LARGE`), e.g. LAV25 (C=7), BTR70 (C=6) |
| armor | armed AND `TRAIT_ARMOR` AND C == 0 AND no `VEHICLE_APC` (tank/TD; RHS T90M2), or ARMED+ARMOR with `SIZE_L/XL` |
| light | armed AND NOT (ARMOR with C>=3 and APC): UAZ_PKM, M151 M2HB, M1025_armed (note M1025 has `TRAIT_ARMOR` but no APC label and is a car: ARMOR must not alone mean heavy) |
| motorized | NOT armed AND (`TRAIT_PASSENGERS_LARGE` OR `VEHICLE_TRUCK` OR C >= 4) |
| (skip) | unarmed utility car (UAZ469, M1025 unarmed, M998): optional "light transport" or ignore |

Disambiguation tips: cargo count dominates label noise (labels are authored by hand, partially missing in mods); `VEHICLE_APC` is the only hull-type label that is reliable; `TRAIT_ARMOR`
is also on the M1025 Humvee. Deduplicate camo/Conflict variants. Group kinds: infantry = no vehicle members, avg unit role labels mostly `ROLE_RIFLEMAN/LEADER/GRENADIER/MACHINEGUNNER/ANTITANK/MEDIC/AMMOBEARER`;
recon = majority `ROLE_SCOUT` / `ROLE_SHARPSHOOTER` units or `GROUPTYPE_*`-less name match "Recon|Sniper|Scout" (weak); exclude groups with `TRAIT_LOGISTICS/TRAIT_ESSENTIAL` (transport/crew) and `ROLE_GUARD`/`TRAIT_GUARD`;
size from `GROUPSIZE_*` or `m_aUnitPrefabSlots.Count()`.

Costs: supply S from SpawnerData; group CAMPAIGN cost of 1 (ReconTeam) is suspect -> treat S < some floor as "unknown" and fall back to sum of member character costs [hyp].
