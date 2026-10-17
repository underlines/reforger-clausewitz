# Catalog overrides (server file and scenario)

Server owners cannot edit `.conf` files packed in the mod. Clausewitz therefore reads
`$profile:Clausewitz/overrides.json` (the server profile directory, e.g. `<profile>/Clausewitz/overrides.json`) every time a force
catalog is built (war start). If the file does not exist an example is written there (entry `EXAMPLE_ONLY`, disabled, matches
nothing) and the path is logged (`CLW: overrides: ...`).

Order of application, after profile + auto-discovery: scenario `m_aOverrides` (`CLW_CatalogOverride`, for compat addons), then the
server file (the server owner has the last word). Every change is logged: `CLW: overrides: [file] match='...' entry <type>: kind a->b cost 60->90`.

## Schema
```json
{"entries":[
  {"match":"M1025","faction":"US","kind":"light","cost":90,"weight":10,"enabled":true,"name":"M1025 M2","antiArmor":false}
]}
```
| field | meaning |
|---|---|
| `match` (required) | case-insensitive substring of the vehicle / group / cargo-group prefab path, OR exact type id, OR substring of the display / vehicle name. All matching entries change |
| `faction` | only for this faction key (missing / empty = all factions) |
| `kind` | `infantry`, `recon`, `motorized`, `light`, `mech`, `armor`, `armored_recon` |
| `cost`, `weight` | integers >= 0 (supplies cost, buy-mix weight) |
| `enabled` | `false` hides the entry from the buy mix. Entries disabled in the faction profile are dropped earlier and cannot be re-enabled |
| `name` | display name (also the vehicle name of vehicle entries) |
| `antiArmor` | marks the entry as an anti-armour team |

Every field except `match` is optional; a missing field leaves the entry unchanged (the loader reads the file twice with different
defaults to tell "missing" from "set").

## Scenario `.conf`
`m_aOverrides` of the scenario takes `CLW_CatalogOverride` objects with the same fields (`m_sMatch`, `m_sFaction`, `m_sKind`,
`m_iCost`, `m_iWeight`, `m_iEnabled` -1/0/1, `m_sName`, `m_iAntiArmor` -1/0/1; -1 / empty = unchanged).

## Debug chat (`debugChat`)
Top-level key of the same file: `{"debugChat":true,"entries":[...]}`. Default `false` (absent = off). Read once at the first debug line
(restart the server/mission to change it; logged as `CLW: debug chat true`).
- `false`: `CLW_Debug.Say` debug lines (spawns, orders, mission lines, new contacts, point captures, HQ replies, build stamp, ...) are NOT shown in
  player chat. They still go to the console/log (`CLW: ...`) and, via a ring buffer (last 50 lines, 160 chars each), with the situation
  POST to the sidecar (`debug: [{id,t,msg}]`), where the dashboard tab "Debug log" lists them (`/api/state` `debugLog`).
- `true`: the lines also appear in every player's chat as before.
Player-facing messages (setup menu, `/war` and `/sitrep` replies, mission popups, SITREP hints, war over, hold-all countdown) are never gated
(`CLW_Debug.Show` / `CLW_Debug.Announce`).

## LLM sidecar endpoint (`sidecarUrl`, `sidecarToken`)
Top-level keys of the same file, read once per server start (`CLW_SidecarLink`, log `CLW: sidecar url ... (source, token set|no token)`).
- `sidecarUrl`: base URL of the sidecar, e.g. `"http://10.0.0.5:8788/"` or `"https://clw.example.com/"`. Default `http://127.0.0.1:8788/`.
  The command line `-clwSidecar <url>` wins over the file.
- `sidecarToken`: sent as header `X-CLW-Token` on every game request; must equal the sidecar's `gameToken`. Command line
  `-clwSidecarToken <token>` wins over the file. Set one whenever the sidecar is reachable from other machines.

