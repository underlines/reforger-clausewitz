# Changelog

All notable changes to Clausewitz. Format: [Keep a Changelog](https://keepachangelog.com/), versioning: [SemVer](https://semver.org/) (pre-1.0: MINOR = features, may break; PATCH = fixes).
Add a line under `[Unreleased]` with every user-visible change; `_tools/release.ps1` rolls it into a version.

## [Unreleased]

### Added
- LLM bridge: HQ commanders send the contact picture to a local sidecar (`dev/tools/llm_sidecar.mjs`) and apply the validated orders; falls back to the rule commander on errors or empty replies.
- Sidecar providers: fake, Anthropic, OpenAI-compatible.
- OpenRouter provider with usage monitoring and spending-pace estimate (over/under by reset): per-request cost, session totals by model and faction, key spending-limit bar in the Configuration view, 80%/95% warnings, and an optional automatic stop of LLM calls at the limit (falls back to the rule commander).
- Web panel at http://127.0.0.1:8788/ : HQ view (map, squads table, SITREP, exchange log) and configuration view.
- More squad data in the situation (type, wounded, health, speed, heading, state, objective).
- Sidecar cost control: `minIntervalSec` and `skipUnchanged` settings; skipped cycles keep current orders. 20 s provider cooldown after HTTP 429/5xx.
- Player squads now follow the vanilla model (real group ID, controller join, slave group for the AI): radial-menu orders move the AI, with voice and map marker.

### Fixed
- Squads whose leader died no longer count as alive (corpse agents showed as 1/6 strength).
- Squads you lead always get an objective: if the LLM ignores a human-led squad, the rule commander issues its first mission.
- Commander voice and move marker work again: the player squad no longer gets a group ID assigned (a real ID made the controller/group IDs disagree). Missions are vanilla tasks assigned to the leading player (faction-owned), visible in the J menu and on the map.

## [0.1.0] - 2026-10-11

### Added
- M0 skeleton: game mode with US vs USSR, player spawn and squad join, per-faction contact book with map markers, rule-based commander (MOVE/ATTACK/DEFEND), vanilla commanding menu, missions as vanilla tasks for human-led squads, debug chat.
