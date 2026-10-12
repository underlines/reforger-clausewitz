# Changelog

All notable changes to Clausewitz. Format: [Keep a Changelog](https://keepachangelog.com/), versioning: [SemVer](https://semver.org/) (pre-1.0: MINOR = features, may break; PATCH = fixes).
Add a line under `[Unreleased]` with every user-visible change; `_tools/release.ps1` rolls it into a version.

## [Unreleased]

### Added
- LLM sidecar protocol v2 (`"v":2`): objectives, bases, war economy (tickets, supplies, caps), buy catalog and unit kinds in the situation; validated replies with `orders` (objective `target` or `pos`, commands MOVE/ATTACK/DEFEND/RECON/FALL_BACK), `buy` and mission `missions` offers for human-led squads. Protocol v1 keeps working.
- Sidecar wake triggers `objectiveChanged`, `ticketsDropPct`, `unitSpawned`, `missionEvent` (Configuration view); an objective owner change or a material war-number change is never "unchanged".
- Dashboard: objectives on the map (radius circle, owner colour, contested hatch/blink, progress ring), bases, per-faction war strip, unit kind / vehicle / mounted state, objectives and recent-events panels, purchases and mission offers in the exchange log.

### Changed
- Sidecar system prompt rewritten for protocol v2 (war doctrine, combined arms, field reference, reply schema); fake provider has a simple v2 doctrine (buy, attack nearest foreign objective, defend threatened, offers for human squads).
- Dashboard faction colours are derived from the faction keys instead of fixed US/USSR names.

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
- Situation JSON and world uploads are built by concatenation (`string.Format` cut them at 8 KB).
- Squads whose leader died no longer count as alive (corpse agents showed as 1/6 strength).
- Squads you lead always get an objective: if the LLM ignores a human-led squad, the rule commander issues its first mission.
- Commander voice and move marker work again: the player squad no longer gets a group ID assigned (a real ID made the controller/group IDs disagree). Missions are vanilla tasks assigned to the leading player (faction-owned), visible in the J menu and on the map.

## [0.1.0] - 2026-10-11

### Added
- M0 skeleton: game mode with US vs USSR, player spawn and squad join, per-faction contact book with map markers, rule-based commander (MOVE/ATTACK/DEFEND), vanilla commanding menu, missions as vanilla tasks for human-led squads, debug chat.

