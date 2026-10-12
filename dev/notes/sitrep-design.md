# Faction SITREP: design (2026-10-12, branch feature/ux-sitrep)

Research: `research/sitrep-and-briefing.md`. Goal: a short, player-useful situation report per faction, shown to that faction's
players (deploy screen, map journal, hint) and on the web dashboard. Works with no sidecar; the LLM version replaces it.

## 1. Template (one structure for faction now, squad later)
```
headline   US SITREP T+12:30 | tickets 380 vs 395 | points 2/6 | 7 units, 3 contacts
situation  Holding A, B. Enemy holds F. Open: C, D, E. Contested: C. / Tickets 380 (enemy 395). 41:20 left.
enemy      SALUTE-condensed, freshest first, max 4: "4x USSR infantry, 300 m NE of C Timber Hub (grid 026 027), 12 s ago"
mission    HQ intent + main effort: "Main effort: C Timber Hub (3 units). Take and hold the objectives, preserve tickets."
execution  one line per main unit ("US_Inf1 ATTACK C Timber Hub"), idle count, "Squad leaders: accept an HQ mission (J)"
logistics  Supplies 220 (+60/min). AI 38/50, vehicles 3/6. 120 personnel. Weak: US_Inf2 3/8.
```
Sections <= 420 chars, headline <= 140 (enforced in `CLW_Sitrep.Clean`). Ids/titles live in `CLW_SitrepTemplate`
(`situation, enemy, mission, execution, logistics`). Basis: US/NATO SITREP + SMEAC + SALUTE; the vanilla journal entry types
(Situation, Intel, Mission, Execution, Custom "Logistics") map 1:1.
**Squad SITREP (later, not implemented):** same object with `scope:"squad"`, `unit:<squad id>`; sections = own strength/wounded,
local contacts (SALUTE), current task, next step, ammo/vehicle. Data shape, wire format, store and journal mapping already carry
`scope` + `unit`, so a squad level only adds a builder and a recipient rule (squad members instead of faction players).

## 2. Data flow
```
game (server)                                              sidecar                         players
CLW_RemoteCommander.Think (10 s)
  UpdateSitrep -> CLW_SitrepStore.UpdateRules (every 20 s, forced on war start)
      CLW_SitrepBuilder.Build(book, registry, orders)  == rules SITREP, fog-limited
  POST /orders  { ...situation..., "sitrep": <rules json> } ----> strips it, keeps as "rules" source
                                                                   LLM call (first / due / trigger):
                                                                   prompt asks for "sitrep" in the SAME reply
  OnReply: reply.sitrep -> CLW_SitrepStore.SetLlm <------------- { orders, buy, missions, sitrep }
  CLW_SitrepStore.Current(faction) = LLM while <= 360 s war time old, else rules
  SyncPlayers: for each player -> his faction's SITREP, only when the content version changed
      RPC CLW_SitrepSend (listen-server host: direct call) ----> CLW_SitrepClient.Receive
                                                                 journal entries rewritten locally (map + deploy screen)
                                                                 hint on spawn/respawn, on big update (>= 240 s apart), /sitrep, H
GET /api/state  factions[<f>].sitrep  <- sidecar composes LLM-if-fresh else rules (generatedAt only moves when content changes)
```
Fog of war: the rules builder reads this faction's contact book, own units/orders and public war values (objectives, both ticket
counts, as on the HUD); the LLM sees only the faction situation it already gets. Each player gets only his own faction's entry; the
journal text exists only on his client. `RewriteEntry_SA` (broadcast) is deliberately not used.

## 3. Protocol change (v2, backward compatible)
- Game -> sidecar: optional top-level `"sitrep": {scope,unit,source:"rules",warTimeSec,headline,situation,enemy,mission,execution,logistics}`.
  Removed by the sidecar before the model sees the situation (no tokens spent on it). Old sidecar ignores it.
- Sidecar -> game: optional `"sitrep": {headline,situation,enemy,mission,execution,logistics}` (strings, `\n` line breaks) next to
  orders/buy/missions, also on `keep:true` replies. Old game ignores it. A reply with only a sitrep is valid.
- Model prompt: a SITREP paragraph (template keys with char budgets) is appended only on calls where a SITREP is wanted;
  `max_tokens` is raised by 450 for those calls so the JSON is not cut. "sitrep" is asked to be the last key.

