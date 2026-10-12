# Vehicles, mechanized units and runtime group spawning (vanilla)

Labels: **[SRC]** = read from vanilla scripts/prefabs in the pak (unpacked copy of `scripts/` and `Prefabs/{Vehicles,Groups,AI}` used for grep); **[WIKI]** = API index / wiki; **[hyp]** = inference, untested in game.
GUIDs: collected by scanning every `{GUID}path` reference in all vanilla .et/.conf/.c (the pak has no .meta; `asset_search` returns no GUIDs). A prefab that nobody references would be missing (none of the ones below are).
Game version of the data: current install (1.8/1.9 era scripts, `SCR_AIGroup` has the SCR_AIWorld spawn queue and lifecycle policy).

## 1. Vehicles (Prefabs/Vehicles/Wheeled/...). Factions: `m_sFaction` US / USSR / FIA. NO tracked vehicles exist (only `Core/TrackedVehicle_Base.et`, no concrete prefab). Folders: Wheeled, Helicopters, Core, Generic.

Seats = Pilot + Cargo slots (+ Turret slots for armed). "Default occupants" = what `SpawnDefaultOccupants` puts in each seat (see section 2). All have `SCR_AIVehicleUsageComponent` (m_eVehicleType CAR / TRUCK / SUPPLY_TRUCK / APC).

| Role | Prefab | GUID | Seats [SRC] | Armed |
|---|---|---|---|---|
| US truck | M923A1/M923A1_transport.et | {F1FBD0972FA5FE09} | cab: pilot + 2 cargo (Passenger_r01, _m01); bed seats come from child part `VehParts/M923A1_cargo.et` {5A7E3ECE4599214B}; editor passenger budget 14 | no |
| US truck covered | M923A1/M923A1_transport_covered.et | {81FDAD5EB644CC3D} | as above | no |
| US truck (MERDC camo) | M923A1_transport_MERDC {3F2AA823B6C65E1E}, _covered_MERDC {04BDACC0BB83284E} | | | no |
| US tanker/command/ammo/repair | M923A1_tanker {2BE1F8B9299B67C1}, _command {36BDCC88B17B3BFA}, _ammo {92264FF932676C13}, _repair {A042ACE5C2B13206}, _arsenal {1FF144F8A209239D}, _engineer {2D74C39A650A3030} | | pilot + 2 | no |
| USSR truck | Ural4320/Ural4320_transport.et | {16C1F16C9B053801} | same layout; bed from `VehParts/Ural4320_cargo.et` {58B3FB7F102A8B1B}; passenger budget 14 | no |
| USSR truck covered | Ural4320/Ural4320_transport_covered.et | {D9B91FAB817A6033} | | no |
| USSR tanker | Ural4320_tanker {4C81D7ED8F8C0D87} | | | no |
| FIA truck | Ural4320_FIA_transport {16E32C3ABEAFC2C6}, _covered {B70E6D12A8EC2410} | | | no |
| US light utility | M998/M998_covered.et {B55C6990A6A9411B}, M998_uncovered {5674FAEB9AB7BDD0}, M998_covered_long {9B1BF9644E0378D6} | | pilot + 3 cargo (Passenger_r01, l02, r02) | no |
| US light armed | M998/M1025_armed_M2HB.et | {3EA6F47D95867114} | pilot + 3 cargo + M2HB turret (crew budget 2 = driver+gunner) | YES (.50) |
| US light armed (Conflict) | Conflict_Variants/M1025_armed_M2HB_Conflict.et | {428B2BC55F1628E0} | same | YES |
| US light unarmed | M998/M1025.et | {4A71F755A4513227} | pilot + 3 cargo | no |
| US jeep | M151A2/M151A2.et {F649585ABB3706C4}; M151A2_transport {47D94E1193A88497}; M151A2_M2HB {F6B23D17D5067C11} | | pilot + 1 cargo (+ gun mount turret on M2HB) | M2HB variant armed |
| US IFV/APC | LAV25/LAV25.et | {0FBF8F010F81A4E5} | pilot + 6 cargo slots (l01 r01 r02 l03 r03 l02) + turret(s) gunner/commander; crew budget 3 | YES (25 mm + coax) |
| US LAV Conflict | Conflict_Variants/LAV25_Conflict.et | {590E677BBCD1D26E} | | YES |
| USSR light | UAZ469/UAZ469.et {259EE7B78C51B624}; UAZ469_uncovered {16A674FE31B0921C} | | pilot + 1 cargo (base; more in covered variants via child slots [hyp]) | no |
| USSR light armed | UAZ469/UAZ469_PKM.et | {0B4DEA8078B78A9B} | pilot + 1 cargo + PKM turret | YES |
| USSR van | UAZ452/UAZ452_transport.et {1FBB492E86002BF5}, _cargo {01F65EFB8D767A91} | | pilot + cargo | no |
| USSR APC | BTR70/BTR70.et | {C012BB3488BEA0C2} | pilot + 7 cargo (r02 l03 r03 l04 r04 l05 r05) + gunner turret + commander turret; crew budget 3, passenger budget 6 | YES (KPV 14.5 + PKT) |
| USSR BTR Conflict | Conflict_Variants/BTR70_Conflict.et | {BD75A18920BD1278} | | YES |
| USSR recon APC | BRDM2/BRDM2.et | {254289B9C09904AB} | pilot + 1 cargo + gunner/commander turrets | YES |
| BRDM Conflict | Conflict_Variants/BRDM2_Conflict.et | {F7B9B434FCFF00B3} | | YES |
| FIA | BTR70_FIA {B47110AA1A806556}, BRDM2_FIA {442939C9617DF228}, UAZ469_PKM_FIA {22B327C6752EC4D4}, UAZ469_FIA {F7E9AA0C813EABDA}, UAZ452_transport_FIA {BDE16A6AE9942D44} | | | PKM/BTR/BRDM armed |