## War setup preset (`setup`)
Top-level key of the same file. It lets a dedicated server start the war unattended with chosen settings, without a human in the setup menu.
Without it, the scenario's auto start (`m_iAutoStartSec`, 180 s on Arland) uses the defaults. Read once when the setup phase begins
(restart the server/mission to change it). Code: `CLW_SetupPreset.c`.
```json
{"setup":{"autoStartSec":10,"mode":"short","factionA":"RHS_USAF","factionB":"RHS_AFRF",
          "startPositions":"random","randomSeed":0,"battleSize":"large","timeLimitMin":90}}
```
The object is flat and uses the same keys and values as the menu and `/war start key=value`:
| key | meaning |
|---|---|
| `autoStartSec` | seconds after game start to start the war with the preset. Default 10, minimum 5. `-1` = no auto start: the preset only pre-fills the menu, and the scenario's auto start is off as well |
| `mode` | war mode id (missing = scenario default) |
| `factionA`, `factionB` | faction keys of side A / B (missing = scenario sides) |
| anything else | a parameter of that mode (missing = mode default), e.g. `startPositions`, `battleSize`, `startTickets`, `fobMax`, `fobEarliestMin` (default 6, 0 = no FOB timing gate), `garrisonSquads` (default 1), `idleDespawnMin` (default 6, 0 = off), `cap_logistics`, the friendly-map switches `mapMe` (true), `mapSquadmates` (true), `mapSquads` (false), `mapVehicles` (false) |

- M4.6 keys: `aiDifficultyA` / `aiDifficultyB` (`easy` | `normal` (default, vanilla, a no-op) | `hard` | `veteran`; `aiDifficulty` sets both) apply a vanilla AI skill + perception preset to every AI of that side (`CLW_AiDifficulty.c`, dev/research/ai-difficulty.md). `randomSeed` (layout seed > 0, 0 = a fresh random one per start) is hidden in the setup dialog: it exists only here and as `/war start randomSeed=N`.
- Remembered setup (M4.6): every accepted setup is written to `$profile:Clausewitz/lastSetup.json` (never the seed) and prefills the next round's menu, also after a server restart. A preset with `autoStartSec >= 0` wins over it; delete the file to forget. The dialog's **Reset to defaults** restores this preset (or the built-in values), not the last round. Code: `CLW_SetupMemory.c`.
- The preset values are also the menu defaults, so a human who joins before the auto start sees them and can still change them and start earlier.
- An invalid preset (unknown mode, faction or parameter, bad value) is NOT replaced by defaults. The war stays in setup, and the log shows
  `CLW: ERROR setup preset ...` plus the `StartWar refused` reason. Players get a chat line pointing them to `/war setup`.
- Log: `CLW: setup preset from overrides.json: ...` at load and `CLW: war auto start from the setup preset: ...` at start.

## Side pools (`sides`) - M4 WP3
Code: `CLW_SidePool.c` (rules), `CLW_ForceCatalog.FillFromKin`. A side's buy catalog is built as: its own faction entries (profile + auto-discovery,
catalog overrides applied), then **kin** entries for every role (kind) the own faction lacks. Kinds: infantry, recon, motorized, light, mech,
armor, armored_recon, logistics (a logistics role needs a vehicle and a cargo amount). The first kin faction that has a role fills it; the
entries are cloned with type id `kin_<type>` and carry `m_sKinFrom` ("kin" in the catalog log). Own entries always win, duplicates (same role and
same vehicle model / group prefab) are skipped. This is what gives RHS_USAF / RHS_AFRF a supply truck (the vanilla US / USSR `supply_truck`
profile entry) so convoys run.
- Kin = (1) `sides.<key>.kin` of the server file, (2) `m_aKinFactions` of the faction profile, (3) the factions the faction data calls friendly
  (same rule as the arsenal item pool: a pack faction lists the vanilla faction it is built on). Never the side itself, the enemy side, a faction
  friendly to the enemy side (civilians), a faction on the scenario's ignore list, or one excluded below.
- Entries whose vehicle has no default crew (`crewless`) are never imported and never bought by the AI (see "Crew check").