## 4. Cadence and why (decision: embed in the commander reply, no extra call)
- The commander is already called at "first", at its own scheduled interval (20-300 s, adaptive) and on triggers (objective change,
  ticket drop, unit lost, new contact...). Those are exactly the moments a SITREP changes, and the model has just decided its
  orders, so mission/execution in the SITREP match the orders issued in the same reply (a separate call could contradict them).
- Wanted when: first call of a faction (start SITREP arrives with the first order reply), >= `sitrep.intervalSec` (120 s, config
  group `sitrep`, validated 30-3600) since the last one, manual wake, or a trigger text with objective / tickets / lost.
  Cost: ~250 output tokens on roughly every second to fourth call; no additional round trip, no extra latency budget except the
  longer generation.
- Rules SITREP covers: no sidecar, sidecar down, provider error, budget stop, engine "rules", before the first LLM answer.
  Players therefore get a SITREP within ~10 s of the first commander tick, and the LLM text replaces it when it arrives.
- Rejected: separate cheap call every few minutes (a second prompt, second failure mode, inconsistent with orders, duplicate
  situation tokens); computing the rules SITREP in the sidecar only (the Workshop mod must work without it).

## 5. In-game display (vanilla UI only, one prefab component + one conf)
1. Map journal ("!" tool of the map; also on the deploy screen map): `SCR_RespawnBriefingComponent` added to `CLW_GameMode.et`
   with `Configs/CLW/Journal/SitrepJournal.conf` (five entries, ids 0-4, default key = every faction) and `m_bShowJournalOnStart 1`.
   `CLW_SitrepClient.UpdateJournal` rewrites the entries locally; the headline is the first line of Situation.
2. Hint (`SCR_HintManagerComponent.ShowCustomHint`): mission + enemy + situation summary (<= 330 chars), 28 s, on every
   spawn/respawn (shown when the SITREP arrives if the player spawned before), and when the mission text changes (>= 240 s gap).
   Vanilla **H** (`HintToggle`) shows it again while it is the latest hint. Fallback chat line if hints are disabled.
3. Chat command `/sitrep`: shows the hint now and asks the server for a fresh copy.
A player joining at the start gets the start SITREP (rules, then LLM); one joining later gets the current one on the first sync
after he has a faction (<= 4 s after the next commander tick or immediately on a content change).

## 6. Files
Added: `CLW_Sitrep.c` (data, template, wire/JSON, DTO), `CLW_SitrepBuilder.c` (rules), `CLW_SitrepStore.c` (server store + delivery),
`CLW_SitrepClient.c` (journal, hint, /sitrep), `CLW_SitrepPlayerController.c` (RPCs), `Configs/CLW/Journal/SitrepJournal.conf(.meta)`.
Edited: `CLW_RemoteCommander.c` (DTO field, POST injection, reply handling, UpdateSitrep), `Prefabs/GameModes/CLW_GameMode.et`
(+ `SCR_RespawnBriefingComponent`), `dev/tools/llm_sidecar.mjs`. `CLW_RuleCommander.c` untouched (not needed).
Log prefix `CLW: sitrep ...` (store version/deliver lines, client receive/hint lines, journal-unavailable warning).

## 7. Dashboard contract
`GET /api/state` -> `factions[<faction key>].sitrep` =
`{ text, headline, sections:[{id,title,body}], scope:"faction", unit:<faction key>, source:"llm"|"rules", generatedAt:<epoch ms>, warTimeSec }`
or `null` (no v2 post yet). It sits on the same object as `situation/orders/buy/missions`, rebuilt on every /orders post, so it is
always present for active factions. Exchange log entries also carry `sitrep` (the model's, or null).

## 8. Why units take 30-60 s to start moving (observations, not fixed)
- `ThinkCommanders` runs every 10 s (first run 10 s after game start) and the sidecar's first call can take up to the 20 s provider timeout
  (the game waits up to 30 s); the rule fallback only runs on error/empty, so with an LLM nobody is ordered until the first answer.
- Start forces are spawned at war start and members arrive over several ticks; the registry sweep adopts them every 5 s.
- Order stability (45 s) and the AI LOD/activity lift also delay visible motion.
Cheap fix idea: at war start run the rule commander once immediately (orders), let the LLM override when its reply lands
(`CLW_Orders.LLM_PRIORITY_SEC` already protects fresh LLM orders from the rules).