Notes
- **[SRC]** `m_aOccupantFillCompartmentTypes` in the editable component: APC/IFV = `CARGO PILOT TURRET`, trucks = `PILOT CARGO`. These are the types the Game Master "fill vehicle" uses.
- **[SRC]** Base classes: Core/Wheeled_Base.et {..} (AIVehicleUsage GROUND_VEHICLE_WHEELED, PilotCompartment), Wheeled_Car_Base (CAR), Wheeled_Truck_Base (TRUCK), Wheeled_APC_Base (APC).
- Conflict_Variants = the versions the Conflict mode spawns (extra supply/budget components) [hyp: not diffed]. Plain prefabs work for our purposes.
- Armed/crewed truth: turret seats are `TurretCompartmentSlot` in child part prefabs (e.g. `BTR70/VehParts/Turret/BTR70_Turret.et`, `LAV25/VehParts/Turret/LAV25_turret_base.et`, `BRDM2/VehParts/Turrets/BRDM2_turret.et`). `ECompartmentType.TURRET` fills them.

## 2. Groups, crews, and putting groups in vehicles

### 2a. Group prefabs with vehicles/crews
- **[SRC]** NO vanilla group prefab references a vehicle (grep of Prefabs/Groups for `Vehicles/` = 0 hits). `SCR_AIGroup.m_aStaticVehicles` (array of entity NAMES, `AddVehiclesStatic`) is the only static link: looks the vehicle up by `World.FindEntityByName` and calls `m_GroupUtilityComponent.m_VehicleMgr.TryAddVehicle`. Not useful for runtime spawn unless we `SetName` the vehicle.
- "Transport" groups: `Group_US_Transport` {727C134094032B1F} (2x Campaign_US_Player_Driver), `Group_USSR_Transport` {29DFCC25F263026B} (2x Campaign_USSR_Player_Driver). `Group_USSR_WithDriver` = rifle squad variant. No dedicated crew group; crews are created by the vehicle itself (below).
- Crew characters: `Character_US_Crew` {E1CB513B8B9B08F4}, `Character_USSR_Crew` {9FFEF10757E742EB}; BTR70/BRDM2 pilot+gunner default = USSR_Crew, LAV25 = US_Crew.