```json
{"sides":{
  "*":        {"excludePathContains":["Tank_Destroyer"]},
  "RHS_USAF": {"kin":["US"], "excludeFactions":["FIA"], "excludePathContains":["Mortar"]},
  "WCS_X":    {"ownOnly":true}
}}
```
| key (per faction key, `*` = every side, both merged) | meaning |
|---|---|
| `ownOnly` | `true`: no kin at all for this side (roles it lacks stay empty) |
| `kin` | extra kin faction keys, tried first |
| `excludeFactions` | faction keys never used as kin (also removes them from the explicit lists) |
| `excludePathContains` | prefab path texts (case-insensitive) never used: own AND kin entries whose vehicle / group / cargo-group prefab path contains one are disabled |

Log: `CLW: overrides sides '<key>': ...` (file read), `CLW: sidepool <side>: roles without an own entry: ...; kin factions: ...`,
`CLW: sidepool <side>: role <kind> filled from <kin>: <type> ...`, `CLW: sidepool <side>: roles still without an entry ...`,
`CLW: catalog <side> built: N entries (a from profile, b discovered, c from kin, d crewless, e path-excluded)`.

### Crew check
Discovered vehicles are spawn-probed once (cached per prefab, at most 80 real spawns per catalog build): a vehicle with no pilot / crew seat
that has a default occupant prefab cannot be crewed by `SpawnDefaultOccupants` (seen on RHS_AFRF `APC_K17_unarmed.et`, `K4386.et`). Such an entry
is kept but marked **crewless**: `CLW_ForceCatalog.GetEntries` / `Find` leave it out (AI buy mix, FOB builder, convoys), `includeCrewless = true`
lists it (player vehicle request: the vehicle is spawned empty). A purchase that still hits "no default crew" marks the entry crewless at once
(`CLW: catalog <side> entry <type> marked CREWLESS`). Log: `CLW: catalog crew check <side>: <prefab> ok|has NO default crew|unknown`.

## Props (`props`) and style sets - M4 WP3
Code: `CLW_PropResolver.c`. Roles: `service.command_post`, `service.supply`, `service.arsenal`, `service.vehicle_depot`, `service.medical`,
`service.repair`, FOB kit `fob.arsenal_box`, `fob.supply_box`, `fort.sandbag_long`. The role keeps its placement data (offset, yaw, radius,
footprint, map marker) from the faction data; only the prefab changes.

Resolution chain per side + role (first prefab that **loads** wins):
1. admin override `props` of the server file
2. shipped preference sets `Configs/CLW/Props/PropSets.conf` (non-vanilla sets whose `m_aFactions` names the side)
3. faction data: the side's profile, the profile of `m_sServiceFallbackFaction`, a friendly (kin) profile of the scenario list
4. the vanilla set that fits the side's kin (`m_bVanilla 1`), last resort
5. none (logged)
A side with no service data at all gets its base services synthesized from steps 1, 2 and 4 (`CLW: props <side>: ... services synthesized`).
Prefabs taken from an override or set have their default faction affiliation moved to the side (`CLW_Services.AdoptFaction`, source key `*`).

