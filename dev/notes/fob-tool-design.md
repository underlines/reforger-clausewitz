# FOB placement as an HQ tool (M4.6, work package "fob")

Spec: `dev/VISION.md` 13b "FOB placement is the HQ's strategic decision". Today the LLM picked one of 4 precomputed sites
(`fobSites`, all on the HQ line). Now the commander chooses WHERE (or whether) in two steps; the game validates and snaps.

## Flow

```
planner/commander reply          game                                   sidecar
 fobs:[{ESTABLISH_FOB, area:"D5", intent}]  --->  registry ESTABLISH_FOB (area)
                                                   StartFobZoom: zoom digest of D5
                                                   POST /fobzoom {faction, area, intent, attempt, zoom}  --->  zoom prompt, 1 LLM call (role fobzoom)
                                                   <--- {ok, cell:"f7" | pos:[x,z] | decline:true, reason}
                                                   CLW_FobPlacer.Resolve (validate + snap)
                                                     ok     -> dynamic pool site FSL<n> -> registry ESTABLISH_FOB {site}
                                                     fail   -> attempt 2: POST /fobzoom {retry:{reason,pos}} (once)
                                                     fail 2 -> counts as a failure
 fobs:[{ESTABLISH_FOB, area:"none", reason}] = explicit decline (counts as a decline)
```

* Step 1 (macro) rides on the existing `fobs` channel of the commander/planner reply (terse `["ESTABLISH_FOB","D5","intent"]`,
  `"none"` = decline). The macro picture is `fobMap` in the situation (below). No protocol bump: `fobMap` is an optional field of v4,
  `fobSites` is no longer sent; an old sidecar/game pair keeps working with `site` (the rule doctrine still uses pool site ids).
* Step 2 (zoom) is a dedicated game->sidecar call `POST /fobzoom` (synchronous HTTP answer, its own REST context `fobzoom-<faction>`,
  60 s game timeout, LLM role `fobzoom` = strong slot, `agent/fobZoomPrompt.mjs`). The sidecar is stateless: the retry prompt carries
  the previous pick and the game's reason. Fake provider: deterministic pick (valid cell with a road nearest to `toward`).
* Failure/decline counter per side (`CLW_FobPlanner`): LLM decline, zoom decline, transport error or two rejected spots = 1 failure;
  a success resets it. At 2 consecutive failures the rule doctrine places a FOB right away (`CLW_FobDoctrine.Think(..., force)`),
  then the counter restarts. LLM off/offline: the rule commander runs the doctrine as before.

## Map digest (one builder: `CLW_MapDigest`, reusable for the 13a spatial picture)

Macro grid: cell size S = clamp(round(extent/10 to 100 m), 500, 1600) m (Arland ~500, Everon ~1300), at most 20x20 cells.
Cell id = column letter (west->east, A..) + row number (north->south, 1..). Cell glyphs (2 chars): terrain + road.
`~` sea/lake, `c` coast, `T` town/city, `v` village, `M` mountain (relief >= 90 m), `h` hills (>= 40 m), `f` forest, `.` open;
road `=` main (width >= 6 m), `-` minor, `_` none. Computed once per world from 25 height samples and 9 tree-density probes per
cell, roads from `RoadNetworkManager`, places from `CLW_Places` (cached, rebuilt on Reset); build time logged.

`fobMap` (static part first so the lean prompt caches it):

```json
{"cell":1300,"x0":0,"z0":0,"cols":"ABCDEFGHIJ","n":10,"legend":"...","rows":["..._","..."],
 "towns":["Morton C4","..."],"pois":["Airfield F2"],
 "objs":[["A","Morton","C4","US",0]],"hq":{"own":"B2","enemy":"I9"},"front":"C","frontCell":"E5","fobs":["C3"],"contacts":[["E5",3,"inf"]]}
```

