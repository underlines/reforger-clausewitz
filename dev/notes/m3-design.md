# M3 "Logistics, bases & requests": design (decided 2026-10-12)

Inputs: `decision-command-tools.md` (VISION 5a, proposed), `research/m3-bases-fobs-requests.md`,
`research/llm-tool-design-at-scale.md`, `loadouts-arsenal-plan.md`, `squad-join-design.md` (step 10 command post),
`dedicated-server.md`. Ground rules from `m2-design.md` sections 0/0a still bind: content as data, no vanilla
hard-wiring, server authority, subagents code only, one Workbench e2e at the end (plus a dedicated-server join test).

## 0. Goal
Wars get a logistics backbone and humans get a way to ask HQ for things. A side has bases with **services** (command
post, vehicle depot, arsenal, medical/repair), HQ can **establish FOBs** and must **supply them by truck**, players
**request** vehicles / squads / resupply at buildings, by chat or (with the LLM) in free text, and **report** contacts.
Arsenals and loadout editors work with RHS / WCS / GRS, faction-gated.

## 1. War-mode capabilities (what makes FOBs etc. mode-dependent)
Each war mode declares capabilities + parameters; everything in M3 checks them, and registry entries whose `requires`
are unmet drop out of the LLM manifest and the request menus.

| Capability | Short conflict (default) | Long conflict | Endless |
|---|---|---|---|
| `services` (base buildings) | on | on | on |
| `fob` | on, max 1-2, build `instant` or short `timed` | on, max 3-4, `timed`, service tiers | on, auto-managed, disposable |
| `logistics` (per-base stock, convoys) | on (default) | on | on |
| `requests` (player requests) | on | on | on |
| `reports` (player contact reports) | on | on | on |
| `kitCost` (arsenal kits cost supplies) | on (default) | on | on |
Setup menu shows the parameters (max FOBs, build mode, FOB cost, convoy size) from the mode params like today.
**Owner decision 2026-10-12:** logistics is on by default in every mode and can be switched off completely in the setup
menu (then: one faction pool as today, FOBs fed abstractly). Arsenal kits cost supplies from day one; a setup-menu toggle
makes them free.

## 2. Action registry (step 1, no behaviour change)
As in `decision-command-tools.md`: `CLW_ActionRegistry` + entries (`.conf` data + executor classes), manifest sent to the
sidecar at war start (and on mode/faction change). Port MOVE/ATTACK/DEFEND/RECON/FALL_BACK, buy, mission offers onto it;
validator and prompt command list generated from it. Protocol v3 = v2 + `manifest` hash + new situation blocks (§8).

## 2a. Spike S0 results (2026-10-12, Workbench, builds #37-#39, `CLW_Spike.c` on branch `spike/m3-services`)
- Script-placed vanilla services work in `CLW_GameMode`: `VehicleService_US`, `ArsenalBox_US`, `SupplyCache_S_US_01`,
  `FieldHospital_M_Conflict_US_01`, `Headquarters_S_Conflict_US_01` spawn, register as service points (ONLINE, faction US,
  `bases=1`); the depot's `SCR_CatalogEntitySpawnerComponent` lists 26 US catalog vehicles; arsenal 173 items (uses supplies,
  save type FACTION_ITEMS_ONLY).
- Supplies: `SCR_ResourceContainer.SetResourceValue` fills containers from script (owner saw the cache at 3000/3000 in game).
  `AddSpawnerSupplies` has **no effect** on this depot (it consumes from base-linked containers); a filled cache 50 m away did
  not unlock its requests (`NOT_ENOUGH_SUPPLIES`): the supply link goes through the `SCR_MilitaryBaseComponent` the services join.
- Player interaction layer is Conflict-bound: HQ whiteboard build menu does not open (needs `SCR_CampaignBuildingManagerComponent`);
  `VehicleService_US` root has 1 action (the catalog request actions sit on other Conflict prefabs: request table / depot sign).
- Chat commands never reach their handler in Workbench (`/spike`, also `/sitrep` unseen): don't rely on chat for tests.
- **Consequence (design):** vanilla compositions = 3D + service logic only. Player interaction = our own user actions on the
  buildings -> action registry (§6). Supplies = one CLW container per base/FOB, registered with a vanilla military base and
  synced with the ledger. Vehicle/squad requests go through the registry + `CLW_ForceSpawner` (crews, caps, pack vehicles),
  not the vanilla depot menu.
- Arsenal (owner, in game): opens, shows **5000 supplies** (the filled cache 3000 + nearby 1000-containers) and the value drops
  when taking items; nothing greyed out with enough stock. So kit cost works out of the box against nearby containers: the CLW
  base/FOB container just has to be in range (verify the range / base link in phase B with logging).

## 3. Bases and services
- `CLW_Base` gets **service slots**: kind (`command_post`, `vehicle_depot`, `arsenal`, `medical`, `repair`) -> a
  composition prefab from **data** (faction profile `m_aServices`, with a kin/neutral fallback for pack factions that ship
  none; never hard-wired US/USSR prefabs). Spawned instantly at war start for HQ bases (`SpawnEntityPrefab`), at FOBs by tier.
- **Vehicle depot / request table (changed after spike S0):** the vanilla depot composition is the building; requests are our
  own user action on it ("Request vehicle/squad" -> registry entry -> grant logic -> `CLW_ForceSpawner` at the depot's spawn
  slots), listing the faction's `CLW_ForceCatalog` (RHS/WCS by data, our caps and crew fixes). The vanilla
  `SCR_CatalogEntitySpawnerComponent` menu is not used (its actions live on Conflict prefabs and its supplies need a
  base-linked container).