```json
{"props":{
  "*":        {"fort.sandbag_long":"vanilla"},
  "RHS_USAF": {"style":"my_pack", "service.arsenal":"{0123456789ABCDEF}Prefabs/Props/Pack/Arsenal.et"},
  "RHS_AFRF": {"fob.supply_box":"vanilla"}
}}
```
| value | meaning |
|---|---|
| a prefab path (`{GUID}Prefabs/...et` or a plain path) | use exactly this prefab for the role |
| `"vanilla"` | the vanilla set (US / USSR / FIA look, the one matching the side's kin) |
| a set name | the candidate of that set (`PropSets.conf`) for the role |
| `"style": "<set name or vanilla>"` (a key next to the roles) | applies that set to every role of the side that has no entry of its own |
A value that does not load / names no set / the set has no such role is logged once (`CLW: props <side> <role>: override '<value>' is not usable ...`)
and the chain continues with the next source. Per side + role one line: `CLW: props <side> <role> -> <prefab path> (source override prefab|override vanilla (set x)|override style set x|set <name>|profile <key>|fallback <key>|kin <key>|vanilla set <name>|none)`.

### PropSets.conf
`CLW_PropSets { m_aSets { CLW_PropSet { m_sName, m_bVanilla, m_aFactions, m_aRoles { CLW_PropRoleCandidates { m_sRole, m_aPrefabs } } } } }`.
Shipped: `vanilla_us`, `vanilla_ussr`, `vanilla_fia` (all `m_bVanilla`, the prefabs of the three vanilla profiles). Pack sets: add a `CLW_PropSet`
with `m_bVanilla 0` and `m_aFactions { "RHS_USAF" }` and the pack's prefabs (candidates in order, the first that loads is used; a missing pack
just falls through). None is shipped yet: no pack GUID could be verified (the enfusion asset index has vanilla only).

## Takeover switch (`takeover`) - M4 option C
Top-level key of the same file: `{"takeover":true}`. Default `false` (absent = off). Read once at the game start, SERVER only. When on, Clausewitz takes
over a game mode entity that is not our `CLW_GameMode` (e.g. the `SCR_GameModeEditor` of the vanilla Game Master worlds `worlds/GameMaster/GM_*.ent`) and
runs the war inside it; `CLW_GameMode` worlds are unaffected either way. The command line `-clwTakeover` has the same effect and wins in the log
(`CLW: takeover of <class> in <world> (switch: cli|overrides)`). The scenario of a taken-over world: command line `-clwScenario <name>`
(`Configs/CLW/Scenarios/<name>.conf`), else the world file name without a leading `GM_` (`GM_Arland` -> `Arland.conf`), else by-world / by-listing / auto.
Recipe: `dev/notes/maps.md` ("Run a Game Master world with takeover"). Code: `CLW_WarRuntime.TakeoverSwitch`.

## Economy guard params (`hqReserve`, `convoyMinLoad`) - M4 round 3 WP6
War-mode params (setup menu, preset, `CLW_WarMode_ShortConflict`; no overrides.json key): `hqReserve` (default 100 supplies, 0 = no floor) is the HQ stock
that AI purchases (rule commander, LLM BUY, convoy trucks) and convoy loads never go below; player requests (`/request`, HQ menu, arsenal kits) may.
`convoyMinLoad` (default 60): a convoy loads `stock - hqReserve` (capped by the truck) and is skipped below this. Code: `CLW_EconomyGuard` (`CLW_SupplyStore.c`).
Refusal text for the AI / LLM: `HQ reserve: stock <s>, reserve <r>`. The situation shows `baseRules.hqReserve` and `logisticsRules.hqReserve`.
Logs: `CLW: convoy <side> skipped: loadable <n> < min <m> (stock <s>, reserve <r>)`, `CLW: RuleCommander <side>: no buy, AI cap <n>/<cap>`,
`CLW: RuleCommander <side>: no buy, HQ reserve: stock <s>, reserve <r>`, `CLW: idle check <side>: candidates <n>, oldest <s> s (...)` (once a minute per side).
## Rematch (`rematch`, `rematchDelaySec`) - M4 round 3 WP4
Top-level keys of the same file, read again at every war end (SERVER only). What happens after the war is over:
```json
{"rematch":"inPlace","rematchDelaySec":60}
```
| key | values | meaning |
|---|---|---|
| `rematch` | `"inPlace"` (default) | no engine game end, no world reload: results stay `rematchDelaySec` s, then the war entities are deleted, runtime state is reset and the session is back in the SETUP phase (setup menu for the setup owner, the `setup` preset with `autoStartSec` starts the next war). Players are switched to the default side and respawned through the setup flow. |
| | `"reload"` | the old behaviour: `EndGameMode`, the server reloads the world about 30 s later (clients may fail to rejoin with `JIP_ERROR`). The war runtime statics are cleared in the game mode's `OnGameEnd`. |
| | `"off"` | the war stays ENDED, nothing happens afterwards (restart the server for a new war). |
| `rematchDelaySec` | number, default 60, minimum 5 | seconds the results (HUD winner label, `WAR OVER` + countdown announcements) stay before the reset. |
Without `EndGameMode` there is no vanilla end screen; the results are the HUD (phase ENDED shows "<faction> wins"), the announcements and the log.
Log lines: `CLW: rematch: choice ...`, `war over, results for <n> s`, `reset <manager> (<n> entities)`, `census before/after`, `back to setup (round <k>)`. Code: `CLW_Rematch.c`.