### 2b. How SCR_AIGroup spawns its members [SRC, SCR_AIGroup.c]
- `m_aUnitPrefabSlots` (array of character prefab ResourceNames, slot order = leader first). `m_bSpawnImmediately` default 1.
- `EOnInit`: if not edit mode and `m_bSpawnImmediately` -> `RequestSpawn(slots)` -> `ChimeraAIWorld.EnqueueSpawnRequest(this, slots, observerRange)`; **members are spawned one per AI-world tick** by `ExpandOneMember()` -> `SpawnGroupMember` -> `GetGame().SpawnEntityPrefabEx` at the group position (snap to terrain) -> `AddAIEntityToGroup` which calls `AIControlComponent.ActivateAI()` and `AddAgent`; faction set from group faction (`m_faction`, from the group prefab: Group_US_Base = US, Group_USSR_Base = USSR, FIA_Base = FIA). `Event_OnInit` fires at the last member (or `GetOnInit()`-style invoker; use `GetOnAllDelayedEntitySpawned()` / `GetOnInit()` if present: [hyp: exact public getters not verified]).
- So **no manual ActivateAI is needed**; but members are NOT present synchronously after `SpawnEntityPrefab(group)`: wait (poll `group.GetAgentsCount()` == slot count, or use the init invoker) before adding waypoints that depend on members, or add waypoints right away (group exists, waypoints are group-level: RuleCommander already does this).
- Dormancy: `SCR_EAIGroupLifecyclePolicy` Manual by default (does not despawn). Importance via `ChimeraAIGroup.SetImportance`.
- Navmesh not loaded at the spawn point: `SpawnGroupMember` stalls then proceeds (NAVMESH_STALL_LIMIT) [SRC]; spawn bases that are inside loaded area.
- `s_bIgnoreSpawning` static (`SCR_AIGroup.IgnoreSpawning(true)` before spawning the group prefab) = create the group WITHOUT members (use when we want to fill it ourselves).

### 2c. API to code against

(i) **Spawn a crewed vehicle** [SRC: SCR_ScenarioFrameworkActionQRFDispacher.c L330-350, SCR_EditableVehicleComponent.OccupyVehicleWithDefaultCharacters, SCR_BaseCompartmentManagerComponent L354-520]
```
Resource r = Resource.Load("{C012BB3488BEA0C2}Prefabs/Vehicles/Wheeled/BTR70/BTR70.et");
EntitySpawnParams p = new EntitySpawnParams(); p.TransformMode = ETransformMode.WORLD; p.Transform[3] = pos; // set rotation/yaw too, snap Y to surface
IEntity veh = GetGame().SpawnEntityPrefab(r, GetGame().GetWorld(), p);
SCR_BaseCompartmentManagerComponent cm = SCR_BaseCompartmentManagerComponent.Cast(veh.FindComponent(SCR_BaseCompartmentManagerComponent));
cm.GetOnDoneSpawningDefaultOccupants().Insert(OnOccupants);   // signature (SCR_BaseCompartmentManagerComponent cm, array<IEntity> occupants, bool wasCanceled)
array<ECompartmentType> t = {ECompartmentType.PILOT, ECompartmentType.TURRET}; // + ECompartmentType.CARGO for infantry
cm.SpawnDefaultOccupants(t);   // server only; one character per frame; ALL occupants of one call go into ONE new AIGroup
```
- Occupants come from each slot's `m_DefaultOccupantData.m_sDefaultOccupantPrefab`; the AI group is created by `BaseCompartmentSlot.SpawnCharacterInCompartment` from `Prefabs/AI/Groups/Group_Base.et` {000CD338713F2B5A}, faction taken from the first character (`InitFactionKey`). Characters are spawned above the vehicle, `ActivateAI()`, then `CompartmentAccessComponent.GetInVehicle(veh, slot, forceTeleport=true, ...)`.
- Get the group in the callback: `occupants[0]` -> `AIControlComponent.GetControlAIAgent().GetParentGroup()` -> `SCR_AIGroup`. Then `group.AddWaypoint(wp)` (exactly what QRF does).
- A second `SpawnDefaultOccupants` for the SAME type while pending/after is refused ("can only spawn the same compartment type once"); different types are queued (but queued types go to a new group? comment says types "grouped together" share a group; a later call = another group [hyp]). Safest: ONE call with all types you want.
- Default seat fill: BTR70 = Crew(pilot), SL, AR, AT, AAT, MG, SR, LAT (cargo, so one call with CARGO gives a full USSR squad + driver, gunner crews); LAV25 = US_Crew + TL, AR, GL, Rifleman, LAT; M1025 armed / Humr = Rifleman, GL, AR, LAT; M923A1 = Rifleman, GL (cab only); Ural = Rifleman, GL (cab only, truck bed has no defaults [hyp, not read from cargo part]); BRDM2 = Crew, Crew. Trucks therefore get only a driver + 1-2 passengers by default: to move a squad in a truck use (ii).
- Vanilla registers the vehicle as usable automatically: `SCR_AIGroupUtilityComponent.OnAgentCompartmentEntered` -> `AddUsableVehicle(SCR_AIVehicleUsageComponent.FindOnNearestParent(...))` and the driver AI drives when the group gets movement waypoints [SRC: SCR_AIGroupUtilityComponent L535-560, SCR_AIOccupyDriversInUsedVehicles].

