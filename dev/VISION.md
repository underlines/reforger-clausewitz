# Clausewitz - Vision

> "War is the realm of uncertainty; three quarters of the factors on which action is based are wrapped in a fog of greater or lesser uncertainty." - Carl von Clausewitz

**Clausewitz** is an Arma Reforger mod where two (or more) factions are commanded by **AI High Commands**: a built-in rule-based commander that works out of the box, or an **LLM** for a smarter, more human opponent. Each HQ sees only what its own units and sensors report (**real fog of war**), plans with **limited resources**, and issues orders down a **realistic chain of command** to AI units that fight with vanilla Reforger AI. Players can join, watch, or become part of the chain (map reports, even voice/radio to HQ).

Status: **vision only**. See `research/feasibility-research.md` for what is known to be possible. Items marked `[verify]` are unproven and need a spike before design is locked.

---

## 1. Core loop

```
 World (Reforger server, authority)            Sidecar (outside the game)
 ┌───────────────────────────────┐             ┌──────────────────────────┐
 │ units perceive enemies        │  SITREPs    │ Faction A agent          │
 │ per-faction "known world"  ───┼────────────►│  - belief state (fog)    │
 │ resources, bases, losses      │  world-state│  - memory (episodic)     │
 │                               │             │  - plan + orders         │
 │ order executor (waypoints,    │◄────────────┼─ validated structured    │
 │ spawns, supply, build)        │   orders    │  orders (JSON)           │
 └───────────────────────────────┘             │ Faction B agent (same)   │
                                               └──────────────────────────┘
```

1. Units perceive; the mod aggregates per-faction contacts (with age, confidence, position error).
2. A **SITREP** flows up the chain of command (squad -> platoon -> company -> HQ), possibly delayed or degraded.
3. The faction agent gets a **compact world-state** (only what that HQ knows), its **memory**, and its **resource state**, and decides.
4. The agent emits **structured orders** from a fixed command vocabulary. The mod **validates** them against real state (is the unit alive, is the budget there, is the position legal) before executing.
5. Vanilla AI executes and fights. Units with no orders fall back to **autonomous behaviour** (section 7).

The LLM works at **intent level** on a 15-60 s planning tick. Combat reactions stay with vanilla AI. The LLM is never in a per-soldier real-time loop.

### Commander modes

Each faction's HQ is one of two interchangeable commanders behind one interface:

```
FactionPicture (own units + known contacts, fog applied)
        │
   ICommander ──► RuleCommander    (Enforce script, in game, no setup)
        │    └──► RemoteCommander  (REST → sidecar → LLM)
        ▼
 Commands (fixed vocabulary, section 5) → Validator → Executor
```

- **Rules (default):** simple scoring in Enforce script, no sidecar, no cost. Defend what is threatened, attack the weakest known contact near an objective, recon where intel is missing, reinforce or fall back by strength ratio, keep a reserve. Difficulty = a few knobs (reaction time, commitment, intel use). Works straight from the Workshop.
- **LLM:** the same picture is sent to the sidecar; orders come back in the same vocabulary. Opt-in via scenario settings (endpoint, model).
- **Fallback:** if the LLM is slow, failing or over budget, that faction drops to Rules until it recovers. The war never freezes.
- **Mixed:** chosen per faction, so LLM vs Rules, Rules vs Rules, or LLM vs LLM (also the main test/benchmark setup).

Both see exactly the same fog-limited picture and share validator, executor, SITREPs and everything below; only the decision-making differs. New features get a rule-based version first; the LLM gets them through the shared vocabulary.

---

## 2. Scope: what the agents command

- **Infantry** (fireteams, squads, platoons).
- **Mechanized**: IFVs/APCs with embarked infantry, tanks, trucks (logistics), recon vehicles, and later helicopters / artillery / CAS as stretch goals. Vehicle AI is the weakest part of vanilla; see research doc. Must be tested early, not last.
- **Logistics units** (supply trucks, engineers) as first-class citizens: the economy only works if they can be ordered and can be killed.

