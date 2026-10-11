# Changelog

All notable changes to Clausewitz. Format: [Keep a Changelog](https://keepachangelog.com/), versioning: [SemVer](https://semver.org/) (pre-1.0: MINOR = features, may break; PATCH = fixes).
Add a line under `[Unreleased]` with every user-visible change; `_tools/release.ps1` rolls it into a version.

## [Unreleased]

### Added
- LLM bridge: HQ commanders send the contact picture to a local sidecar (`dev/tools/llm_sidecar.mjs`) and apply the validated orders; falls back to the rule commander on errors or empty replies.
- Sidecar providers: fake, Anthropic, OpenAI-compatible.
- Web panel at http://127.0.0.1:8788/ : HQ view (map, squads table, SITREP, exchange log) and configuration view.
- More squad data in the situation (type, wounded, health, speed, heading, state, objective).

## [0.1.0] - 2026-10-11

### Added
- M0 skeleton: game mode with US vs USSR, player spawn and squad join, per-faction contact book with map markers, rule-based commander (MOVE/ATTACK/DEFEND), vanilla commanding menu, missions as vanilla tasks for human-led squads, debug chat.
