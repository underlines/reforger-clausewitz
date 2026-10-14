# Loadouts and arsenals: implementation contract (2026-10-14)

Implements `loadouts-arsenal-plan.md` (R0-R6) in one sprint with four code-only work packages on disjoint files, then one DS test
session that answers the R0 questions and runs the test matrix. R0 is therefore built as instrumentation + switches + fallbacks inside the
packages instead of a separate spike session (every R0 question gets a log line that answers it).

## Deviations from the plan (decided 2026-10-14)
- **Save = the vanilla save action, hooked.** Every `ArsenalBox_Base.et` child (vanilla, RHS, WCS, ours) already has the vanilla "save
  loadout" action, which ends in `SCR_ArsenalManagerComponent.SetPlayerArsenalLoadout` (authority). A `modded` override stores our copy there
  (calls `super` first). So "save at any arsenal" works without touching third-party prefabs. Only **Load** is a new action
  (`CLW_LoadLoadoutAction`). Our HQ/FOB arsenals are vanilla/pack boxes chosen by faction data (there is no own arsenal prefab), so the
  action goes into an override of vanilla `ArsenalBox_Base.et` in our addon (vanilla content + one action), which every child box
  (vanilla, RHS, WCS) inherits unless it replaces the action list. R0.3 checks which boxes show it.
- **Game <-> sidecar via REST, game keeps a cache file.** The sidecar may not know the server's `$profile` path (other machine, other
  profile). So the game pulls the library from the sidecar at war start and writes `$profile:Clausewitz/loadouts.json` as its cache; without
  the sidecar it uses the cache. Player custom loadouts are game-owned (`$profile:Clausewitz/player-loadouts.json`) and pushed to the sidecar.
- **Restart via polling.** The game polls `GET /control` (cheap, every 15 s) instead of piggybacking on `/orders` (which may be idle when the
  LLM engine is off).

## Files and owners (work packages)
| WP | Scope | Owns (new unless marked) |
|----|-------|--------------------------|
| G1 | R0.1 logging, R2 role picker, R4 squad roles + AI kits | `CLW_RolePicker.c`, `CLW_SquadRoles.c`, `CLW_SquadCompositions.c`, `Configs/CLW/SquadCompositions.conf`; edits: `CLW_ForceSpawner.c`, `CLW_PlayerSquad.c` (spawn path only) |
| G2 | Kit core, R1 custom loadout, R6 kit validation | `CLW_Kits.c`, `CLW_CustomLoadouts.c`, `CLW_ArsenalSaveHook.c`, `CLW_LoadLoadoutAction.c`; prefab override of vanilla `ArsenalBox_Base.et` (only to add the Load action) |
| G3 | R3 server loadouts + spawn slots, library sync, restart, R6 messages, serverKitCost | `CLW_LoadoutLibrary.c`, `CLW_LoadoutSync.c`, `CLW_ServerLoadout.c`, `CLW_SideCustomLoadout.c`, `CLW_LoadoutSlots.c` (replication); edits: `CLW_LoadoutManager.et`, `CLW_WarMode_ShortConflict.c` (param + cap), one call at war start / restart in `CLW_WarRuntime.c` |
| S  | R5 login, loadouts tab, library store, compositions/pins, control | `dev/tools/auth.mjs`, `dev/tools/loadouts.mjs`, edits `llm_sidecar.mjs` (routes only), `dashboard/*`, `dev/tools/test-loadouts.mjs`, README section |

New stringtable keys: each WP lists them in its report (`KEY | EN | DE`); the lead adds them to `dev/i18n/CLW.keys.txt` at merge.
Log prefix for all game parts: `CLW: loadouts ...` (G2/G3), `CLW: squad roles ...` (G1).

## Script API between game packages (exact signatures; implement/consume as written)
```c
// G2, CLW_Kits.c (server-side helpers, static)
class CLW_Kits
{
	// Vanilla loadout string of a living character (same format the vanilla arsenal save produces); "" on failure.
	static string Capture(IEntity character);
	// Replace the character's whole gear with the kit (old gear deleted, not dropped). Server only. Items whose prefab does not load
	// are skipped and listed in 'missing'. tag = log context ("ai US-3 slot 2", "player 5 load").
	static bool Apply(IEntity character, string loadout, string tag, notnull array<string> missing);
	// Item prefabs referenced by a kit (for validation / dashboard); unique, in kit order.
	static void Items(string loadout, notnull array<string> items);
	// R6: status of a kit for a faction: "ok" | "degraded" | "invalid"; missing = prefabs that do not load or are outside the faction's
	// current arsenal pool (CLW_ArsenalPool). degraded = something missing but a primary weapon survives; invalid = unparsable / nothing left.
	static string Validate(string loadout, string factionKey, notnull array<string> missing);
	// Supply cost of a kit at an arsenal (vanilla per-item cost), -1 if unknown.
	static int Cost(string loadout, string factionKey);
}

// G2, CLW_CustomLoadouts.c (server)
class CLW_CustomLoadoutRec   // one player's saved kit for one faction (also the JSON record, see below)
{
	string identityId; string playerName; string faction; string loadout; string basePrefab;
	ref array<string> items; ref array<string> addons; string gameVersion; int savedAt; string status; ref array<string> missing;
}
class CLW_CustomLoadouts
{
	static CLW_CustomLoadoutRec Get(string identityId, string factionKey);   // null if none
	static void All(notnull array<ref CLW_CustomLoadoutRec> outList);         // every record (for the sidecar sync)
	static string IdentityOf(int playerId);                                   // BackendApi id, fallback "n:<name>" (like CLW_PlayerLang.KeyOf)
}

// G3, CLW_LoadoutLibrary.c (server)
class CLW_LoadoutLibrary
{
	// Kit for an AI member of role R (ROLE_* label name, e.g. "ROLE_MEDIC") in a squad of kind 'kind' (CLW_ForceKind string) of
	// faction 'factionKey'. 'slot' = member index (0 = leader) for pinned lists. Pinned kits win, then active library kits with role R
	// (random among several, status ok/degraded only). Returns "" when none (caller falls back to the R2 role prefab).
	static string PickKitForRole(string factionKey, string kind, string role, int slot);
	// Composition override from the dashboard for a kind ("" entries = none): returns false when the sidecar set none.
	static bool CompositionOverride(string kind, notnull array<string> roles);
}

// G1, CLW_RolePicker.c (server + client safe, deterministic from catalogs)
class CLW_RolePicker
{
	// One CHARACTER catalog prefab per ROLE_* for the faction (kin factions fill missing roles); "" if none.
	static ResourceName RolePrefab(string factionKey, string role);
	// Roles the faction can field, in the default slot order (leader, rifleman, machinegunner, antitank, grenadier, medic, sharpshooter, ...).
	static void Roles(string factionKey, notnull array<string> roles);
	// Display name of the role prefab (UI info) for spawn-slot names.
	static string RoleName(string factionKey, string role);
}
```
G2 must not depend on G1/G3; G3 depends on G1 (RolePicker) and G2; G1 depends on G3 (`PickKitForRole`, `CompositionOverride`) and G2
(`Apply`). Compile happens after the merge.

