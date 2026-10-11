# Decision: own game mode, not vanilla Conflict (2026-10-11)

**Decision:** Clausewitz ships its own game mode (`CLW_GameMode`, extends `SCR_BaseGameMode`) with its own minimal scenario world. Conflict is a reference and a parts bin, not a base.

## What Conflict (`SCR_GameModeCampaign`) would give us for free
- Military bases with capture, radio/supply links, spawn points
- Supply economy: `SCR_ResourceComponent`, depots, trucks, `SCR_SuppliesTransferWaypoint`
- Map markers, base UI, HQ commander role, vanilla AI faction commander slot (`SCR_BaseAIFactionCommander`)
- Ready-made scenario worlds and the MCP's `scenario_create_conflict`

## Why that does not outweigh the cost
- It is **player-centric**: loadouts, ranks/XP, respawn, base capture driven by player presence. We need a faction-vs-faction AI war that runs with zero or one spectating player.
- **Override vs augment fights over the same groups** (research, lessons from prior art): vanilla Conflict logic and our commander would both order the same AI.
- Large coupled surface (campaign manager, saves, rank, HUD): every game update can break our `modded` hooks, and replication quirks come with it.
- Our economy is a per-faction **ledger** (VISION section 6), simpler and safer than hooking Conflict internals; we can map it to real containers where cheap.
- Clean fog-of-war and "scenario setup screen" flow need to own the game-mode lifecycle.

## How we still reuse Conflict
- Components are reusable without the mode: `SCR_ResourceComponent` and containers, `SCR_MapMarkerManagerComponent`, `SCR_AIGroup` rally points, military-base components. Study and attach them from our mode (M3).
- Read `SCR_BaseAIFactionCommander` / `SCR_FactionCommanderHandlerComponent` as the slot design to mirror.
- Prefab and faction data (catalogs) are read at runtime, so any faction works.
- Later option (post-M5): an "augment Conflict" adapter that runs our commander inside a Conflict scenario, once the core is stable.

## Revisit if
The M0 spike shows base capture/spawning must be vanilla to get working AI behaviour, or M3 logistics proves far cheaper through Conflict's economy than through our ledger.

## Addendum: human players are central (2026-10-11)
Players join either side as soldiers or squad leaders (VISION section 2a). That raises the cost of owning the game mode: faction join, spawn/loadout, respawn and group join/leave are now our job in M0. The decision still stands because these come from base-game components, not Conflict-specific code `[verify in M0]`: `SCR_BaseGameMode`, respawn system and loadout manager, `SCR_GroupsManagerComponent`. Lighter vanilla modes (e.g. Capture & Hold) are better references for the player flow than Conflict. **Explicit revisit trigger:** if the M0 player-join/spawn path takes much longer than ~1 week or needs heavy Conflict internals, reconsider extending Conflict (augment mode).
