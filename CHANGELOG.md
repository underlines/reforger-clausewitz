# Changelog

All notable changes to Clausewitz. Format: [Keep a Changelog](https://keepachangelog.com/), versioning: [SemVer](https://semver.org/) (pre-1.0: MINOR = features, may break; PATCH = fixes).
Add a line under `[Unreleased]` with every user-visible change; `_tools/release.ps1` rolls it into a version.

## [Unreleased]

### Added
- M3 phase B, base services: every side gets its service compositions at its HQ at war start (command post, supply cache, arsenal, vehicle depot, medical), positioned on free spots around the base and listed per base for later requests/FOBs. Prefabs come from the faction profile data (`m_aServices`, US, USSR and FIA ship them); a faction without services uses its kin faction (`m_sServiceFallbackFaction` or a friendly faction with services), so RHS/WCS/GRS sides get a base too. Log lines `CLW: services ...`.
- Supply link: the base supply containers mirror the side supplies; arsenal kits spend the same supplies as purchases (changes in both directions are synced every 1.5 s).
- Arsenal kin pool: during a Clausewitz war each faction's arsenal also lists the items of factions friendly to it (pack factions list the vanilla faction they are built on), never the enemy side; saved kits accept friendly items.
- Setup menu: "Logistics" and "Kit cost" switches (kit cost off = arsenal kits are free).
- M3 phase R, action registry: one registry (`CLW_ActionRegistry`, data in `Configs/CLW/Actions/Actions.conf`, executors as script classes) behind every HQ command. The LLM orders (MOVE/ATTACK/DEFEND/RECON/FALL_BACK), purchases and mission offers are validated and executed through it (no behaviour change). War modes declare capabilities (`CLW_Capabilities.Has`, switchable with `cap_<name>` mode params); registry entries whose capabilities are off drop out. Protocol v3: the game sends its action manifest to the sidecar (`POST /manifest`, hash in every situation), the sidecar builds the prompt command list, the validator and the fake provider from it and falls back to a built-in manifest (v2 posts keep working); the dashboard shows the manifest in the HQ view data (`/api/state` `manifests`).

### Changed
- Debug lines (spawns, orders, missions, contacts, point captures, ...) no longer appear in players' chat by default; they stay in the log and are sent to the sidecar, whose dashboard has a new "Debug log" tab. Set `"debugChat": true` in `$profile:Clausewitz/overrides.json` to show them in chat again. War-over and hold-all announcements stay visible.

### Fixed
- Purchases are refused up front (before supplies are spent) when the engine active-AI limit has no room for the unit (headroom = limit - active - margin 6 - AI of purchases in progress); the situation carries `war.engineAI {active, limit, headroom}`, the rule commander and the fake LLM provider skip buys without headroom, the LLM prompt says so, and starting a war logs a warning when both AI caps together exceed the limit.
- HQ purchases no longer fail with "no members arrived" once about 90 AI are alive: purchased groups are spawned with CRITICAL importance (the engine admits NORMAL groups only below 70 % of the active-AI limit and silently drops the request otherwise), an empty group re-requests its members twice before the purchase is refunded, and spawn logs show the active AI count. New groups are also exempt from the activity budget for 60 s.

## [0.4.0] - 2026-10-12

### Added
- Faction SITREP for players: one briefing per side (BLUF headline, Situation, Intel, Mission, Execution, Logistics) in natural NATO/US military prose (sizes and times in words, objective or nearest map place plus a 100 m grid matching the map labels, no unit ids or coordinates). Built in game every 20 s from that side's own picture (works without the sidecar); the LLM version rides in the commander reply (no extra call) and replaces it while fresh. Only the own side receives it: map journal on the deploy screen and in-game map (opens on first map view), hint on every spawn (H shows it again), `/sitrep` chat command.
- Battle size scales the battlefield: small battles place the HQs at half the authored distance (`m_aDistanceScales`, minimum `m_fMinHqDistanceM`), objectives declare the smallest battle size they appear in (`m_iMinBattleSize`) and are re-spaced with a hard HQ clearance (`m_fHqObjectiveClearM`) and a minimum gap between objectives (`m_fObjectiveGapM`); the spawn point and players at the old HQ move along. Hook for future FOB placement.
- Dashboard: faction-vs-faction comparison tables (game mode rules and live numbers, forces), SITREP card per side, forces table with a detail row, missions page with a mini map (click a squad or offer to focus its target), map shading and filter for active vs dormant units (hover card shows the activity). Situation JSON reports `dormant` per squad.

### Changed
- Enemy contact markers show size words ("squad", "veh section + fire team") instead of a raw count.
- Mission panels on the dashboard explain that HQ only offers missions to player-led squads.

### Fixed
- Enemy contact counts: destroyed and disarmed targets and vehicle crews no longer inflate contacts (a side with one vehicle was reported as "75 vehicles"); vehicles and infantry are counted separately.

## [0.3.0] - 2026-10-12

### Added
- Ticket counter HUD for all players (`CLW_WarStatus` server broadcast every 2 s and on changes, `CLW_WarHud` client panel, `UI/layouts/CLW/WarStatusHud.layout`): per side the localised faction name in the side colour (map identity: BLUFOR blue, OPFOR red, INDFOR green), tickets (red below 25 %, "-" without tickets), objectives held (count and value), time left, "Setup..." in SETUP and the winner after the end; hidden while the map is open.
- Situation v2 carries `side` / `identity` (and `war.sides`, side A first); the sidecar exposes them per faction in `/api/state` and sorts `activeFactions` by side; the dashboard orders factions side A first and colours them by identity, and the Overview objective bar is laid out side A | neutral | side B with contested value hatched in the owner segment.
- Squad join service: HQ infantry squads appear in the vanilla deploy menu and groups menu (named "<id> - <type>", up to 4 humans, no "new group", vanilla preset groups removed), mirrored to connected clients over an RPC. Every entry point (deploy menu pick, groups menu pick, auto-join fallback) ends in one server service (`CLW_PlayerSquad.Establish/Join/Release`) driven by the vanilla group events; the last human leaving gives the AI back to HQ. Picking a squad more than 500 m away shows a distance/direction popup instead of teleporting. Mission board follows leader changes.
- `CLW_SpawnLogic`: no deploy menu during SETUP (auto-spawn at the base, setup dialog unaffected); after the war starts the vanilla deploy menu (sides, loadouts, base spawn points, HQ squads) on spawn and respawn.
- Mission offers for player squad leaders (`CLW_MissionBoard`): HQ offers 2-3 tasks (Capture / Defend / Recon / Destroy) in the J task list and on the map, visible only to the leader; accepting one removes the others, abandoning replans after 60 s; completion is detected automatically (+60 supplies, +100 for a major point), 15 min timeout, popups with sounds for offer / accept / complete / fail, LLM offers used when fresh.
- Anti-armour infantry is data now: `m_bAntiArmor` on force entries (set on the AT teams of the vanilla profiles, detected from the `ROLE_ANTITANK` member label in auto-discovery) replaces the name matching in the rule commander.
- Faction choice for both sides: the setup menu's Side A / Side B pick any playable faction of the world, including factions added by mod packs (e.g. RHS) with no dependency on them. At war start the sides are re-keyed (bases, spawn points, control points, ledgers, squad registry, force catalogs), the two factions become playable and hostile, players join side A by default with a spawnable loadout, and scene groups of other factions are removed (`m_bRemoveUnusedSceneGroups`). The Arland world now uses `CLW_FactionManager` / `CLW_LoadoutManager` (children of the vanilla editor managers).
- Faction force profiles (`Configs/CLW/Factions/US|USSR|FIA.conf`, class `CLW_FactionProfile`) plus catalog auto-discovery: factions without a profile, or profiles with `m_bIncludeDiscovered`, get buyable forces classified from the faction entity catalogs (mod-pack vehicles such as tanks show up as kind `armor`); the catalog is built at war start per faction key.
- Force purchases and start forces (`CLW_ForceSpawner`): supplies, AI and vehicle caps, crewed vehicles with embarked infantry, refund on failure; Arland starts both sides with a motorized and a mechanized unit.
- Mechanized / motorized vehicle units (`CLW_VehicleUnit`): mount, dismount short of the target, overwatch, trucks return to base and are removed (partial refund), remount with timeout and split, stuck watchdog (re-issue, then infantry walks).
- War setup menu: the first player to join opens a generated dialog (Battle group with the two factions, War mode group with the mode selector and its parameters as sliders / spin boxes / checkboxes, Start war / Defaults); other players see "Waiting for <name>"; owner handover when the owner leaves; chat fallback `/war setup` and `/war start [mode] [key=value ...]`; auto start after 180 s.
- Control points: own capture logic (progress, contested, neutralise-then-capture), public map markers for points and bases.
- War state with pluggable war modes: "short conflict" (tickets, bleed for the side holding fewer points, win by tickets / holding all points / time limit). The war waits in a SETUP phase until started (auto start after N s from the scenario).
- Supplies ledger with income from held points (TrySpend / Refund / AddSupplies API for later phases).
- Bases and respawn: per-faction spawn point spawned at the base position, scene spawn points of the scenario factions disabled; respawn gated by tickets.
- Scenario config data (`Configs/CLW/Scenarios/Arland.conf`): factions, bases, objectives, economy and war mode defaults; no hard-wired factions or coordinates in code.
- LLM sidecar protocol v2 (`"v":2`): objectives, bases, war economy (tickets, supplies, caps), buy catalog and unit kinds in the situation; validated replies with `orders` (objective `target` or `pos`, commands MOVE/ATTACK/DEFEND/RECON/FALL_BACK), `buy` and mission `missions` offers for human-led squads. Protocol v1 keeps working.
- Sidecar wake triggers `objectiveChanged`, `ticketsDropPct`, `unitSpawned`, `missionEvent` (Configuration view); an objective owner change or a material war-number change is never "unchanged".
- Dashboard: objectives on the map (radius circle, owner colour, contested hatch/blink, progress ring), bases, per-faction war strip, unit kind / vehicle / mounted state, objectives and recent-events panels, purchases and mission offers in the exchange log.
- Activity management (`CLW_Activity`): AI far from players no longer goes dormant. Every 10 s each commanded unit is scored for relevance (enemy within 800 m, active order / moving vehicle unit, contested objective, human-led) and the best ones within a per-side budget (`m_aActiveAgentCaps` of the war mode, 40/70/110 on Arland) get `PreventMaxLOD`; idle units are released after 60 s. Switch `m_bActivityManagement` in the scenario; a 60 s log line per faction shows relevant units, prevented agents and agents at max LOD.
- Vehicle unstuck escalation: re-issue (40 s), teleport toward the target onto a free road spot when no human is within 400 m (80 s), then despawn with 50 % vehicle refund (no human within 500 m) or dismount everybody and delete the empty vehicle later (160 s). Thresholds in `CLW_WarConfig`.
- Server overrides file `$profile:Clausewitz/overrides.json` (example written on first start) and scenario `m_aOverrides` to fix catalog kind / cost / weight / name / enabled / anti-armour without editing packed configs (`dev/notes/overrides.md`).
- Player loadouts: `CLW_LoadoutManager` now lists the vanilla US and USSR role loadouts (rifleman, AR, MG, GL, AT, sniper / sharpshooter).

### Changed
- Sidecar system prompt rewritten for protocol v2 (war doctrine, combined arms, field reference, reply schema); fake provider has a simple v2 doctrine (buy, attack nearest foreign objective, defend threatened, offers for human squads).
- Dashboard faction colours are derived from the faction keys instead of fixed US/USSR names.
- Dashboard split into views (Overview, Map, Forces, HQ / LLM, Missions, Usage & Config; selected tab remembered, deep link via `#tab`); factions of a previous war or setup phase are hidden (`activeFactions` in `/api/state`, toggle "show inactive factions").
- Catalog classifier: an armed `VEHICLE_CAR` (even armoured, e.g. M1025 M2) is `light`; `mech` needs an APC / armour label and at least 3 cargo seats; an APC whose UI info reports 0 passengers is re-counted with a spawn probe.
- Empty or dead groups get their waypoints cleared (avoids the vanilla `SCR_AIProcessFailedMovementResult` exception on a null leader).

## [0.2.0] - 2026-10-12

### Added
- Dashboard map: building footprints (cached vector layer, toggle `buildings`) and base / airport / port symbols with bold labels (toggle `objects`), including vanilla military bases with faction.
- LLM bridge: HQ commanders send the contact picture to a local sidecar (`dev/tools/llm_sidecar.mjs`) and apply the validated orders; falls back to the rule commander on errors or empty replies.
- Sidecar providers: fake, Anthropic, OpenAI-compatible.
- OpenRouter provider with usage monitoring and spending-pace estimate (over/under by reset): per-request cost, session totals by model and faction, key spending-limit bar in the Configuration view, 80%/95% warnings, and an optional automatic stop of LLM calls at the limit (falls back to the rule commander).
- Web panel at http://127.0.0.1:8788/ : HQ view (map, squads table, SITREP, exchange log) and configuration view.
- More squad data in the situation (type, wounded, health, speed, heading, state, objective).
- Sidecar cost control: `minIntervalSec` and `skipUnchanged` settings; skipped cycles keep current orders. 20 s provider cooldown after HTTP 429/5xx.
- Adaptive HQ wakeup: the game polls every 10 s, the sidecar calls the LLM only when due (model-chosen `next_check_sec`), on triggers (new contact, casualties, squad lost, objective reached, ...) or via "wake now" in the dashboard; configurable under Wakeup/Triggers.
- Dashboard: pause button (no LLM calls, game keeps current orders) and HQ engine selector (`llm` / `rules`; `model` prepared for a future decision-model tier) next to "wake now"; new `engine` setting.
- Player squads now follow the vanilla model (real group ID, controller join, slave group for the AI): radial-menu orders move the AI, with voice and map marker.
- Dashboard map: real map background read from your own game install (never redistributed; override `dev/tools/maps/<World>.png` + `.json` for modded terrains), plus road and place-name overlay sent by the game (`CLW_WorldInfo`), with layer toggles and image opacity.

- Dashboard map: heading/speed arrow per moving squad; objective arrows also for rule-commander and human-led squads (not only LLM orders).
- HQ picks up AI groups added at runtime (e.g. from Game Master) via a squad registry that sweeps every 5 s; opt a group out with the entity-name prefix `CLW_IGNORE_`.

### Fixed
- Vehicle crews of pack factions (default occupants are vanilla US/USSR crew) are re-affiliated to the buying faction (group, characters, vehicle), so the registry adopts them and the vehicles get orders.
- Cargo seating: soldiers that did not end up in the vehicle are retried with an explicit free cargo slot (teleport), then get a Get-In waypoint; verbose per-member diagnostics replaced by one summary line.
- Catalog probes no longer cause delayed-init VM exceptions: seats come from the editable-entity UI info when available, otherwise a probe vehicle is spawned high above the base, inactive, and deleted after 3 s; probe results cached per prefab; the catalog is built once at war start.
- Discovered force costs are normalized (11 per soldier, vehicle base by kind) instead of the inconsistent pack supply costs; classifier: tanks are `armor` regardless of rider seats, armed trucks are `motorized`, `mech` needs armour or APC; decisive labels are logged.
- Players without a squad (faction switch, start forces not there yet) retry for 2 min and then get a free infantry squad; the player stays alone during SETUP and joins the first infantry squad at war start.
- Rule commander stops spamming refused buys: after an AI-cap refusal no buys until the AI count drops or 60 s passed.
- Placement: unknown/unreliable navmesh answers (-1) no longer decide base choice (roads do); scenario vehicle slots only need to be unoccupied; Arland start forces are 2 infantry + motorized + mech per side.
- Situation JSON and world uploads are built by concatenation (`string.Format` cut them at 8 KB).
- Squads whose leader died no longer count as alive (corpse agents showed as 1/6 strength).
- Squads you lead always get an objective: if the LLM ignores a human-led squad, the rule commander issues its first mission.
- Commander voice and move marker work again: the player squad no longer gets a group ID assigned (a real ID made the controller/group IDs disagree). Missions are vanilla tasks assigned to the leading player (faction-owned), visible in the J menu and on the map.

## [0.1.0] - 2026-10-11

### Added
- M0 skeleton: game mode with US vs USSR, player spawn and squad join, per-faction contact book with map markers, rule-based commander (MOVE/ATTACK/DEFEND), vanilla commanding menu, missions as vanilla tasks for human-led squads, debug chat.