---

## 3. Scenario setup screen (first player joins)

A lobby UI shown to the first (admin) player before the war starts. Dedicated-server friendly (can also be pre-set from a config file / JSON for headless runs).

| Setting | Options |
|---|---|
| Faction A / B | Any faction from loaded mods (vanilla, RHS, WCS, custom). Read dynamically from faction + entity catalogs. |
| Home base location | Pick on map, or **Random** (constrained by min distance, road access, terrain flatness) |
| Infantry size | S / M / L (company-ish numbers, caps tied to the unit budget) |
| Mechanization | None / Recon only / Mechanized / Full (tanks + IFVs + support), each S / M / L |
| War type | A attacks, B defends / B attacks, A defends / meeting engagement / both defend (attritional) |
| Starting resources | Low / Medium / High, plus supply income rate |
| Max total AI / "size of war" | Auto-derived from server performance budget, adjustable |
| Cleanup policy | See section 9 |
| Fog of war strictness | Full / Reduced (some free intel) / Off (debug) |
| Human participation | Players join a faction as ordinary soldiers, as HQ observers, as a commander with LLM staff support |
| Commander (per faction) | Rules (difficulty) / LLM (with Rules fallback) |
| LLM settings | Model/endpoint, planning interval, "think budget", dry-run flag |
| Win conditions | Capture/destroy HQ, hold objectives for N minutes, attrition threshold, time limit |
| Other | Time of day / weather, reinforcement waves, whether LLM may use arty/air, difficulty (AI skill), random seed |

Generated scenario = a data object (`ScenarioConfig`) that is saved/loaded, so a setup can be repeated, shared or run headless.

---

## 4. Chain of command & SITREPs

**Upward (reporting):**
- Every AI group has a leader. Groups produce SITREPs on a timer and on events (contact, casualties below threshold, objective reached, ammo/fuel low, lost contact).
- SITREP content: position, strength, ammo/fuel state, current task + progress, observed enemies (type, count, bearing/grid, movement, age), status flag (OK / contact / pinned / combat-ineffective).
- Reports travel up **through intermediate echelons**, which merge and summarise. Optional realism: **report delay**, **distortion**, **lost reports** when a leader dies or comms are cut (radio range / jamming as later options).

**Downward (orders):**
- HQ agent -> formation commanders (platoon/company) -> group leaders -> units, using group waypoints and orders. Echelon agents can be real separate LLM calls (expensive, rich) or HQ-only with deterministic decomposition (cheap). Make this a **setting**: "HQ only" / "HQ + company staff" / "full chain".
- Orders carry: intent, objective, constraints (ROE, route, avoid), priority, and a timeout.
- Units acknowledge, report progress, and report failure (cannot comply: blocked, no fuel, outgunned).

---

## 5. Command vocabulary (repeatable meta-commands)

The LLM only picks from a fixed, validated set; each maps to waypoint chains / group settings / spawns.

| Command | Meaning |
|---|---|
| `SECURE <location>` | Move to and hold the location, then report. |
| `DEFEND <location> [depth/posture]` | Hold with defensive posture, optionally in prepared positions. |
| `ATTACK <position/objective>` | Assault with specified axis / support. |
| `DESTROY <target base/unit group>` | Attack to destroy a known enemy asset. |
| `RECON <area> [infantry/vehicle/mixed] [covert/aggressive]` | Scout and report contacts, avoid decisive engagement. |
| `MOVE <unit(s)> <A -> B> [route, formation, speed]` | Redeploy. |
| `FALL_BACK <units/all> <X>` | Withdraw to a rally point, with delay-action options. |
| `REINFORCE <location/unit>` | Send units to strengthen a friendly position. |
| `RELIEVE <unit>` | Replace a unit in position. |
| `ESCORT / CONVOY <asset> <A -> B>` | Protect and move logistics. |
| `RESUPPLY <unit/base>` | Dispatch supply trucks. |
| `ESTABLISH_FOB <location>` / `FORTIFY <location>` | See section 6. |
| `REQUEST_FIRE_SUPPORT <target>` (stretch) | Artillery/CAS if available. |
| `HOLD_POSITION` / `RELEASE` | Block / unblock autonomous behaviour. |

