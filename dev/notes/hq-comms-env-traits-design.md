# HQ comms, radial menu, weather/time, HQ traits: design

Owner request 2026-10-14. Branch `feature/hq-comms-env-traits`. Implemented code-only by Sonnet subagents in work packages (WP)
with disjoint file ownership; one compile + dedicated-server e2e at the end (lead).

## 1a. Natural language reaches the fixed commands
`/hq <text>` already runs player registry actions (authority `player`) chosen by the sidecar agent turn (`agent/agentTurn.mjs`,
tools = player entries of the manifest). Reports and the new help call were not registry actions, so `/hq 2 enemy infantry about
100m north of my position` could not become a report. New player registry entries (tag `request`, channel `requests`):

| Action | Params (all strings on the wire) | Meaning |
|---|---|---|
| `REPORT_CONTACT` | `kind`* inf\|veh\|armor\|air\|unknown, `count` (default 1), `distance`* metres, `direction` N NE E SE S SW W NW \| bearing 0-359 \| `ahead` (default ahead = the reporter's view direction, server side) | same result as `/report` |
| `REPORT_CANCEL` | none | removes the player's last report (its contact) if it still exists |
| `REQUEST_BACKUP` | `note` (optional, what is happening) | help call: urgent, any role (not only leaders) |

Chat: `/report <count> <kind> <dist> <dir>` (existing; dir also `ahead` or a bearing), `/report cancel`, new `/backup [note]`.

### /backup (help call)
Any player (soldier or leader), cooldown 120 s per player. Server:
1. orders the nearest free AI group to the player's position (same selection as `REQUEST_REINFORCE`, command DEFEND), if any;
2. records a help call (player, squad, pos, grid, note, time, answeredBy squad id or "") kept 10 min;
3. pushes a war event to the faction ("HELP CALL: <player> at <grid> needs backup: <note>") so the LLM commander wakes early;
4. answers the player (granted with the squad and distance, or "no free group, HQ informed").
Situation JSON gets `helpCalls[]` (see contracts). The LLM commander prompt says to answer open help calls first.

## 1b. Radial HQ menu
Vanilla radial menu (`SCR_RadialMenu` + `SCR_RadialMenuController` + `SCR_SelectionMenuCategoryEntry` for levels) on its own input
action `CLW_HqRadial` (default key to verify as free, prefer `U`; gamepad optional). Quick access only; the full dialog (`/hqmenu`)
stays and is one entry of the radial. Layout (levels):

- **Report contact** -> what (Infantry, Vehicle, Armor, Air, Unknown) -> count (1, 2, 3, 4, 6, 8, 12, 20) -> distance (50, 100, 200,
  300, 500, 800, 1200 m) -> direction (Ahead = camera heading sent as a bearing, N, NE, E, SE, S, SW, W, NW) -> sends
- **Cancel my last report**
- **Backup!** (help call)
- **Requests** -> Vehicle -> vehicle types (from the HQ menu payload) | Resupply | Squad (leader) | Reinforce (leader) | Mission | Status
- **SITREP** (show the last one)
- **Full HQ menu** (opens the dialog)

Client -> server through the existing HQ menu action RPC (`CLW_HqMenuAction(action, arg, vector.Zero)`):
`req` arg `<kind>|<type>` (existing), `report` arg `/report` syntax (existing; dir may be a bearing), `report_cancel`, `backup` arg note.
Dynamic entries (vehicle types, costs, role) come from the HQ menu payload: the radial asks with action `refresh` when it opens and
caches the parsed `CLW_HqMenuData`.

## 2. Weather and time
Setup params (all war modes that call `CLW_Environment.AddParams`):
- `daySpeed` choice: 0x 1x 4x 8x 16x 24x 48x 96x (1x = 24 h real per game day; 0x = time stands still)
- `startHour` number 0-23.5 step 0.5 (default 8), `startYear` 1960-2040, `startMonth` 1-12, `startDay` 1-31 (defaults: world's)
- `weatherMode` choice: `slots` (default) | `world` (do not touch the weather: the world's own automatic weather)
- `weather1` .. `weather6` choice: `random` + the world's weather states (discovered at runtime from the weather manager, fallback
  list if discovery fails), default `random`. Static weather = six times the same state.
- Equal progression: the war time limit is split into 6 equal slots; at each slot border the weather transitions (over ~10 % of the slot)
  to the next slot's state; random slots are rolled at war start (logged). After slot 6 the weather stays.
Server applies at war start (`TimeAndWeatherManagerEntity`: date, time, day duration, forced weather states), logs `CLW: env ...`.
HQ updates: war events to both factions at weather changes ("Weather changing to Rainy") and at dawn / dusk; situation block `env`
(see contracts); rules SITREP gets one weather/time line; the LLM prompts get a short environment paragraph.

## 3. HQ character traits
Setup params `hqTraitA`, `hqTraitB` (per side, A/B = `factionA` / `factionB`): random, aggressive, defensive, methodical, cautious,
opportunistic, crazy. Random rolled at war start (logged, never told to the enemy side). Effects:
- **LLM commander**: personality paragraph (decision bias + communication style) in the commander system prompt and the SITREP tone;
- **HQ radio** (`/hq` answers): communication style line;
- **rule commander** (no LLM): tuning multipliers (attack radius, max attack/defend units, threat radius, buy priority / reserve);
- own side's first SITREP / war start hint names the commander's character.

## Contracts (fixed, every WP codes against these)
Game, server-side static helpers (the lead hooks them into `CLW_Situation.Build`, the war mode constructor, the rules SITREP):
- `CLW_Backup.GetJson(string faction)` -> `""` or JSON array `[{"id":1,"player":"..","squad":"..","grid":"025-285","pos":[x,z],"note":"..","ageSec":40,"answeredBy":"..."}]` (open calls, newest first, max 5) -> situation key `helpCalls`
- `CLW_Environment.AddParams(notnull array<ref CLW_ModeParam> params)`; `CLW_Environment.OnWarStart()`; `CLW_Environment.Stop()`;
  `CLW_Environment.GetJson()` -> `""` or object `{"time":"14:30","date":"1989-06-21","daySpeed":4,"daylight":"day|dusk|night|dawn","sunrise":"05:12","sunset":"21:03","weather":"Rainy","next":"Overcast","nextInMin":12,"plan":["Clear","Cloudy","Rainy","Overcast","Clear","Clear"]}` -> situation key `env`;
  `CLW_Environment.SitrepLine()` -> English one-liner or "" (rules SITREP)
- `CLW_HqPersona.AddParams(notnull array<ref CLW_ModeParam> params)`; `CLW_HqPersona.OnWarStart()`; `CLW_HqPersona.Trait(string faction)` -> trait key or "";
  `CLW_HqPersona.GetJson(string faction)` -> `""` or `{"trait":"aggressive"}` -> situation key `hq`
Sidecar:
- `agent/environment.mjs`: `export function envPrompt(sit)` -> "" or a short ENVIRONMENT paragraph for any HQ prompt (time, daylight, weather, next change, tactical hints for night/rain/fog)
- `agent/personality.mjs`: `export const TRAITS`; `personaCommander(sit)`, `personaRadio(sit)`, `personaSitrep(sit)` -> "" when `sit.hq?.trait` is missing/unknown
Strings: each WP writes its new keys to `dev/i18n/fragments/<wp>.keys.txt` (same `KEY ::: EN ::: DE` format); the lead merges them into
`CLW.keys.txt` and regenerates.

## Work packages
| WP | Scope | Owns (only these files) |
|---|---|---|
| WP1 comms | REPORT_CONTACT/REPORT_CANCEL/REQUEST_BACKUP, `/report cancel`, dir `ahead`/bearing, `/backup`, help calls, HQ menu server actions, dialog buttons | `CLW_Reports.c`, `CLW_ContactBook.c`, new `CLW_Backup.c`, new `CLW_ActionsComms.c`, `CLW_ActionsRequests.c` (Register only), `CLW_RequestPlayerController.c`, `CLW_HqMenuServer.c`, `CLW_HqMenuDialog.c`, fragments `wp1` |
| WP2 radial | radial menu, input action + key, payload cache | new `CLW_HqRadial*.c`, new configs under `Configs/CLW/UI/` and the input config override, `CLW_HqMenuClient.c`, fragments `wp2` |
| WP3 env | weather/time params + runtime + JSON + events | new `CLW_Environment.c`, new `agent/environment.mjs` (+ test), fragments `wp3` |
| WP4 persona | traits params, rule tuning, commander/SITREP prompts, help-call + env text in the commander prompt | new `CLW_HqPersona.c`, `CLW_RuleCommander.c`, `llm_sidecar.mjs`, new `agent/personality.mjs` (+ test), fragments `wp4` |
| WP5 radio | `/hq` agent: prompt rules + fake parser for reports/backup/cancel, persona + env lines | `agent/agentTurn.mjs`, `agent/test_agent.mjs`, `agent/fixtures/*` |
| lead | hookups (`CLW_Situation.c`, `CLW_WarMode_ShortConflict.c`, war start/stop, `CLW_SitrepBuilder.c`), strings, compile, DS e2e, docs | |