(ii) **Move an already spawned infantry group into cargo seats**
- Instant (server): for each agent character `c`: `SCR_CompartmentAccessComponent ca = SCR_CompartmentAccessComponent.Cast(c.FindComponent(SCR_CompartmentAccessComponent)); ca.MoveInVehicle(veh, ECompartmentType.CARGO, GetGame().GetWorld().IsGameTimePaused());` returns false if no free seat (order PILOT, TURRET, CARGO as the editor does: SCR_EditableCharacterComponent.OnParentEntityChanged L409-480). Slot locked for one frame, so call per character in a loop is fine. Lower-level: `CompartmentAccessComponent.GetInVehicle(veh, slot, true /*forceTeleport*/, -1, ECloseDoorAfterActions.INVALID, false)` with a slot from `FindFreeCompartment(veh, ECompartmentType.CARGO)`.
- Walking (AI behaviour, no teleport): add a `SCR_BoardingEntityWaypoint` (prefab AIWaypoint_GetIn) to the infantry group with `wp.SetEntity(veh)` (entity, not name) and `wp.SetAllowance(driver, gunner, cargo)`; pattern verbatim in `SCR_ContextGetIngroupCommand.CreateBoardingEntityWP` (spawn wp prefab via `SpawnEntityPrefabLocal`, `SetCompletionRadius`, `slaveGroup.AddWaypoint(wp)`; `SetPriorityLevel(SCR_AIActionBase.PRIORITY_LEVEL_PLAYER)` to force). Completes via `SCR_AIGetInActivity` -> `CompleteWaypoint`. `AIWaypoint_GetInNearest` (SCR_BoardingTimedWaypoint, radius 20, no entity) picks the nearest vehicle itself.
- Pair with `group.GetGroupUtilityComponent().AddUsableVehicle(SCR_AIVehicleUsageComponent.FindOnNearestParent(veh, veh))` (public; happens automatically on compartment enter).
- Dilemma [hyp]: crew in group A drives, infantry in group B rides. The vehicle follows group A's waypoints; B only needs GetIn at start and GetOut at end. B's own movement waypoints must not be active while embarked. Single-group option: one `SpawnDefaultOccupants({PILOT,TURRET,CARGO})` call gives one group (default mixed squad) that moves with vehicle and dismounts as one: simplest, matches QRF MOUNTED_INFANTRY.

(iii) **Dismount**
- Group order (AI walks out): add waypoint `AIWaypoint_GetOut` (SCR_BoardingWaypoint, radius 9, allowance all) at the drop point, or `AIWaypoint_GetOutInstant` (radius 10). With `SetAllowance(false,false,true)` only cargo leaves, crew stays (`m_BoardingParameters` driver/gunner/cargo flags, [SRC SCR_BoardingWaypoint.c]). `SCR_AIGetOutActivity` completes the waypoint and removes the vehicle from usable list.
- Hard (server, per character): `ca.GetOutVehicle(EGetOutType.TELEPORT, -1, ECloseDoorAfterActions.INVALID, false)` (as `SCR_DisembarkContextAction`) or `ca.GetOutVehicle_NoDoor(transform, false, false)`; `KickFromVehicle(doorIdx)`. Also `character.RemoveUsableVehicle(...)` in the editor path; for AI group call `utility.RemoveUsableVehicle(usage)`.

## 3. Waypoints (Prefabs/AI/Waypoints/)