Commands are first-class objects with an ID, state (pending, active, done, failed, cancelled), owner, assigned units, and timestamps, so the LLM can reason about "what is in flight" and the mod can display it.

---

## 6. Economy, bases, logistics, building

- Each faction has **resource pools** (supplies, optionally fuel/ammo/manpower) held at bases; spending = spawning units, building, resupply. Income from bases/objectives (Conflict-style) or a rate set in the scenario.
- **Multiple bases / FOBs**: resources are stored per base; moving them between bases requires **actual supply trucks** (or helicopters) on the road, which can be ambushed. The LLM schedules convoys.
- **Reinforcements** spawn at a base, consume budget, and are then ordered like any other unit.
- **Creating / destroying FOBs**: HQ orders `ESTABLISH_FOB`; engineers + supply trucks move there; the FOB is built. Enemy can capture / destroy; loss redistributes or loses resources.
- **Building**: Reforger's campaign building system is script-drivable server-side (community-confirmed). Preferred: real engineer behaviour (an AI moves to a site and "builds"). Acceptable fallback: **fake it** - a timed progress bar and a script that spawns the composition (still consuming the supplies, still requiring the engineer/trucks on site). Both modes should hide behind one interface (`IBuildExecutor`).
- **Fortifications**: sandbags, bunkers, trenches, AT positions, vehicle hull-down spots from compositions; AI units garrison / use them as defend positions `[verify AI actually uses placed fortifications well]`.
- Depot supply, vehicle repair/refuel/rearm: stretch goals; at minimum track them abstractly.

---

## 7. Autonomous behaviour (no orders for a long time)

Every group has an **orders age** timer. If no order arrives for N minutes AND the unit is not under `HOLD_POSITION` / `DEFEND` lock:

1. **Assess** (rule-based, no LLM): threat nearby? supplies? isolated?
2. **Choose** one of:
   - request instructions from HQ (queue a "requesting orders" SITREP, gets priority in the next planning tick),
   - move to a nearby friendly position / rally point / the front line,
   - fall back if weak or outgunned,
   - patrol or scout around the current position,
   - join an adjacent friendly group.
3. A **local mini-planner** can be rule-based first, LLM-assisted later (cheap model, tiny prompt).
4. Autonomy radius is capped (`SetMaxAutonomousDistance`-style), and all autonomous actions are reported up.

---

## 8. Intel: sightings, map markers, humans in the loop

- **AI sightings -> faction picture:** from group perception. Each contact has position, type, count, direction of movement, age and confidence. Contacts decay and expire (stale after N minutes), merge with newer ones.
- **Player map markers:** the vanilla game already has an AI enemy-marking system (`SCR_AIEnemyMarkingSystem`: time-to-delete, position inaccuracy by distance). Plan: reuse or re-implement it so the faction's *known* enemy contacts appear on the friendly map **automatically** and **disappear when stale**.
- **Humans report too:**
  - Option A: players place standard map markers; the mod parses them (marker type/icon/text) into contacts for HQ and removes them when stale.
  - Option B: an **in-game "report contact" quick menu**: look toward the enemy, open a radial menu, choose Infantry/Vehicle/Armor/Air/Unknown -> count -> moving/stationary -> bearing/distance taken from the player's view/range estimation. Creates a marker plus a SITREP entry.
  - Human reports are treated as lower/higher-confidence sources, configurable.
- Same pipeline whether the report came from AI or player: `Contact` objects -> faction picture -> prompt context + map.

