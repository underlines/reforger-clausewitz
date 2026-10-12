# M2 "Real war" + mechanized: design (decided 2026-10-12)

Source of truth for the M2 implementation. Research behind it: `research/missions-objectives-endgame.md`,
`research/vehicles-and-spawning.md`, `research/vehicle-ai-web.md`, `research/commanding-and-tasks.md`.
Target: Reforger stable 1.8 (1.9 experimental: member spawn queue = members arrive over several ticks; always wait for them).

## 0. Ground rules for implementers
- Code only. **No Workbench, no wb_* tools, no Play/e2e tests**: one end-to-end test happens after all phases. Static checks
  (`script_lint`, `script_analyze`) are fine. Because nothing is run before the end, write defensively: null checks,
  bounded retries, and a `Print("CLW: <area> ...")` log line for every state change so one test run can diagnose everything.
- Server authority: all logic `Replication.IsServer()`-guarded; clients only display (RPC pattern: `CLW_DebugPlayerController.c`).
- Enforce gotchas (skill `reforger-script-dev`): `string.Format` truncates at ~8 KB (concatenate JSON), max 9 Format args,
  no variable named like a type, `Managed` already has `ToString()`, callbacks must be held in a `ref`.
- Player squads: always `CLW_PlayerSquad.Join` (never AddPlayerToGroup/AssignGroupID).
- Content (factions, prefabs, coordinates, economy numbers) lives in `.conf` data (section 0a); pure code tunables
  (timers, thresholds) in `CLW_WarConfig` statics. Existing hard-wiring (`CLW_SquadRegistry.FACTION_US/USSR`,
  `IsClwFaction`, `CLW_ContactMarkers.GetIdentity`, patrol points and preferred squad names in `CLW_GameMode`,
  prefab-name parsing in `CLW_SquadInfo.TypeName`, the commanders' waypoint prefab constants) is moved to data as part of
  the phase that owns the file. Vanilla waypoint prefabs (Move/Defend/GetIn/...) are engine-level, not faction content:
  they may stay as defaults but go through one `CLW_WarConfig` place.
- Prefer the online Script API (https://community.bistudio.com/wikidata/external-data/arma-reforger/ArmaReforgerScriptAPIPublic/index.html)
  and `api_search` / `game_read` of vanilla scripts to confirm every engine call you are not sure of. Mark unverifiable calls with `// [verify]`.
- Commit style: Conventional Commits; add one CHANGELOG `[Unreleased]` line per user-visible feature.

## 0a. Content-agnostic: no vanilla hard-wiring, ever (binding rule)
The mod must run with any faction/vehicle pack (RHS, WCS, ...), including more vehicle types than vanilla. So:
- **No prefab, faction key, vehicle name, seat count or map coordinate in script code.** Code holds mechanisms only.
  All content is data in `.conf` config resources (`[BaseContainerProps(configRoot: true)]` classes, loaded with
  `BaseContainerTools.LoadContainer` + `CreateInstanceFromContainer` or `SCR_ConfigHelper`; create files with the MCP
  `config_create` tool so `.meta` files are right). Vanilla data ships as *one* set of these configs, nothing special.
- **Scenario config** (`Configs/CLW/Scenarios/<World>.conf`, class `CLW_ScenarioConfig`, chosen by a `ResourceName`
  attribute on the CLW_GameMode prefab, overridable by any addon/prefab variant): the two faction keys, per faction the base
  position + map identity (BLUFOR/OPFOR/INDFOR) + profile, objectives (id, name, pos, radius, value, start owner), economy
  numbers (tickets, supplies, income, bleed, caps, time limit). Section 1-3 values below are the Arland *data*.
- **Faction profile** (`Configs/CLW/Factions/<Name>.conf`, class `CLW_FactionProfile`): list of `CLW_ForceEntry`
  {type id, display name, kind, cost, group prefab, vehicle prefab (optional), cargo group prefab (optional), weight in the buy mix}.
  Vanilla ships `US.conf`, `USSR.conf`, `FIA.conf`. A compat addon (e.g. "Clausewitz - RHS") ships its own profiles and a
  scenario that references them: zero code changes.
- **Auto-discovery fallback** when a faction has no profile: build entries from the faction's `SCR_EntityCatalog`
  (`SCR_Faction.GetFactionEntityCatalogOfType(EEntityCatalogType.GROUP / VEHICLE)`) and a **role classifier** that only
  uses generic properties: editable-entity labels (vehicle/trait labels), compartments (count of PILOT/TURRET/CARGO slots
  from the compartment manager), armed (turret with a weapon), armor if a label says so; groups by labels or member count.
  Cost = formula from kind + size (configurable). Log the discovered catalog once. Mark untested label names `// [verify]`.
- **Runtime facts are read, not assumed:** cargo capacity = count CARGO compartments of the spawned vehicle; the embarked
  group is chosen/trimmed to fit; crew = whatever `SpawnDefaultOccupants({PILOT, TURRET})` produces; "is vehicle" =
  has a `BaseCompartmentManagerComponent`/`Vehicle` class, never a prefab name; faction of anything = its faction
  affiliation component.
- Kinds are a closed **generic** set the commanders reason about: `infantry`, `recon`, `motorized` (unarmed transport +
  infantry), `light` (armed light vehicle), `mech` (armed vehicle with cargo + infantry), `armor` (armed vehicle without
  cargo, e.g. tanks from mod packs), `armored_recon`. Commanders and the LLM see kind + display name, never prefabs.
- Squad ids and display types come from data (entry display name), not from parsing vanilla prefab names.

## 1. The battlefield (Arland data, east-west front)

| Id | Name | x, z | radius | value |
|---|---|---|---|---|
| A | Newfield Crossroads | 2120, 2895 | 50 | 1 |
| B | Ridge Junction | 2387, 2772 | 50 | 1 |
| C | Timber Hub | 2600, 2750 | 80 | 2 (major) |
| D | North Junction | 2547, 3088 | 50 | 1 |
| E | Signal Hill | 2509, 2261 | 80 | 1 |
| F | Beauregard West | 3069, 2872 | 50 | 1 |

Main bases (not capturable in M2): **US (1560, 3090)**, **USSR (3560, 2740)** (fallback 3329, 2594 if the log warns it is
water/unsuitable: check `GetSurfaceY` vs ocean height at start and log). Start ownership: A = US, F = USSR, others neutral.
The scene-placed squads stay where they are (forward elements at the front at start; HQ orders them from there).

**Control points: our own script, no trigger prefab** (`CLW_ControlPoints`). Every 2 s count living characters per faction
inside the radius (all characters: AI from AIWorld agents + players; use `GetGame().GetWorld().QueryEntitiesBySphere` or the
agent list, alive = not DEAD). Capture rules:
- Progress -100..+100 toward a faction. Exactly one faction present -> progress moves toward it at
  `rate = 4 %/s * min(count, 4) / 2` (1 man ~50 s, 4+ men ~12 s from neutral). Contested (both present) -> frozen.
  Nobody present -> slowly decays back toward the current owner (1 %/s).
- An owned point must first be neutralised (progress through 0 -> owner none) and then captured (reaches 100 -> new owner).
- Events: `OnOwnerChanged(id, oldFaction, newFaction)`, `OnContested(id)`. Log every change; `CLW_Debug.Say` captures.
- Ownership and contested state are public (both HQs and all players know them); enemy forces at a point stay fogged.
- Map: one static marker per point visible to all (`SCR_MapMarkerManagerComponent`, see `CLW_ContactMarkers.c`), military
  symbol of the owner identity (BLUFOR/OPFOR/neutral -> INDFOR or UNKNOWN), text `"<Id> <Name>"`, re-inserted on owner change.
  Base markers the same way (text `"US HQ"`, `"USSR HQ"`).

## 2. War state: tickets, supplies, end of game (`CLW_WarState`)
- **Tickets** (war will, start 400 each). Costs: AI soldier killed -1, player killed -1, vehicle destroyed -5 (IFV/APC),
  -3 (truck/light). **Bleed** every 30 s: the side holding fewer points loses `2 * (difference in held point value)`.
- **Supplies** (purchasing power, start 400). Income every 60 s: `+20` base `+10 * held point value`. Mission rewards add more (section 6).
- **End:** tickets <= 0 -> other side wins (ENDREASON_SCORELIMIT). Holding **all** points for 5 min continuously -> win.
  Time limit 75 min -> more tickets wins, equal -> draw (ENDREASON_TIMELIMIT / ENDREASON_DRAW).
  `EndGameMode(SCR_GameModeEndData.CreateSimple(reason, -1, factionIndexOfWinner))`.
- Kill accounting: override `OnControllableDestroyed` / `OnPlayerKilled` in CLW_GameMode (check the exact vanilla signatures)
  and classify character vs vehicle and its faction (vehicle faction: `SCR_VehicleFactionAffiliationComponent` /
  `FactionAffiliationComponent`, or our own spawn bookkeeping).
- Player respawn: always allowed while tickets > 0 (`CanPlayerSpawn_S` override returns false at 0). Each death costs 1 ticket.
- `CLW_WarState` exposes getters for the situation and the dashboard, and logs a one-line summary every 60 s:
  `CLW: war US tickets=.. supplies=.. points=.. | USSR ...`.

## 2a. War modes and the setup phase (added 2026-10-12, user request)
The rules of section 2 are **one war mode**. Modes are pluggable; the host picks one and its parameters at startup.
- **Mechanism vs rules:** `CLW_WarState` owns mechanisms (ticket + supply ledgers, kill accounting, events, EndGameMode,
  spawn gate, TrySpend/Refund/AddSupplies). The active `CLW_WarMode` subclass decides (income, bleed, caps, refill,
  end conditions, time left, player spawn allowed). New mode = new subclass + `CLW_WarModeRegistry` line + config entry in
  the scenario `.conf` (`ref array<ref CLW_WarModeConfig> m_aModes`, defaults per mode).
- **Parameters:** each mode publishes `CLW_ModeParam`s (id, label, description, INT/FLOAT/BOOL/CHOICE, min/max/step,
  default, choices), rendered generically by the setup menu (and later the dashboard); values travel as compact JSON.
- **Phases:** `SETUP -> RUNNING -> ENDED`. In SETUP bases/points exist, but no economy, no purchases, no end checks.
  `StartWar(modeId, paramsJson, byPlayerId)` validates and starts. Scenario `m_iAutoStartSec` (0 = wait) auto-starts the
  default mode for headless/AI-vs-AI runs. Optional admin chat fallback `/war start [mode] [key=value ...]`.
- **Setup menu (phase P5):** the first human player who joins during SETUP is the setup owner and gets a menu: mode list
  with descriptions, then that mode's parameters (generated from `CLW_ModeParam`), Start button. Others see "waiting for
  <name> to set up the war". Server validates the owner. Implemented with vanilla UI (e.g. `SCR_ConfigurableDialogUi` with
  a content layout or a registered menu; research in P5). Client <-> server through owner/server RPCs on `SCR_PlayerController`.
- **Mode 1 (M2): Short conflict** (`short`): section 2 rules; params timeLimitMin (15-180, 60), startTickets (100-2000, 400),
  startSupplies (0-2000, 400), incomeMultiplier (0.25-3, 1), battleSize small/medium/large (AI cap per faction 30/50/80,
  vehicle cap 3/6/10), ticketBleed (on), holdAllWinMin (0-15, 5; 0 = off).
- **Vision (later modes):** see VISION section 3b (endless conflict, long conflict).
- Situation v2 `war` gets `"mode":{"id","name","params":{...}}` and `"phase"`; `timeLeftSec` -1 / tickets -1 = not used by the mode.

## 3. Bases and respawn (`CLW_Bases`)
At game start (server): spawn `SpawnPoint_US {5A2220DD0C16958B}` / `SpawnPoint_USSR {4FEA25583D0975F7}`
(`Prefabs/MP/Spawning/...` path: confirm via game_read) at the base positions (surface Y) and disable the scene-placed
spawn points of these factions (`SetSpawnPointEnabled_S(false)`). Reinforcements spawn at a free spot within 60 m of the base
center (spread positions, `GetSurfaceY`; vehicles on/near the closest road point if cheap).

## 4. Force generation (`CLW_ForceCatalog`, `CLW_ForceSpawner`)
Catalog entries come from the faction profile `.conf` (section 0a), or auto-discovery. The table is the **vanilla profile data**
to put into `Configs/CLW/Factions/US.conf` / `USSR.conf` (plus a FIA profile from the research file); `type` ids are
free-form strings defined by the data, the commanders only rely on `kind`.

| type | kind | US | USSR | cost |
|---|---|---|---|---|
| `rifle_squad` | infantry | Group_US_RifleSquad {DDF3799FA1387848} | Group_USSR_RifleSquad {E552DABF3636C2AD} | 100 |
| `fire_team` | infantry | Group_US_FireTeam {84E5BBAB25EA23E5} | Group_USSR_FireGroup {30ED11AA4F0D41E5} | 50 |
| `mg_team` | infantry | Group_US_MachineGunTeam {958039B857396B7B} | Group_USSR_MachineGunTeam {A2F75E45C66B1C0A} | 60 |
| `at_team` | infantry | Group_US_Team_LAT {FAEA8B9E1252F56E} | Group_USSR_Team_AT {96BAB56E6558788E} | 70 |
| `recon_team` | infantry | Group_US_ReconTeam {F65B7BB712F46FEE} | Group_USSR_ReconTeam {06F0C9675883F18A} | 60 |
| `motorized` | truck + squad | M923A1_transport {F1FBD0972FA5FE09} + rifle squad | Ural4320_transport {16C1F16C9B053801} + rifle squad | 150 |
| `light_armed` | light vehicle | M1025_armed_M2HB {3EA6F47D95867114} | UAZ469_PKM {0B4DEA8078B78A9B} | 90 |
| `mechanized` | IFV/APC + squad | LAV25 {0FBF8F010F81A4E5} + fire team | BTR70 {C012BB3488BEA0C2} + rifle squad | 280 |
| `armored_recon` | armored car | (none: M1025 again) | BRDM2 {254289B9C09904AB} | 140 |

Full prefab paths: see `research/vehicles-and-spawning.md`. Caps: live AI per faction **50**, vehicles per faction **6**.
A purchase is validated (known type, supplies >= cost, caps), charged at spawn start, refunded if the spawn fails.

**Spawning (server):**
- Infantry: `SpawnEntityPrefab(group prefab)` at the base; members arrive over several ticks; give the group an entity name
  `<FACTION>_<Kind><n>` (e.g. `US_Inf3`, `USSR_Mech1`) via `SetName` *before* the registry sweep sees it (registry ids = entity names).
- Vehicles: spawn the vehicle, then crew via `SCR_BaseCompartmentManagerComponent.SpawnDefaultOccupants({PILOT, TURRET})`
  with `GetOnDoneSpawningDefaultOccupants()` callback -> crew group (from `occupants[0]` -> AIControlComponent -> agent ->
  parent group). Name the crew group `<FACTION>_Mech<n>` / `_Mot<n>` / `_Car<n>`.
  Embarked infantry: spawn the infantry group prefab next to the vehicle, wait until its members exist (poll, <= 20 s), then
  `SCR_CompartmentAccessComponent.MoveInVehicle(vehicle, ECompartmentType.CARGO)` per member (teleport). Members that do not
  fit stay on foot with the group. Name the cargo group `<crew name>_Inf`.
  Make the vehicle usable for both groups: `GetGroupUtilityComponent().AddUsableVehicle(SCR_AIVehicleUsageComponent.FindOnNearestParent(v, v))`.
- Every spawned group: `SetDeleteWhenEmpty(true)`; importance HIGH if the API exists on 1.8.

## 5. Units (what HQ commands)
A **unit** = one commandable thing with an id. Kinds: `infantry` (one group), `motorized`, `light`, `mech`, `armored_recon`
(vehicle + crew group [+ cargo group]). `CLW_VehicleUnit` links vehicle, crew group, cargo group under the **crew group's
registry id**; the cargo group's own registry id is hidden from HQ while linked. Unlink rules:
vehicle destroyed or immobile/abandoned -> the cargo group (and surviving crew) become plain infantry units;
cargo wiped -> the vehicle unit continues alone.

**Vehicle unit state machine** (`CLW_VehicleUnit`, ticked every 2 s by the game mode):
`MOUNTED_IDLE -> DRIVING -> DISMOUNTING -> DISMOUNTED (infantry fights, vehicle overwatch) -> REMOUNTING -> DRIVING ...`
- MOVE target: everyone mounted, crew group gets a Move waypoint at the target (road-adjacent if possible).
- ATTACK target: drive to a **dismount point** 250 m short of the target on the approach line (motorized/truck: 400 m), then
  cargo group gets a GetOut waypoint (`AIWaypoint_GetOut {C40316EE26846CAB}`; fallback per member `GetOutVehicle` teleport
  after 20 s) and then SearchAndDestroy at the target; the IFV/APC moves to an **overwatch point** 150 m behind the
  infantry's target on the same line and Defends there; a **truck** instead drives back to base (`RTB`) and is
  deleted on arrival with crew (refund 50 % of the truck share: +30 supplies). Light/armored recon: crew ATTACKs directly.
- DEFEND target: drive to target, dismount, infantry Defend at target, vehicle Defend 50 m behind.
- New order farther than 600 m while dismounted: REMOUNT (cargo group GetIn waypoint `AIWaypoint_GetIn {712F4795CF8B91C7}`
  with `SetEntity(vehicle)`; vehicle waits); after 90 s: drive off and let the infantry walk (unit splits into two units).
- **Stuck watchdog:** while DRIVING, sample position every 10 s; < 15 m progress in 40 s -> re-issue waypoint (log);
  still stuck after 90 s -> dismount and continue on foot (infantry gets the order, vehicle Defends in place). Log every step.
- Trucks dismount everyone at first contact (vanilla): if the cargo group is on foot within 500 m of a contact, treat it as
  DISMOUNTED and give it the attack order directly.

## 6. Commands (shared by RuleCommander and LLM)
Order target = an objective id (`"C"`) **or** a position `[x, z]`. Commands:

| Command | Applies to | Execution |
|---|---|---|
| `MOVE` | all | infantry: Move wp; vehicles: mounted drive |
| `ATTACK` | all | infantry: SearchAndDestroy; vehicles: dismount/assault/overwatch (section 5). On an objective = capture it |
| `DEFEND` | all | Defend wp at target (vehicles: dismount + Defend) |
| `RECON` | all (best: recon_team, light, armored_recon) | Move wp to target, then Defend there when it arrives or meets contacts (do not assault) |
| `FALL_BACK` | all | ForcedMove `{06E1B6EBD480C6E0}` (ignores combat) to target, default = own base; then Defend |

**Buy:** `{"type":"mechanized","count":1}` (validated as in section 4). **Missions** for human-led squads: section 7.
Order stability: an AI unit is not re-ordered within 45 s unless its order completed/failed or it became weak (< 50 %).
Order lifecycle per unit: `active` (with age), `done` (arrived/objective captured), `failed` (stuck/unit lost).

## 7. Missions for human squad leaders (`CLW_MissionBoard`)
- A human-led squad without an accepted mission gets **2-3 offers** (refresh at most every 3 min, or 60 s after a
  decline/abandon/completion). Offer = vanilla task (`ExtendedTask.et {CF6A2A13DF7B1018}`), ownership EXECUTOR, visibility
  EXECUTOR, `AddTaskExecutor(FromPlayerID(leader))`, **not** assigned. Name `"[HQ] Capture C - Timber Hub"`, description = brief.
- Accept = the player assigns himself in the J menu/map (`SCR_Task.GetOnTaskAssigneeAdded()`); then the sibling offers are deleted.
  Abandon = `GetOnTaskAssigneeRemoved()` while the task is not COMPLETED -> HQ replans, new offers after 60 s.
- Kinds: `CAPTURE <point>` (done when owned by us), `DEFEND <point>` (hold 4 min while the point stays ours; progress shown),
  `RECON <pos>` (leader within 75 m), `DESTROY <pos of contact>` (squad within 150 m and no contact left there for 60 s).
  Timeout 15 min -> FAILED. Rewards: +60 supplies (+100 for major point), shown to the player as a popup
  (`SCR_PopUpNotification.PopupMsg` via owner RPC, sounds TASK_ACCEPT / TASK_SUCCEED / TASK_FAILED) and in `CLW_Debug.Say`.
- Rule commander generates offers from its own plan (the most valuable target it has not covered, a threatened own point,
  a stale contact to recon). The LLM may send offers with its own prose brief (section 8); else the rules generate them.
- Replaces today's forced `ShowMissionTask` path for human-led squads.

## 8. Situation and reply: protocol v2 (game <-> sidecar)
POST `/orders` body (one faction):
```json
{"v":2,"faction":"US","time":1234,"timeLeftSec":3200,
 "war":{"tickets":350,"enemyTickets":310,"supplies":220,"income":60,"liveAI":38,"aiCap":50,"vehicles":3,"vehicleCap":6},
 "objectives":[{"id":"C","name":"Timber Hub","pos":[2600,2750],"radius":80,"value":2,"owner":"none|US|USSR","contested":false,"progress":-35}],
 "bases":[{"faction":"US","pos":[1560,3090]},{"faction":"USSR","pos":[3560,2740]}],
 "catalog":[{"type":"rifle_squad","kind":"infantry","cost":100,"desc":"9 rifle infantry"}],
 "squads":[{"id":"US_Mech1","kind":"mech","type":"LAV25 + US FireTeam","pos":[x,y,z],"agents":7,"startAgents":7,
   "vehicle":{"type":"LAV25","health":0.8,"mounted":true,"state":"DRIVING"},
   "humanLed":false,"wounded":0,"health":1.0,"speed":8.2,"heading":90,"state":"moving",
   "objective":{"command":"ATTACK","target":"C","pos":[2600,2750],"reason":"...","ageSec":40,"status":"active"},
   "mission":null}],
 "contacts":[{"pos":[x,y,z],"desc":"4x USSR infantry","kind":"infantry|vehicle|unknown","ageSec":12}],
 "events":["C captured by USSR","US_Inf2 lost"]}
```
- `squads[].mission` for human-led squads: `{"state":"offered|accepted|none","kind":"CAPTURE","target":"C"}`.
- `contacts[].kind`: vehicle if any target in the cluster is a vehicle entity (`Vehicle` / has `SCR_VehicleDamageManagerComponent`).
- `events`: things since the last POST (capture/loss of points, units lost/spawned, mission accepted/completed), max 10.
- The body must stay < 60 KB; drop the catalog `desc` and old contacts first if needed.

Reply:
```json
{"orders":[{"squad":"US_Mech1","command":"ATTACK","target":"C","reason":"..."},
           {"squad":"US_Inf2","command":"MOVE","pos":[2200,2900],"reason":"..."}],
 "buy":[{"type":"mechanized","count":1,"reason":"..."}],
 "missions":[{"squad":"US_PlayerSquad","offers":[{"kind":"CAPTURE","target":"D","brief":"Seize the north junction..."}]}],
 "next_check_sec":90,"next_check_reason":"..."}
```
- `target` (objective id) or `pos` ([x,z]) per order; one order per squad; max 3 buys per reply; max 3 offers per squad.
- `keep:true` with nothing else = keep current orders (unchanged). v1 replies (orders with pos, commands MOVE/ATTACK/DEFEND)
  must still be accepted by the game.
- The game validates everything again (ids, commands, targets, budget, caps, humanLed squads only get missions).

## 9. Rule commander v1 (doctrine, in game, default and fallback)
Every think (AI-only units; human-led squads only through missions):
1. **Spend**: while supplies allow and caps permit, buy toward a target mix (by unit count): 45 % infantry squads/teams,
   20 % motorized, 20 % mechanized, 15 % light/armored recon; at most 2 buys per think; keep a 40-supply reserve.
   If known enemy contacts include vehicles, prefer `at_team` for the next infantry buy.
2. **Tasks** from the points: `DEFEND` own points with a known enemy contact within 400 m (threat = contact counts);
   `ATTACK` neutral/enemy points ordered by value / distance from our base (the front advances outward);
   `RECON` the most valuable point with no contact info if we have a recon/light unit; keep **one reserve** at base
   (newest infantry) until supplies are rich (> 300) or a point is threatened.
3. **Assignment**: greedy by distance; vehicle units preferred for targets > 800 m; at most 3 units per attack target
   and 2 per defend target; weak units (< 50 %) `FALL_BACK` to the nearest own point/base; idle units with a contact
   within 300 m attack it.
4. Stability (section 6) and logging: one line per decision with the reason.
Difficulty knobs (consts): think interval, reserve size, max units per target.

## 10. Sidecar + dashboard (Node, `dev/tools/`)
- Accept protocol v2 situations (keep v1 working). Validate replies v2: orders (`target` known objective or finite `pos`;
  commands MOVE/ATTACK/DEFEND/RECON/FALL_BACK; squad known; humanLed squads never get orders, only missions), `buy`
  (known catalog type, count 1-3, total cost <= supplies), `missions` (human-led squads only, kinds CAPTURE/DEFEND/RECON/DESTROY,
  max 3, brief <= 240 chars).
- New system prompt: doctrine summary (combined arms: mech/motorized for distance, infantry to hold, recon first,
  objectives win the war via ticket bleed, preserve tickets, spend supplies), field explanations, reply schema.
  Keep it compact (< 1.5k tokens).
- Fake provider v2: simple doctrine (buy if rich, attack nearest non-owned objective, defend threatened), offers for human squads.
- New triggers: `objectiveChanged` (any owner change), `ticketsDropPct` (own tickets dropped >= 10 % since last call),
  `unitSpawned`, `missionEvent`. Config + dashboard settings for them.
- Dashboard: objectives on the map (circle by radius, owner colour, contested hatch, progress), bases, war strip per
  faction (tickets, supplies, income, AI/vehicle counts vs caps), unit kind icon/label per squad (vehicle type, mounted
  state), purchases + mission offers in the exchange log, events list.

## 11. Implementation phases (all on branch `feature/m2-real-war`)
| Phase | Owner files (new unless noted) | Depends on |
|---|---|---|
| **P1 War foundation** | `CLW_WarConfig.c`, `CLW_ScenarioConfig.c` (+ `Configs/CLW/Scenarios/Arland.conf`, attribute on `CLW_GameMode.et`), `CLW_ControlPoints.c`, `CLW_WarState.c`, `CLW_Bases.c`, `CLW_GameMode.c` (edit), `CLW_SquadRegistry.c` + `CLW_ContactMarkers.c` (edit: factions/identity from the scenario) | - |
| **P2 Forces + mechanized** | `CLW_FactionProfile.c` (+ `Configs/CLW/Factions/US/USSR/FIA.conf`), `CLW_ForceCatalog.c` (profile + auto-discovery + classifier), `CLW_ForceSpawner.c`, `CLW_VehicleUnit.c`, `CLW_SquadRegistry.c` (edit: kind/link/hide), `CLW_SquadInfo.c` (edit: display type from data) | P1 |
| **P3 Commander v1 + protocol v2** | `CLW_Orders.c` (executor), `CLW_RuleCommander.c`, `CLW_RemoteCommander.c`, `CLW_Contact.c` (kind) | P1, P2 |
| **P4 Missions** | `CLW_MissionBoard.c`, `CLW_MissionPopup` (RPC on `SCR_PlayerController`, new file) | P3 |
| **P5 Setup menu** | setup menu UI (layout + script), RPCs on `SCR_PlayerController` (new file), waiting hint for other players | P1 |
| **S Sidecar + dashboard** | `dev/tools/llm_sidecar.mjs`, `dev/tools/dashboard/*` | this spec only (parallel) |
| **E2E** | one Workbench run of everything, fixes, PROGRESS/skills update | all |