| Use | Prefab | GUID | Script class | Notes |
|---|---|---|---|---|
| Move | AIWaypoint_Move.et | {750A8D1695BD6998} | SCR_AIWaypoint | radius 5; works for vehicle groups (driver drives, road pathing) |
| Forced move | AIWaypoint_ForcedMove.et | {06E1B6EBD480C6E0} | SCR_AIWaypoint | priority 2000: ignores combat; good for convoy legs |
| Get in (specific vehicle) | AIWaypoint_GetIn.et | {712F4795CF8B91C7} | SCR_BoardingEntityWaypoint | radius 9; needs `SetEntity(veh)`; allowance flags |
| Get in selected | AIWaypoint_GetInSelected.et | {8AD8C82346156494} | SCR_BoardingEntityWaypoint | radius 5 |
| Get in nearest | AIWaypoint_GetInNearest.et | {B049D4C74FBC0C4D} | SCR_BoardingTimedWaypoint | radius 20, nearest usable vehicle, timed |
| Get out | AIWaypoint_GetOut.et | {C40316EE26846CAB} | SCR_BoardingWaypoint | radius 9 |
| Get out instant | AIWaypoint_GetOutInstant.et | {E5002E8CD9D1F4AF} | SCR_BoardingWaypoint | radius 10, no params |
| Follow entity | AIWaypoint_Follow.et | {A0509D3C4DD4475E} | SCR_EntityWaypoint | `SetEntity()`; could make infantry follow their truck |
| Load/Unload supplies | AIWaypoint_LoadSupplies {0F38A8CA489A1B3D}, AIWaypoint_UnloadSupplies {409DE49C64865E30} | | SCR_LoadSuppliesWaypoint / SCR_UnloadSuppliesWaypoint | priority 2000; vanilla truck supply logistics (SuppliesTransportSystem/SCR_TransportUnitComponent) |
| Base/others | Base {49CED34BBCD060F0}, Defend {93291E72AC23930F}, SearchAndDestroy {B3E7B8DC2BAB8ACC}, Patrol {22A875E30470BD4F}, Wait {531EC45063C1F57B}, Suppress {ED8277F35B46B4AA}, ArtillerySupport {C524700A27CFECDD} | | | |

- `SCR_TransportWaypoint` (class exists, `m_WaitForBoardingTime`, `m_sDriverCharacterName`/`SetDriver`) has NO prefab in Prefabs/AI/Waypoints: it is used by scenario framework (SCR_ScenarioFrameworkWaypoint, SCR_ResupplyTaskSolver, SCR_TransportUnitComponent). Skip it; use Boarding + Move.
- Vehicle usage list: `SCR_AIGroupUtilityComponent.AddUsableVehicle/RemoveUsableVehicle/IsUsableVehicle`, manager `m_VehicleMgr` (`SCR_AIGroupVehicleManager`: TryAddVehicle, FindVehicle, GetAllVehicleEntities, GetVehiclesCount). Not `SCR_AIGroup.AddUsableVehicle` (that name exists only on SCR_EditableCharacterComponent). Auto-added when any group agent enters a compartment; auto-removed on vehicle delete/destroy.
- `SCR_AIVehicleUsageComponent` types: GROUND_VEHICLE_WHEELED etc.; `IsVehicleTypeValid()` must be true or boarding errors out.

## 4. Runtime group spawning: sequence
```
Resource r = Resource.Load("{DDF3799FA1387848}Prefabs/Groups/BLUFOR/Group_US_RifleSquad.et");
EntitySpawnParams p = new EntitySpawnParams(); p.TransformMode = ETransformMode.WORLD; p.Transform[3] = pos; // Y = surface
SCR_AIGroup g = SCR_AIGroup.Cast(GetGame().SpawnEntityPrefab(r, GetGame().GetWorld(), p));
// server only. Members spawn over the next ticks via SCR_AIWorld queue and are auto-activated.
g.AddWaypoint(wp);   // can be added immediately
```
- Vanilla prior art for runtime group spawn: `SCR_AmbientPatrolSpawnPointComponent`, `SCR_ScenarioFrameworkQRFSlotAI`, `SCR_GroupsManagerComponent.CreateNewPlayableGroup` (also `DeactivateAI` then `RegisterGroup`). Prefabs under `AmbientPatrols/` named `*_NotSpawned` are the "m_bSpawnImmediately 0" variants for those spawners: do NOT use them (members only appear on `SpawnUnits()`/`RequestSpawn`).
- Players' playable groups: `Prefabs/Groups/PlayableGroup.et`, `Campaign_PlayableGroup.et` (not for AI reinforcements).
- AI budget: groups count against `aiLimit` / per-faction limit; importance LOW..CRITICAL (see feasibility-research.md section 8). Spawn queue means large waves trickle in over many ticks.

