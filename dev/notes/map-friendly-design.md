# Friendly units on the player's map: setup switches (2026-10-13, owner request, plan)

Replaces the single "See friendly map markers" level of VISION 8a with four independent on/off switches in the setup menu.
Default realism-focused: all OFF means a player sees only what vanilla/fog gives them (own side's known picture), see 4.

## 1. The four switches (war-mode params, `CLW_ModeParam.CreateBool`, setup menu + `/war start key=value` + overrides.json preset)
| key | label | shows on the map of a player of faction F |
|---|---|---|
| `mapMe` | Show me | the player's own position |
| `mapSquadmates` | Show squad members | every member (AI + human) of the player's own group, individually |
| `mapSquads` | Show friendly squads | ONE symbol per friendly group (at its leader / centre), no individual members, humans' groups and AI groups |
| `mapVehicles` | Show friendly vehicles | every friendly vehicle with crew or claimed by a group (symbol by kind from data, not hard-wired) |

- Independent, combinable. `mapSquads` does not imply `mapSquadmates`/`mapMe` (a squad symbol may overlap them; dedupe: the player's own
  squad is drawn as members when `mapSquadmates` is on, else as one squad symbol if `mapSquads` is on).
- Applies per faction on the client; the enemy is never shown beyond known contacts (existing contact markers). No leak: the server only
  sends a client what its switches allow.
- Setting is per war (mode param), same value for all players. (A per-player in-map toggle is a later option.)

## 2. Facts to establish first (research spike, before coding)
1. Does vanilla already draw the local player and group members on the map (it does for squads in the Reforger map via
   `SCR_MapMarkerManagerComponent` / group markers / `SCR_MapEntity` player marker)? If yes, "off" must SUPPRESS that vanilla marker
   (find the hook, client-side), not only skip ours.
2. Cheapest correct mechanism for moving units: a client-side map layer drawn over `SCR_MapEntity` (see VISION 8a tier 2) fed by a compact
   server->client picture, versus static `InsertStaticMarker` markers (replicated to everyone, bad for moving units; only OK for slow squads).
3. Vehicle symbol/kind source (data, `.conf`, runtime discovery), human-led squads vs AI squads, vehicles with player crew.
4. Bandwidth: positions of N squads + vehicles per faction every 3-5 s to that faction's clients only.

## 3. Architecture (to confirm by the spike)
- Server: `CLW_FriendlyPicture` builds per faction a compact list {kind: me|member|squad|vehicle, id, pos, heading, symbolKind, label, human}
  filtered by the switches PER RECEIVING PLAYER (me / own-group members are per player, the rest per faction). Sent over a dedicated
  PlayerController RPC (pattern: `CLW_SitrepPlayerController`) every ~4 s, only when something changed or on join.
- Client: map layer widget draws symbols (NATO-style by kind, colour by faction, human-led marker) and removes them when the map closes;
  vanilla own/group markers suppressed when the matching switch is off.
- Setup menu: four bool rows (existing `CLW_ModeParam` mechanism renders them), keys `#CLW-Param_mapMe` etc. in `dev/i18n/CLW.keys.txt` (EN+DE).
- Dashboard: no change needed (it already shows the full picture); optionally show the four settings in the Setup/Overview view.

## 4. Open questions (decide after the spike)
- Default values (proposal: me ON, squad members ON, squads OFF, vehicles OFF? or all OFF for realism). Owner decides.
- GM takeover / spectators: show everything or nothing.
- Dead/unconscious members, vehicle wrecks: removed immediately.