---

## 9. World-state hygiene: cleanup, pausing, size of war

Large AI wars die on entity counts. The mod needs an explicit **population manager**:
- Hard **budget** for total AI/vehicles (server-performance driven), exposed in the scenario screen.
- **Dead bodies / wrecks**: use the engine garbage system (`SCR_GarbageSystem`: lifetime, insert/bump/flush) with per-type lifetimes; faster cleanup for infantry far from any player/contact, slower near fights.
- **Distance / relevance LOD:**
  - Pause or simulate abstractly groups far from anything interesting `[verify via dormant/"expand-collapse" group API: ChimeraAIGroup has dormant + expand/despawn members]`.
  - Collapse inactive groups to a data record (type, strength, position) and respawn on demand, so a 1000-man "army" is a few hundred live entities.
- **Delete vs. pause policy** is a scenario setting: *preserve* (collapse and keep strength), *attrit* (delete when far from action and mark as lost/reserve), *aggressive* (delete after N minutes out of contact).
- Safe-guards: never delete a unit while it is in an active command, only after the command is reassigned; keep counts consistent with the faction's resource ledger.

### Experimental (far future): virtual battlefield

An ALiVE-style layer (see research section 8a). Admin toggle, **off by default**; all radii and timers configurable.

- **Idea:** units far from humans (or all units when nobody is on the server) live only as **data records**. They keep moving, fighting, taking losses and following orders. Only the parts of the war near humans exist as real entities. Result: a much larger war than the engine could keep live.
- **Record:** id, faction, group type, position, per-soldier health, vehicle damage, ammo, current command + waypoints, last-simulated time. Damage, ammo and casualties survive both directions of the transition.
- **Virtual movement:** along the order's route at the group's speed × time since last update. Update a few records per frame, round-robin, so cost stays flat.
- **Virtual combat:** enemies within a few hundred metres fight by strength-based attrition with randomness, ammo drain and per-class balance (ALiVE's virtual armour was overpowered). Virtual detection produces the same `Contact` records, so commanders and fog of war work unchanged.
- **Materialise** when a human comes within ~1000-1500 m (aircraft observers separately). Spawn queue, nearest-to-player first, ~1 group per 0.3 s, units invulnerable until the whole group exists, positions validated (navmesh/road, never inside objects), active cap respected.
- **Collapse** at a larger radius (hysteresis), never while in combat, airborne, or shortly after a human was near or died nearby.
- **Subtle by design:** spawn out of sight where possible; materialised units continue their virtual order seamlessly.
- **Engine fit:** our own records with `Manual` group lifecycle, not the engine's distance-driven dormancy (which would stop all far fights). Relevant APIs in research section 8.
- **Why it matters even without players near:** it also solves "AI-vs-AI fights far from any player never happen", so a cheap version may come earlier than the full feature.

---

## 10. Custom factions and third-party content (RHS, WCS, etc.)

The mod must not hard-code vanilla prefabs. Instead:
- Read factions from `SCR_FactionManager` and unit/vehicle/group definitions from entity catalogs (`SCR_EntityCatalog`, groups, labels).
- Build a **faction capability profile** dynamically: which roles exist (rifle squad, AT, MG, sniper, recon, APC, IFV, MBT, truck, engineer, medic...), their cost, crew/passenger seats.
- A **role classifier** (labels, weapon/vehicle type, seats, armour/weapon data) maps arbitrary prefabs to the mod's roles; an override JSON allows manual fixes per mod pack.
- RHS / WCS and other packs are **optional dependencies**; ship only soft compatibility data (mappings), no hard requirement.
- Everything the LLM sees about a faction is generated from this profile (so it knows what it has).

---

## 11. Humans: chat, voice and radio

Goal: a player talks to HQ (and HQ to the player) in natural language.
- **Phase 1 - text:** in-game chat (`SCR_ChatComponent`) -> mod forwards to the faction agent; replies appear in chat (and radio-style UI). Cheap, certain to work.
- **Phase 2 - voice (research needed):** the Reforger script API exposes VoN capture only through components; **no script access to raw microphone audio was found** `[verify]`. Likely route: a small **companion app on the player's PC** (push-to-talk -> Whisper/other STT -> HTTP to the sidecar or to a local mod endpoint), with TTS for HQ replies. A server-side tap of the VoN stream may be possible but is unconfirmed.
- Natural-language commands from players are parsed by the LLM into the same command vocabulary.

---

## 12. Architecture summary

| Layer | Responsibility | Tech |
|---|---|---|
| **Game mod (Enforce Script, server authority)** | Perception aggregation, contact tracking, SITREP generation, **rule-based commander**, order validation/execution, spawning/building, population manager, scenario setup UI, map markers, chat bridge | Enfusion scripts, `RestContext` |
| **Sidecar (separate process, optional)** | Only for LLM mode. One agent per faction: belief state, memory, planning, prompt construction, LLM calls, schema validation, scenario/state persistence, logging/replay | Node or Python, any LLM |
| **Optional companion** | Voice capture, STT/TTS | Local app |
| **Spectator/debug tools** | God-view map, agent reasoning log, replay | Web UI |

Design rules:
- **Server-side only**; clients just display.
- **Validate everything** the LLM says; it can only choose from the command vocabulary with checked parameters.
- Every mutation has a **revert path**; **rate-limit** orders; ship **dry-run** (log the command, change nothing) first.
- **Compact state**: summarise to stay far below the 1 MB REST limit and keep tokens low; the LLM never sees information its faction has not earned.
- **The mod is complete without an LLM.** The in-game RuleCommander is the default and the fallback when the LLM is slow, offline, or budget-limited (section 1, Commander modes).

---

## 13. Roadmap

Own game mode `CLW_GameMode` (see `notes/decision-game-mode.md`). Live status: `PROGRESS.md`.

| Milestone | Contents | Proves |
|---|---|---|
| **M0 - Skeleton** | Game mode + minimal world, one squad per side, per-faction contacts, RuleCommander issues MOVE/ATTACK/DEFEND waypoints, logs. Spikes: perception tap, order loop | Perception + waypoint APIs, core loop, playable without LLM |
| **M1 - LLM bridge** | REST spike (POST/headers/polling), sidecar, RemoteCommander, schema validation, fallback to Rules | LLM loop |
| **M2 - Real war** | More commands, SITREPs, win conditions, spawn budget, resource ledger, bases, supply convoys | AI vs AI with economy |
| **M3 - Mechanized & intel** | Vehicle groups, embark/dismount, convoys; map markers, expiry, player reports, setup screen | Vehicle AI, humans in the loop |
| **M4 - Scale & realism** | Chain-of-command echelons with delayed reports, population manager (cleanup, collapse/expand), FOBs | Size of war |
| **M5 - Content compat** | RHS / WCS profiles, role classifier | Custom factions |
| **Later** | Voice/radio, virtual battlefield (experimental), Conflict adapter | - |

## 14. Open questions / risks

1. POST body reliability and custom headers in `RestContext` (community reports conflict).
2. How many simultaneously live AI the server can sustain; actual collapse/expand behaviour.
3. Vehicle AI quality for mechanized assaults, convoy cohesion, off-road routing.
4. Whether AI uses placed fortifications sensibly.
5. Raw voice audio access (probably impossible in-script).
6. Cost/latency: two continuously planning LLMs; local model needs real hardware.
7. LLM hallucination: need strict schema + validation + ground-truth reconciliation.
8. Dedicated server/Workshop quirks: stale caches, client/server script mismatch.
9. Whether ground-truth fog of war (knowledge-per-faction) can be kept consistent when players can also see/mark things.
10. Replication/authority: ensure all state lives on the server and UI is client-side only.