- **Supplies:** one CLW supply container per base/FOB registered with a vanilla `SCR_MilitaryBaseComponent` (so vanilla
  consumers such as the arsenal and support stations draw from it), value synced with `CLW_WarState` / `CLW_SupplyStore`.
- **Arsenal:** `CLW_Arsenal.et` (vanilla box child) + the kin item pool from `loadouts-arsenal-plan.md`. Kits cost
  supplies (vanilla item supply cost, drawn from the base/FOB stock or the faction pool via a CLW resource container);
  setup toggle `kitCost` off = buy multiplier 0 (free, the RHS trick).
- **Command post:** user actions on the HQ composition: join/switch squad (squad-join step 10), request menu (§6), SITREP.
- **Medical / repair:** vanilla support stations inside the compositions; nothing custom.

## 4. FOBs
- Intent `ESTABLISH_FOB <site>` (macro): reserve cost -> assign a supply truck + an infantry escort (buy if none) -> move ->
  on arrival build via `CLW_BuildExecutor` (`instant` = spawn composition; `timed` = outline + `AddBuildingValue` progress
  while the truck is on site; `engineer` = later) -> spawn point + service tier 1 (command post, arsenal), tier 2 adds
  depot/medical when stock allows.
- **Sites:** candidates from the scenario/battle-size layout hook (data) plus validated free positions (min distance to
  bases/objectives/other FOBs, road access, not inside enemy contact radius). LLM and rules pick from candidates first.
- **Loss:** enemy holding the FOB zone or destroying the command post -> FOB lost, its stock lost (or partly captured,
  mode param), spawn point removed. Players and AI respawn there at ticket cost like bases.
- Rule commander doctrine: one FOB toward the contested centre once supplies allow; LLM may do more within the cap.

## 5. Logistics (capability `logistics`)
- Per-base stock (`CLW_SupplyStore`), income lands at the HQ base. Intents `RESUPPLY <base|fob>` / `CONVOY <A -> B>`:
  supply-truck units (vehicle kind `logistics`, cargo amount from data) drive, unload on arrival via the resource API
  (vanilla Conflict truck/waypoint system is Campaign-typed, not reused). Trucks can be ambushed; cargo is lost with them.
- Without `logistics`: one faction pool (today), FOB build/upkeep paid from it.
- Spending at a FOB (respawn, depot, kits if costed) draws from that FOB's stock.

## 6. Player requests
- **Fast path (no LLM needed):** request menu at command post / depot / FOB, chat `/request vehicle|squad|resupply|recon|
  reinforce`, later quick menu. Each maps to a registry entry with rule grant logic (ledger, caps, cooldown per player,
  role: squad leaders may request squads/vehicles, soldiers kits/transport). Answer by hint + chat.
- **Free text (LLM on):** `/hq <text>` -> sidecar **agent turn** (AI SDK `ToolLoopAgent`, <= 4 steps, scoped tools +
  `toolSearch`, async). Action tools queue for the game and return an id; the game executes on the next poll and the
  answer comes back as radio/chat. Grant, partial grant, counter-offer or refuse with a reason.
- Rule fallback answers free text with the fast-path menu hint.

## 7. Player reports (VISION 8)
Marker parsing (enemy markers -> `Contact`, source `human`, confidence param) and `/report <count> <inf|veh|armor> <dist>
<dir>`; contacts expire like AI ones. Quick menu later.

## 8. Protocol v3 additions (situation)
`bases[]` and `fobs[]` with services, stock, state (building %, under attack), `convoys[]`, `requests[]` (pending player
requests with requester role), `manifest` hash; reply adds intents from the registry and `requestDecisions[]`.

## 9. Content compat test (RHS / WCS / GRS)
Test packs: add GRS (`GRS - Bundle` 6518EA4DDEA42E15 + `GRS - USSR` 667018A2DF8C1AA4) next to RHS/WCS. Run the 10-step
checklist from `research/m3-bases-fobs-requests.md` §2 in the e2e: arsenal item counts server/client, kin items shown,
enemy items absent, editor save + respawn with kin items (save type), RHS role loadouts, depot lists pack vehicles.

## 10. Phases (branch `feature/m3-logistics`, subagents code only)
| Phase | Content | Depends |
|---|---|---|
| **S0 Spike (me, Workbench, short)** | script-placed depot/arsenal/service composition in `GameMode_Base`: actions populate, request spends, arsenal not greyed; one listen-server run | - |
| **R Registry** | §2, port existing commands, manifest, protocol v3 skeleton | - |
| **B Bases + services** | §3, faction profile service data, CLW_Arsenal + kin pool (loadouts plan) | S0 |
| **F FOBs + build executor** | §4, mode capabilities §1 | R, B |
| **L Logistics** | §5 | F |
| **Q Requests + reports** | §6 fast path, §7 | R, B |
| **A Agent turn** | sidecar `package.json`, AI SDK, `callModel`/`runAgentTurn`, `/hq`, dashboard request log | R, Q |
| **E2E** | Workbench run (vanilla + RHS/WCS/GRS packs) + dedicated server with one client | all |

## 11. Owner decisions (2026-10-12)
1. Layered registry + Vercel AI SDK: **approved** (`decision-command-tools.md`).
2. S0 spike in Workbench first (script-placed depot + arsenal): **yes**.
3. Arsenal kits cost supplies from day one; setup-menu toggle makes them free.
4. Logistics on by default; setup menu can switch it off completely.
5. Phase order (not answered, default kept): R+B+Q first, F+L second, A last.