### Infantry group prefabs (slots = m_aUnitPrefabSlots, leader first) [SRC]
| Type | US ({GUID}Prefabs/Groups/BLUFOR/...) | USSR (.../OPFOR/...) | FIA (.../INDFOR/...) |
|---|---|---|---|
| Rifle squad | Group_US_RifleSquad {DDF3799FA1387848}: SL TL TL AR AR GL GL LAT LAT (9) | Group_USSR_RifleSquad {E552DABF3636C2AD}: SL AR AT AAT SR LAT (6 as read; the file inherits/overrides, verify with prefab inspect) | Group_FIA_RifleSquad {CE41AF625D05D0F0}: SL MG AMG AT Rifleman Rifleman LAT (7) |
| Fire team | Group_US_FireTeam {84E5BBAB25EA23E5}: TL AR GL LAT (4) | Group_USSR_FireGroup {30ED11AA4F0D41E5}: SL AR AT AAT (4) | Group_FIA_FireTeam {5BEA04939D148B1D}: SL AT Rifleman Rifleman LAT |
| Light fire team | Group_US_LightFireTeam {FCF7F5DC4F83955C}: TL + 3 Rifleman | Group_USSR_LightFireTeam {657590C1EC9E27D3}: SR + 3 Rifleman | Group_FIA_LightFireTeam {1BB20A4B3A53D0F5} |
| MG team | Group_US_MachineGunTeam {958039B857396B7B}: MG AMG | Group_USSR_MachineGunTeam {A2F75E45C66B1C0A}: MG AMG | Group_FIA_MachineGunTeam {22F33D3EC8F281AB} |
| AT team | Group_US_Team_LAT {FAEA8B9E1252F56E}: TL AR LAT LAT | Group_USSR_Team_AT {96BAB56E6558788E}: SR AT AT AAT (RPG-7) ; Group_USSR_Team_LAT {2E59ECDA9ED0B993} | Group_FIA_Team_AT {2CC26054775FBA2C}: Rifleman AT AT AAT ; Team_LAT {9328FE7DD0019E60} |
| Grenadier/Suppress | Group_US_Team_GL {DE747BC9217D383C} | Group_USSR_Team_GL {43C7A28EEB660FF8} | |
| Recon | Group_US_ReconTeam {F65B7BB712F46FEE}: Scout Scout_RTO | OPFOR/KLMK/Group_USSR_ReconTeam {06F0C9675883F18A} | Group_FIA_ReconTeam {2E9C920C3ACA2C6F} |
| Sniper/marksman | Group_US_SniperTeam.et (Spotter Sniper; GUID not referenced anywhere, load by path or read GUID in Workbench) | none (USSR has no sniper group; Sharpshooter is in PlatoonHQ) | Group_FIA_SharpshooterTeam {6307F42403E9B8A4}: Scout Sharpshooter |
| Sentry (2 riflemen) | Group_US_SentryTeam {3BF36BDEEB33AEC9} | Group_USSR_SentryTeam {CB58D90EA14430AD} | Group_FIA_SentryTeam {6E725D44CA973C24} |
| Platoon HQ | Group_US_PlatoonHQ {B7AB5D3F8A7ADAE4}: PL Sergeant RTO Medic | Group_USSR_PlatoonHQ {0D10CCEEC7B3EC34} | Group_FIA_PlatoonHQ {EE92725E9B949C3D} |
| Transport drivers | Group_US_Transport {727C134094032B1F} | Group_USSR_Transport {29DFCC25F263026B} | |
Base prefabs: Group_Base {000CD338713F2B5A}, Group_US_Base {EACD97CF4A702FAE}, Group_USSR_Base {8DE0C0830FE0C33D}, Group_FIA_Base {242BC3C6BCE96EA5}, Slave_Group {04D3B38E23F51754}.
Other groups exist (Engineer, Medical, Ammo, Sapper in each faction folder, KLMK camo variants for USSR, GreenBerets for US): `asset_search Group_`.
NB: USSR rifle squad slot list in the file is 6 entries as read; verify with `prefab inspect` if exact size matters.