## Kin fill mode (`kinFill`, `kinWeight`, `sidesDefault`) - wp9
By default (`"kinFill":"missing"`) kin entries are imported only for roles the own faction lacks. With the vehicle packs (M1 Abrams, M2 Bradley, JLTV in the
vanilla US catalog) a pack faction such as RHS_USAF already has an armor / light entry, so the other modern vehicles never reach the AI buy pool,
`/request vehicle` or the LLM manifest. `"kinFill":"all"` also imports the kin entries for roles the own faction already has:
own entries first. For roles the side already has, ONLY kin entries with a vehicle are imported (no kin infantry / recon foot teams / cargo-only groups: the side keeps its own infantry identity) unless `"kinAllInfantry": true` is set. Roles the side lacks are filled with any entry. Kin entries are deduped by vehicle model / group prefab as before (so an Abrams the side already owns is not added twice), the kin
entries' weight scaled by `kinWeight` (default 0.5, minimum weight 1). Catalog overrides (`overrides.json` `overrides` list) apply to the imported
entries afterwards, so an explicit `weight` there wins. `ownOnly`, `excludeFactions` and `excludePathContains` still apply.

| key | meaning |
|---|---|
| `kinFill` | `missing` (default) or `all`, per side key, `*` or in `sidesDefault` |
| `kinWeight` | number > 0, weight factor of kin entries imported for a role the side already has (default 0.5) |
| `kinAllInfantry` | `true`: kinFill all also imports foot entries for roles the side already has (default false) |
| `kinInfantry` | `own` (default): every infantry group of an imported kin entry (cargo group, foot team) is replaced by the side's own group closest in size; `kin`: keep the kin faction's soldiers (old behaviour). Per side key, `*` or in `sidesDefault` |
| `sidesDefault` | `{"kinFill":..., "kinWeight":..., "kinInfantry":...}`: default for every side; a `*` or per-faction `sides` value overrides it |

Enable `all` for every side:
```json
{"sidesDefault": {"kinFill": "all"}}
```
or equivalently `{"sides": {"*": {"kinFill": "all", "kinWeight": 0.5}}}`.
Log: `CLW: overrides sidesDefault: ...`, `CLW: overrides sides '<key>': ... kinFill='all' kinWeight=...`,
`CLW: sidepool <side>: role <kind> added (own role present, kinFill all) from <kin>: kin_<type> (<vehicle>) cost=.. cargo=.. weight=..` (one line per
imported entry; roles the side lacked keep `filled from <kin>`). `/request vehicle`, the HQ menu, the AI buy pool and the LLM catalog manifest all read
`CLW_ForceCatalog.GetEntries`, so imported entries appear everywhere.

