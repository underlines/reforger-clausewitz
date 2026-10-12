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