Characters (for custom groups / `SpawnCharacterInCompartment`): US_SL {E45F1E163F5CA080}, US_TL {E398E44759DA1A43}, US_AR {5B1996C05B1E51A4}, US_GL {84029128FA6F6BB9}, US_LAT {27BF1FF235DD6036}, US_MG {1623EA3AEFACA0E4}, US_Rifleman {26A9756790131354}, US_Crew {E1CB513B8B9B08F4}, US_Sniper {0F6689B491641155}, US_Scout {371FD0F920B600DD}; USSR_SL {5436629450D8387A}, USSR_SR {333DA6244C7DA34C}, USSR_AR {23ADBBC31B6A3DC6}, USSR_AT {1C78331E156A3D65}, USSR_LAT {BF643BE4ADBDFDD3}, USSR_MG {96C784C502AC37DA}, USSR_Rifleman {DCB41B3746FDD1BE}, USSR_Crew {9FFEF10757E742EB}. All under `Prefabs/Characters/Factions/{BLUFOR/US_Army|OPFOR/USSR_Army}/Character_<F>_<role>.et`.

## 5. Supplies on trucks [SRC]
- Truck cargo part prefabs (`M923A1/VehParts/M923A1_cargo.et` {5A7E3ECE4599214B}, `Ural4320/VehParts/Ural4320_cargo.et` {58B3FB7F102A8B1B}) carry `SCR_ResourceComponent` with a `SCR_ResourceContainer` (`m_eStorageType CARGO_VEHICLE`, `m_eResourceRights FACTION`, `m_fResourceValueCurrent 0`) plus encapsulator (Queue_Encapsulator_Vehicle.conf). Cars/APCs (Humr, UAZ469, BTR70, BRDM2, LAV25, M151A2) have a CARGO_VEHICLE container with Max 0 (no capacity). Dedicated service boxes (`Core/Vehicle_EngineerBox_Base.et`: Max 550, repair/ammo/engineer boxes) are other containers.
- API [WIKI]: `SCR_ResourceContainer.GetResourceValue/GetMaxResourceValue/GetStorageType` (+ setters not listed in the compact page: look at `SCR_ResourceComponent.GetContainer`/`GetComponent`). Vanilla logistics: `SCR_LoadSuppliesWaypoint`, `SCR_UnloadSuppliesWaypoint`, `SCR_SuppliesTransferWaypoint`, `SuppliesTransportSystem/SCR_TransportUnitComponent`, `SCR_ResupplyTaskSolver`. Prefabs AIWaypoint_LoadSupplies / UnloadSupplies exist. A supply truck is therefore feasible with vanilla parts; the details of filling the container at runtime are [hyp].

## 6. Recommended code plan and open risks
1. Infantry reinforcement: `SpawnEntityPrefab(Group_*)` at the base; add waypoints right away; log `GetAgentsCount()` to confirm the queue spawned everyone. Not `_NotSpawned` prefabs.
2. Mechanized v1 (simplest, vanilla-proven by QRF): spawn BTR70/LAV25/M1025 armed, `SpawnDefaultOccupants({PILOT,TURRET,CARGO})`, take the resulting group in the callback, treat vehicle+group as ONE unit: Move/ForcedMove waypoints, then `AIWaypoint_GetOut` at the objective, then Defend/SearchAndDestroy. Truck convoy: truck defaults give only driver (+1-2), so v2: spawn infantry group, `MoveInVehicle(CARGO)` loop (or GetIn waypoint), driver group does Move.
3. Risks: (a) 1.9 vehicle rewrite; (b) AI vehicle pathing/off-road/bridges/stuck: need stuck detector, `SCR_AIRemoveStuckVehicle` exists as a vanilla behaviour; (c) after GetOut the driver stays seated (ok) but the APC remains in group's usable list until `GetOutActivity` completes; (d) SpawnDefaultOccupants only server, one char/frame, same-type re-spawn refused; callback is the only place to get the group; (e) group of occupants has no formation/leader semantics tested; (f) `MoveInVehicle` is RPC'd to the character's owner and teleports, needs RplComponent on the slot entity; for AI characters owned by the server this is immediate; (g) dedicated server AI cap/budget; (h) no tracked vehicles in vanilla; (i) unverified: exact public getter for group-init event, Humr turret seat details, UAZ469 covered variants' extra seats, the Group_US_SniperTeam GUID.