## Urban base placement (WP11): HQ in villages, clearing small obstacles
Random / auto start positions now prefer HQs in or at the edge of villages and towns, 8-30 m beside a vehicle road. Code: `CLW_RandomLayout.FindHqUrban`, `CLW_PlaceClear`, `CLW_FreeSpot.CheckPose` (clear mode).
**HQ site search** (scenario `m_RandomLayout` block, all new fields optional: 0 / missing = built-in default; `m_bNoUrban 1` = old open-ground rules; the relaxed second search always uses the old rules):
| field | default | meaning |
|---|---|---|
| `m_fUrbanCoreHalfM` | 12 | half size of the clear core (compact base, command post ~20x21 m) |
| `m_fRoadPreferMinM` / `m_fRoadPreferMaxM` | 8 / 30 | preferred distance to the nearest vehicle road (cost 0.5 per metre outside the band) |
| `m_fRoadRejectM` | 60 | farther from a vehicle road = rejected |
| `m_fPlaceNearM` | 250 | within this of a village/town centre (`m_aSnapTypes`) costs <= 1, farther costs 1 + extra/50 |
| `m_fUrbanPlaceRadiusM` | 130 | a place wish is taken within this of the centre (0 = inside the village) |
| `m_fUrbanRingInnerM` / `m_fUrbanRingOuterM`, `m_iUrbanRingMinFree` | 12 / 45 / 5 | room check: of `m_iRingSamples` service-sized spots on this ring at least this many are clear (clearable objects count as clear) |
| `m_iUrbanMaxBuildings` | 150 | max buildings within `m_fTownRadiusM` (dense town centres stay out) |
| `m_iSiteSamples` / `m_iSiteValid` | 60 / 5 | candidates tried per HQ / valid ones scored before the best is taken |
| `m_fWeightRoad` / `m_fWeightPlace` / `m_fWeightRoom` | 1 / 1 / 1 | cost weights |
Score (lower = better) = road cost + place cost + room cost (8 * missing share of the ring) + density (0.03 per building over 40) + 4 if off the navmesh (unknown = 0) + 2 per metre of height range + 0.005 per metre moved from the wish.
Logs: `CLW: layout HQ <A|B> site score <s> at <pos> grid <g> (wish moved .., N valid of M samples, k settlements near the wish): road=.. m (cost ..) place='<name>' .. m (cost ..) room=a/b clear samples (cost ..) buildings=.. (cost ..) navmesh=.. clearable objects in the core=..`, per-site rejects in the existing `CLW: layout random rejections (...)` line as `site <reason>=n` (`site road too far`, `site core object-norpl`, `site service ring too small`, `site too many buildings`, ...).

**Clearing small obstacles** (`Configs/CLW/Placement/ClearRules.conf`, class `CLW_ClearRules`, loaded by path): an object under a service footprint no longer blocks the spot when it may be deleted; the objects under the FINAL pose are deleted (server only) just before the service is spawned.
| key | meaning |
|---|---|
| `m_bEnabled` | master switch (0 = nothing is cleared, only free spots are used) |
| `m_bRequireReplicated` | 1 (default): only entities with an `RplComponent` are cleared. A world object without it would be deleted on the server only and stay (visible + solid) on every client, so it keeps blocking (reject reason `object-norpl`) |
| `m_fMaxRadiusM`, `m_fMaxHeightM`, `m_iMaxPerSpot` | global bounding radius (3 m) and height (8 m) limit of a clearable object, max objects removed per footprint (60) |
| `m_aNeverPathContains`, `m_aNeverClassNames` | prefab path substrings / class names that are never cleared (houses, buildings, bridges, roads, power lines, vehicles ...) |
| `m_aRules` | allow-list: `m_sKind`, `m_bEnabled`, `m_bAnyTree` (every BaseTree/TreeEntity), `m_aPathContains`, `m_aClassNames`, `m_fMaxRadiusM` (0 = global). The first matching rule decides; an object matching no rule is never touched |
Always protected in code: characters, vehicles, `Building`, terrain, our own placed services (`CLW_PlaceClear.Protect`). Classification uses the ROOT entity of the queried object.
Logs: `CLW: place clear rules loaded: ..`, `CLW: place clear <kind> removed <n> objects: <prefab>, ... (<side> <base> <kind>)`; the place line gets `clear=<n>`.

**Depot**: `DEPOT_ROAD_M` 40 -> 60; in road mode the depot faces the road point (face target = road point), log `depot road search: ... facing the road`.