Token estimate (measured on the 10x10 test fixture, 5 objectives, 2 towns: static ~160, dynamic ~65, total ~225 tokens incl. the 40-token legend; a real map with 14 towns / 8 objectives ~350). It is sent only when
`fob` is on, fobMax > 0 and the side has room for another FOB. Size is logged: `CLW: fob map <side>: ... chars (~N tokens)`.

Zoom digest (`zoom` in the POST): the macro cell at 100 m resolution (S/100 x S/100, at most 16 x 16, ids `a`.. + row, e.g. `f7`),
3 chars per sub-cell: elevation digit 0-9 (relative to `elev.min..max`), terrain, road.
Terrain glyphs: `~` water, `x` keep-out (game rule: HQ clearance, objective capture edge + 250 m, other FOB spacing, enemy contact),
`^` steep, `T` many buildings (>= 5), `b` few buildings, `f` forest, `.` open. Plus `pois`, `objs` (in the area and within 1 km),
`contacts` (known, with age), `own` (HQ/FOB sub-cells), `toward` (front objective position). ~16x16x3 = 770 chars + lists = ~400 tokens.
Reply: `{"cell":"f7","reason":".."}` or `{"pos":[x,z],..}` or `{"decline":true,"reason":".."}`.

## Validation and snapping (`CLW_FobPlacer.Resolve`)

Wish = `pos` or the centre of `cell`. Search radius 250 m around the wish (`CLW_Placement.FindSiteEx`): dry (+25 m shore), flat
(`PLACE_MAX_SLOPE_TAN`), inside the map, clear of both HQs (base clear), of every objective (capture radius + `FOB_MIN_OBJECTIVE_DIST_M`),
of live FOBs (spacing) and of known enemy contacts; `CLW_FreeSpot.RefineSite` makes room for the cluster; road within 60 m
(`CLW_FobSites.ROAD_MAX_M`, the builder truck must reach it); then `CLW_FobManager.CheckSite` (own half, spacing, contacts).
The result becomes a dynamic pool site `FSL<n>` and goes through the normal `ESTABLISH_FOB {site}` path (cost, reserve, builder).
A rejection returns a one-sentence reason built from the rules the WISH violates ("no road within 60 m of any valid ground near ...",
"within 250 m of the own HQ", "water", "too steep", "too close to FOB1", "enemy contact near").

## Rule fallback (improved pool, `CLW_FobSites.Build`)

Per side and per objective that is at least `FRONT_MIN_M` from that side's HQ: candidate on the line objective -> own HQ at
d = clamp(0.3 * dist, need + 60, max(need + 210, 0.5 * dist)) from the objective (need = capture radius + 250 m), capped so it stays
>= 1.5 x base clearance from the own HQ; three tries (straight, +-30 degrees) so the snap finds ground; id `FSO<obj>_<side>`.
Every flank therefore has a candidate on the own side. The old along-the-HQ-line sites remain only as a last resort when a side got
fewer than 2 candidates. Ranking: `FrontObjective` skips objectives already covered by an own live/planned FOB (within 900 m scaled), so
a second FOB goes to the next flank (`FrontScore` unchanged).

## Log lines (DS test)

`fob map`, `fob zoom`, `fob place`, `fob decision`, `fob fallback`, `fob site` (pool), `fob doctrine`, sidecar: `fobzoom faction=.. area=.. -> ..`.

## Files

Game: `CLW_MapDigest.c` (new), `CLW_FobPlacer.c` (new: Resolve + CLW_FobPlanner), `CLW_FobSites.c`, `CLW_ActionsFob.c`, `CLW_Fob.c`
(`CanStart`, `FobMapJson`), `CLW_RemoteCommander.c`, `CLW_Situation.c`, `CLW_ActionRegistry.c` (script override of the ESTABLISH_FOB
params, `CLW_FobActionSpec`). Sidecar: `llm_sidecar.mjs` (`/fobzoom`, validateV2, fake), `agent/fobZoomPrompt.mjs` (new),
`agent/commanderPrompt.mjs`, `agent/promptKit.mjs`.