## Sidecar REST contract (game <-> sidecar)
All game endpoints accept header `X-CLW-Token`; the sidecar enforces it only when `gameToken` is set in `sidecar-config.json`. The game
reads the token from `$profile:Clausewitz/overrides.json` key `sidecarToken` (absent = send none).
- `POST /loadouts/player` (game, on every save): body = one custom loadout record. Reply `{ok:true}`.
- `POST /loadouts/sync` (game, at war start and after a restart): body
  `{v:1, round, gameVersion, addons:[{guid,name}], factions:[{key,name,side}], players:[record...]}`.
  Reply `{v:1, library:[entry...], compositions:{<kind>:[ROLE_*...]}, pins:{<faction>:{<kind>:[libId...]}}}`.
  Game writes the reply to `$profile:Clausewitz/loadouts.json` (same shape) and uses it; on error/timeout it reads that file.
- `POST /loadouts/status` (game, after validating at war start): `{v:1, library:[{id,status,missing:[...]}], players:[{identityId,faction,status,missing}]}`.
- `GET /control` (game, every 15 s): `{restartId:<int>}`. The game remembers the first value it sees; a later, higher value = restart the war
  (`CLW_WarRuntime.RematchRestart` path), which re-syncs the library. Reply 0 when none was ever requested.

Records:
- custom loadout: `{identityId, playerName, faction, loadout, basePrefab, items:[prefab], addons:[guid], gameVersion, savedAt, status, missing:[prefab]}`
- library entry: `{id, name, role, faction, active, order, loadout, basePrefab, items, addons, gameVersion, createdFrom:{identityId, playerName}, status, missing}`
  (max 7 `active` per faction, enforced by the sidecar; `order` 1..7 = spawn slot order).
- Big strings: loadout strings can exceed 8191 chars. Game code builds JSON with `+` only, never `string.Format`/`Substring` on them (see
  `reforger-script-dev`), and parses with `SCR_JsonLoadContext` DTOs.

## R0 answers expected from the test session (log lines)
- R0.1 `CLW: squad roles slots <unitId> <groupPrefab> [<prefab>:<ROLE_...>, ...]` (before roles are changed).
- R0.2 `CLW: squad roles kit <unitId> m<idx> <role> applied=<0|1> missing=N primary=<prefab|none> mags=N` + a delayed (10 s) check
  `CLW: squad roles kitcheck <unitId> m<idx> weapon=<prefab> ammo=N`. Switch `squadKits` in overrides.json (`on` default | `off`).
- R0.3 manual: which arsenal boxes (vanilla / RHS / WCS) show `CLW_LoadLoadoutAction`, and how do WCS/Bacon actions look next to it.
- R0.4 `CLW: loadouts slot <side> <idx> faction=<key> name=<..> source=<library|role|hidden>` on server and client.
- R0.5 after a dashboard restart: client spawn screen shows the new slot names without reconnect?

## Item details (WP7, 2026-10)
Optional record fields (game `CLW_ItemInfo.c` -> `POST /loadouts/player` and `/loadouts/sync` players, never in `player-loadouts.json`):
`totalCost` (int, kit supply cost, -1 unknown) and `itemInfo: [{id, name, key?, path?, type, cost, count}]` (`id` = `{GUID}` prefab, `name` translated UI
name or "", `key` = raw localization key when untranslated, `type` = rifle|sniper|machinegun|launcher|pistol|weapon|magazine|attachment|headwear|torso|
legs|footwear|handwear|clothing|vest|backpack|equipment|medical|throwable|explosive|vehicle|other, `cost` per unit, `count` occurrences).
Sidecar: cleans leniently, copies to library entries, strips from the sync reply, list view carries `summary` only,
`GET /api/loadouts/(library|player)/<id>/items` returns the full list. Old records without the fields work everywhere.
