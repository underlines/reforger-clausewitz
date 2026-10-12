# Vehicle AI (mechanized units): web research, 2026-10-12

Labels: [verified] = BI primary source (official news, BI script repo, BI API docs), [forum] = Steam/Reddit/secondary guide, [hyp] = my inference, [gap] = could not find.
Researched with camoufox browser. Code search on github.com requires login, so real-world mod patterns (BattleGen, AI Kommandanten, DCO_AI, Raven source) were NOT retrievable [gap]; the patterns below come from BI's own script repo.

## 0. Version status
- Stable (Main) is 1.8.0.13 (3 Sep 2026). 1.9 is still EXPERIMENTAL as of the last news (1.9.0.5 on 15 Sep, 1.9.0.9 on 18 Sep, 1.9.0.10 on 24 Sep; Dev Report #24 on 22 Sep; modding changes post 24 Sep). No stable 1.9 announcement found in news list [verified, news page]. Experimental cannot join Main servers. Confirm in-game before relying on 1.9 behaviour.
- Sources: https://reforger.armaplatform.com/news , https://reforger.armaplatform.com/news/modding-update-sept-24-2026 , https://reforger.armaplatform.com/news/dev-report-24 , https://reforger.armaplatform.com/news/experimental-september-15-2026 , https://steamdb.info/patchnotes/25426149/

## 1. AI driving state
| Fact | Label | Source |
|---|---|---|
| AI driving exists since 1.2 (June 2024): road network graph + vehicle navmesh ("BTRlike" project). Roads must be typed Road Main/Minor/Trail (SCR_AIWorld.ent). | [verified] | https://reforger.armaplatform.com/news/modding-update-june-7-2024 |
| Bridges: only drivable if the map has `RoadNetworkBridgeComponent` on the bridge. Gates: only if `NavmeshCustomLinkComponent` (Door) is set up; then AI dismounts?/opens/passes/closes. Maps without this = AI cannot cross. | [verified] (setup), [forum] (dismount-at-gate) | same + https://gamers.wiki/en/games/arma-reforger/guides/arma-reforger-game-master-ai-routes-waypoints-and-resets |
| Destructible props (stone posts etc) are marked on the vehicle navmesh; path flags per vehicle on `AIPathfindingComponent`, modifiable with BT nodes AITaskSetPathfindingFilters/Reset. | [verified] | June 2024 post |
| Partial road blockage is treated as full disconnection (detour / stop). Combat can interrupt a route. | [forum] (secondary guide, Stable 1.8.0.10) | gamers.wiki GM routes |
| Off-road: AI "prioritise roads rather than dirt tracks" ; mixed results, accuracy poor. A GM convoy test: 2/3 arrived, 1 into ravine (crew bailed and walked), 1 circled; vehicles "bumper-car" each other after missed turn until GM moved one. (2024 anecdote) | [forum] | https://steamcommunity.com/app/1874880/discussions/0/4853281214106814190/ , https://steamcommunity.com/app/1874880/discussions/0/594022657516656410/ |
| Map-dependent: user reports AI vehicles ignoring move waypoints on GM Everon but working on Arland/Takistan (May 2025). Check that the target world has the AIWorld/road-network set up. | [forum] | steam thread 594022657516656410 |
| Mounted weapons: AI crew gets auto-assigned to the vehicle turret; if gunner/driver incapacitated, another crew/cargo member takes the seat. | [verified] in script (SCR_AIVehicleCombatActivity), [forum] summary | script below |
| Vehicle with no/unconscious driver now self-steers (1.9.0.5), so abandoned vehicles roll/drift. | [verified] | Dev Report 24, 1.9.0.5 changelog |
| 1.9.0.5: "AI Logistic drivers are now forced to have simulation LOD and not get dormant" (vanilla has AI logistics drivers = a supported truck-driving use case). | [verified] | experimental-september-15-2026 |
| 1.9.0.10: "AI Perception was not being blocked by some of the objects". 1.9 "AI visibility through vegetation received further fixes". | [verified] | steamdb 25426149 , dev-report-24 |
| Trucks were very slow/unable on hills in 1.7-1.8 (powertrain bug; M923A1/Ural engine zones were widened as stop-gap). 1.9 rewrites powertrain, inter-axle differentials, Ackermann: fixes this, but retunes all wheeled vehicles. Expect AI drive speed/stuck behaviour to differ between 1.8 stable and 1.9. | [verified] | dev-report-24, modding-update-sept-24-2026 |

## 2. Group/vehicle mechanics from BI script repo (https://github.com/BohemiaInteractive/Arma-Reforger-Script-Diff, raw files read 2026-10-12; repo may be slightly ahead of stable)
[verified] unless noted.

### Vehicle prerequisites
- `SCR_AIVehicleUsageComponent` must exist on the vehicle ROOT (or static tripod). Without it "other AI systems depend on this, so some AI functionality might not work". Check vanilla vehicle prefabs have it (they do) and that custom spawned vehicles use vanilla prefabs. API: `CanBePiloted()`, `GetVehicleType()`, `IsOccupiedByGroup(SCR_AIGroup)`, `IsOccupiedBy(IEntity)`, `FindOnNearestParent(ent, out owner)`.
- Vehicle is removed from group's usable list when destroyed or damage state changes (`OnVehicleDamageStateChanged -> RemoveUsableVehicle`), and `CanMove()` uses `SCR_AIVehicleUsability.VehicleCanMove` / on-fire check.

### How a vehicle becomes "the group's vehicle"
1. Waypoint route: a `SCR_BoardingWaypoint` (prefab "Get In") issues `SCR_AIGetInActivity`; on select it calls `m_Utility.AddUsableVehicle(vehicleUsageComp)`; on completion it completes the waypoint, on failure it removes the vehicle again and completes the waypoint anyway. `SCR_BoardingEntityWaypoint` = same but linked to an entity: `SetEntity(IEntity)` / `SetEntityName(string)`. Boarding params (`SetAllowance(driver, gunner, cargo)` on `SCR_BoardingWaypoint`) choose which seat types may be filled. (SCR_BoardingWaypoint.c, SCR_BoardingEntityWaypoint.c, SCR_AIGetInActivity.c)
2. Implicit: when any group agent enters a compartment (`OnAgentCompartmentEntered` in `SCR_AIGroupUtilityComponent`) the vehicle is added via `AddUsableVehicle`, a vehicle "subgroup" is created (driver = subgroup leader), and fireteam manager assigns crew/cargo fireteams. So: if you seat the AI with `CompartmentAccessComponent.GetInVehicle(...)` (below) the group learns about the vehicle by itself [verified code path, runtime behaviour untested here].
3. Static: `SCR_AIGroup.AddVehiclesStatic(array<string> names)` (public) looks up named entities (`FindEntityByName`) and does `m_GroupUtilityComponent.m_VehicleMgr.TryAddVehicle(...)`. Also attribute `m_aStaticVehicles` ("Group Vehicles" category). Requires entity names, so you must `SetName` on the spawned vehicle first (hyp: naming works at runtime, untested).
4. Group-side compartment reservation: `SCR_AIGroup.AllocateCompartment(slot)/ReleaseCompartment/ReleaseCompartments/GetAllocatedCompartments` (sets `SetCompartmentAccessible(false)` so two groups don't claim one seat; released when Get-In activity is deselected). Stale reservations are a known failure class [forum, gamers.wiki squad guide: "stale vehicle reservations, partial exits"].

### Other vehicle waypoints in vanilla
`SCR_TransportWaypoint` (attributes: `m_WaitForBoardingTime` = how long infantry wait for the vehicle before walking, `m_sDriverCharacterName` = named driver character the infantry wait for; `GetVehicle()` resolves the driver's pilot compartment owner), `SCR_LoadSuppliesWaypoint`, `SCR_UnloadSuppliesWaypoint`, `SCR_SuppliesTransferWaypoint`, `SCR_BoardingTimedWaypoint`, `SCR_EntityWaypoint`, `SCR_SearchAndDestroyWaypoint`, `SCR_DefendWaypoint`, `SCR_SuppressWaypoint`. There is no obvious "GetOut" class file in Group/; "Get Out" in editor is likely a prefab of a generic/timed waypoint handled by the dismount message path [hyp]. Unload by Transport semantic from Arma 3 does not exist as such.

### Engine API (BI Script API docs, https://community.bistudio.com/wikidata/external-data/arma-reforger/ArmaReforgerScriptAPIPublic/)
- `CompartmentAccessComponent.GetInVehicle(IEntity vehicle, BaseCompartmentSlot compartment, bool forceTeleport, int doorInfoIndex, ECloseDoorAfterActions closeDoor, bool performWhenPaused)` -> bool. forceTeleport = instant seating (ignores door index).
- `GetOutVehicle(EGetOutType type, int doorInfoIndex, ECloseDoorAfterActions, bool performWhenPaused, bool isExitTeleport=false)`; `GetOutVehicle_NoDoor(vector transform[4], bool sendIntoRagdoll, bool performWhenPaused, bool isExitTeleport)` (teleports out); `CanGetInVehicle(IEntity)`, `IsGettingIn()`, `IsGettingOut()`, `CanGetOutVehicle()`, `FindSuitableTeleportLocation(vector TR[4])`, `GetVehicleIn(IEntity char)`, `IsInCompartment()`, `GetCompartment()`.
- `BaseCompartmentManagerComponent.GetCompartments(out array<BaseCompartmentSlot>)`, `FindCompartmentByName`, `FindCompartment(id)`; slot types: `PilotCompartmentSlot` (driver), `TurretCompartmentSlot` (gunner/commander), cargo slots (`ECompartmentType.CARGO`); `slot.GetOccupant()`, `slot.IsCompartmentAccessible()/SetCompartmentAccessible(bool)`.
- `SCR_AIGroup.AddWaypoint(AIWaypoint)`, `RemoveWaypointFromGroup`, `CompleteWaypoint`, `SpawnUnits()` (when `m_bSpawnImmediately` false), `GetAgents(out array<AIAgent>)`, `AddAIEntityToGroup` / `RemoveAIEntityFromGroup`.
- Not found as callable: a ready-made "SCR_AIGroup.OrderGetInVehicle(vehicle)". The group-level request is a waypoint (SCR_BoardingEntityWaypoint) or the message `SCR_AIMessageHandling.SendGetInMessage(agent, vehicleEntity, EAICompartmentType.Pilot|Turret|Cargo, ...)` (used internally; likely usable from mod code but takes an activity/comms arg: [hyp]).

### Combat behaviour (SCR_AIVehicleCombatActivity.c)
- Active when group owns >=1 non-static vehicle, not HOLD_FIRE, not slave group, and the most dangerous target cluster was seen <20 s ago and is within 550 m (if it endangers) or 200 m (else).
- On start: vehicle WITH weapon (turret slot): crew stays (gunner/driver), if driver is wounded/unconscious a cargo member is sent to the pilot seat; CARGO fireteams are sent a Dismount message (so passengers DISMOUNT when contact is dangerous). Vehicle WITHOUT weapon (truck, unarmed jeep): crew AND cargo are dismounted. "GunnerRetreatLogic" is explicitly disabled in code ("until retreat mechanic is available").
- Consequence [verified code + hyp]: trucks dismount everybody at first contact (<=550 m); IFV/APC stay as fire support and unload cargo; no scripted retreat/withdraw. Nothing in the script keeps infantry near the IFV afterwards; dismounted soldiers follow the group's normal waypoint logic.

## 3. Chains people use
- Player/GM chain [forum + secondary guide]: Move (stage) -> Get In (linked to vehicle, passenger/driver allowance) -> Move (destination) -> Get Out -> (Cycle). Test each stage alone; Get In fails if vehicle not functional / no suitable seats / out of radius / timeout. Reddit Arma 3 advice ("Transport unload for APC + Get Out for squad, sync") does NOT map to Reforger [forum, https://www.reddit.com/r/arma/comments/12xakyc/].
- AI Kommandanten / BattleGen / DCO_AI / Conflict HQ PvPvE: nothing retrievable [gap].
- Raven AI Commander (logistics): physical convoy via crew boarding then an "outbound waypoint" state; "convoy start failures" get a retry delay; no documented stuck/dead-driver recovery; it binds to a fixed pre-selected vehicle (M1025 US / BTR-70 USSR) rather than scanning. [forum, field notes https://github.com/MarcoBetti1/reforger-agent-harness/blob/main/docs/raven-field-notes.md ; source not reviewed since packed]
- Advanced AI Command (APL, Workshop 69A404653EE3F3C4, built for 1.8): offers "order squads into a chosen vehicle or dismount" and "Attach/Detach: link squads into foot-follow groups or vehicle convoys". Source not read; APL so it can be decompiled/read if wanted [gap].

## 4. Convoys
- No evidence of native formation/spacing for multi-vehicle columns; the secondary guide says primary evidence "does not establish deterministic multi-vehicle formation or spacing" [forum]. Eight jeeps across Everon "did not go very well" (video, Mar 2025) [forum]. Known: vehicles bump each other after a missed turn; recover only if one is moved away [forum, 2024].
- 1.9 adds no convoy feature in the notes I read [verified for the pages read: 1.9.0.5, 1.9.0.10, dev report 24, modding post].
- [hyp] robust approach: independent vehicles each with own group, staggered departure (time offset), own waypoints, no inter-vehicle following dependency; or leader-only waypoints + followers re-issued Move to leader's road position.

## 5. AI vehicles in combat vs infantry/vehicles; combined arms
- Armed vehicles keep crew inside and engage via turret; cargo dismounts. [verified code] Effectiveness evidence: only the "AI can use mounted weapons / reassign gunner" statement [forum]. No tank guidance found; vanilla vehicle roster: BTR-70, M113A1 family, LAV-25 (1.9.0.5 added 3rd optic + smoke launchers), BRDM-2, M1025/UAZ/Ural/M923 trucks. Tanks: [gap] I found no source for vanilla tanks (confirm locally with `faction_list_units` / prefab search). 
- No official "keep infantry near the IFV" mechanic; do it in our own logic (e.g. dismount waypoint within ~100-200 m of objective, vehicle holds Defend/Move to overwatch point, then infantry gets objective waypoint).

## 6. 1.9 / experimental items relevant to our mod
| Item | Impact |
|---|---|
| AI spawning rework (modding post): group entity is permanent; members spawn via a world queue (max 4 members per 0.5 s globally); groups can be DORMANT (members despawned, counts kept); `group.RequestSpawn(int slotsWanted=-1, float observerRange=0)`, `group.DespawnMembers()` (SCR_AIGroup), `IsDormant()`, `GetDormantAliveCount()`, `GetDormantDeadCount()`, `SetLifecyclePolicy(SCR_EAIGroupLifecyclePolicy.ProximityDriven, 600, 800)` (default Manual), importance tiers CRITICAL/HIGH/NORMAL/LOW, eviction of groups >800 m from players, global limit 128 (`-activeAILimit`, `AIWorld.GetLimitOfActiveAIs/GetCurrentNumOfActiveAIs/CanActivateGroup`). | A vehicle crew spawned in 1.9 will NOT be complete in one frame. Wait until all members exist before seating / ordering. Members who are AI-activated (driver) keep the group active. A dormant group with a vehicle: members despawn, vehicle stays (hyp). Set Manual policy (default) for HQ-controlled groups so they are never despawned behind our back; note HIGH/CRITICAL importance if you need them to survive limit pressure. |
| 1.9.0.5: vehicle self-steering without driver; AI logistic drivers forced LOD-active | Dead-driver vehicles drift; destroyed/abandoned vehicle will roll. |
| Wheeled vehicle overhaul (powertrain, IAD, Ackermann, tire model) | Trucks climb hills now; AI speed/stuck tuned for old physics may change. `SCR_WheelSlotInfo.GetWheelIndex()` added. |
| 1.9.0.10 AI perception fix; vegetation visibility fixes | Minor. |
| Not found: any 1.9/1.10 note about AI driving, waypoint, convoy changes. No 1.10 info exists yet. | |

## 7. Known bugs / workarounds
- AI "will not board/dismount": check vehicle functional, accessible compartments, doors, blockers, path to vehicle; stale reservations and partial exits are historic failure classes [forum]. In code: group `ReleaseCompartments()` clears reserved seats [verified].
- Stuck vehicle: no official stuck detector found. GM workaround = move vehicle [forum]. [hyp] our own: sample position every N s; if displacement < X m while a Move waypoint is active -> (1) re-issue waypoint (remove/add), (2) after 2nd failure dismount group and send it on foot, (3) never teleport unless nobody is within view.
- Dedicated server: Raven changelog mentions "waypoint fixes for dedicated servers" [forum] so test waypoint creation on server (authority) only; AI/waypoints are server-authoritative, a client cannot create them.
- Roads/bridges absent in a world = AI will not path across: check world has road network / bridge components, else avoid mechanized routes there [verified setup requirement].

## 8. Recommended robust design (summary of recommendations)
RELY ON: vanilla prefabs (they carry SCR_AIVehicleUsageComponent); crew seated by script with `CompartmentAccessComponent.GetInVehicle(vehicle, slot, forceTeleport=true,...)` right after spawn (no walk-to-vehicle dependency), driver = `PilotCompartmentSlot`, gunner = `TurretCompartmentSlot`, rest cargo; group learns vehicle through OnAgentCompartmentEntered, optionally call `AddVehiclesStatic` with a named entity or use `SCR_BoardingEntityWaypoint.SetEntity(vehicle)` for walking infantry; plain Move waypoints on road-adjacent points for vehicle; dismount via `GetOutVehicle` or let combat activity do it; own watchdog for stuck vehicles; mounts/dismounts as a state machine in HQ code with timeouts (Get In timeout -> fall back to foot).
AVOID: relying on multi-vehicle convoy/formation; off-road or bridge routes on maps without bridge/gate components; long routes without intermediate road waypoints; trucks as fire support (they dismount all at contact); depending on 1.9-only dormant/queue semantics until 1.9 is stable; waypoint `SCR_TransportWaypoint` waiting by driver name (name-based, brittle); expecting retreat behaviour (disabled in vanilla); AddPlayerToGroup-style hacks (see project rule).
