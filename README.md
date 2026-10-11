# Clausewitz

AI High Command for Arma Reforger (built-in rule-based commander, optional LLM): AI vs AI wars with real fog of war, SITREPs, chain of command, resources and logistics, for infantry and mechanized forces, with support for custom factions (RHS, WCS, ...).

## Layout
- `addon/` - the Reforger addon (`addon.gproj`, GUID `C1A05E7B3D9F4A28`). Only this is packaged and published.
- `dev/` - never shipped.
  - `VISION.md` - the project vision, scope, features, roadmap (section 13)
  - `PROGRESS.md` - live milestone checklist and log
  - `notes/decision-game-mode.md` - why we use our own game mode, not Conflict
  - `research/feasibility-research.md` - what is known to be possible, sources, risks, first spikes

## Status
Vision, research and game-mode decision done; no code yet. Next: M0 skeleton (`CLW_GameMode`, perception + rule-based commander + waypoints), see `dev/PROGRESS.md`.

## Dependencies
None yet (base game only). RHS / WCS planned as optional compatibility.

## Publish
Not yet.
