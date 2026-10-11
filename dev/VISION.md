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

### 1a. Middle tier: small label-probability model (JEV-like) [verify]

Between the full LLM/agent commander and the plain RuleCommander, add an optional **cheap classifier tier**. Per the project owner: JEV is a small, fast decision model introduced September 2026 that needs no finetuning; open variants exist besides the original commercial JEV, which has very low pricing and latency. Input is a context plus **custom labels**; output is a **probability per label** at runtime. Details of the real API/licence are unverified, check before designing (the idea does not depend on that exact model; any such zero-shot classifier fits).

- **Use:** pick among a fixed set of discrete choices per squad or faction, e.g. {ATTACK, DEFEND, MOVE/SEARCH, FALL_BACK, HOLD}, or choose which candidate objective/contact to act on, from a short text rendering of the situation. Far cheaper and faster than an LLM call, so it can run per squad every cycle (also a good fit for squad-leader decisions, section 4a).
- **Fallback ladder:** LLM/agent -> small classifier -> RuleCommander. Step down on timeout, budget exhaustion or offline; step up again when healthy. The classifier may also run *between* LLM calls so squads keep reasonable behaviour while HQ thinks.
- **Limits:** it only chooses among given labels (no free-form plans, coordinates come from rules/candidates), so the validator and executor stay unchanged. Probabilities can gate behaviour: low confidence -> keep the current order or escalate to the LLM.
- **Hosting:** behind the same sidecar interface (a new `CLW_LLM` backend mode, e.g. `classifier`), local open model or the commercial API; the dashboard (12a) can show label probabilities per decision.

### 1b. Adaptive HQ wakeup: later improvements

Basic version (built): game polls every 10 s, the sidecar scheduler calls the LLM only on first request, at a model-chosen `next_check_sec` (clamped, budget-stretched) or when a trigger fires (new contact, contact near squad, casualties, squad lost, strength drop, objective reached, squad change) or a manual wake. Later:

- **Game-side event push:** POST immediately on engagement/kill/contact events instead of waiting for the 10 s poll (lower latency, no polling load).
- **LLM-defined watch conditions:** the model returns conditions ("wake me if an enemy is within 500 m of X", "if squad Y reaches Z") that the sidecar/game evaluates cheaply, replacing the fixed trigger set.
- **Weighted alert score:** combine events into a score with decay (contact near + casualties + lost squad) instead of single thresholds; wake on score crossing, scale the cooldown by score.
- **Per-squad / per-sector wakeups** and squad-leader agents (section 4a) that handle local events so HQ is only woken for strategic changes.
- **Cheap classifier tier (1a)** decides whether an event is worth escalating to the big model.
- **Delta / summary prompts:** send only what changed since the last call plus a rolling summary to cut input tokens.
- **Budget-aware model downgrade:** switch to a cheaper model when pace overshoots, instead of only stretching intervals.
- **Learning per-mission default intervals** from past sessions (how often calls actually changed orders).
- **Selectable HQ engines (prepared):** the dashboard engine selector (`engine` config: `llm` | `rules` | `model`) plus a global pause. `rules` = rules-only tier (sidecar answers no orders, the game's rule commander drives, zero LLM calls); `model` = future trained decision model / classifier tier (1a) as a cheaper HQ engine; selector option is shown disabled ("soon") until it exists.
- Order-age awareness: skip waking while all squads still follow fresh orders with healthy ETA; wake when an order stalls (no movement progress).

---

## 2. Scope: what the agents command

- **Infantry** (fireteams, squads, platoons).
- **Mechanized**: IFVs/APCs with embarked infantry, tanks, trucks (logistics), recon vehicles, and later helicopters / artillery / CAS as stretch goals. Vehicle AI is the weakest part of vanilla; see research doc. Must be tested early, not last.
- **Logistics units** (supply trucks, engineers) as first-class citizens: the economy only works if they can be ordered and can be killed.

---

## 2a. Humans take part in the war (central)

Players are **not spectators**. They fight on either side, inside the same chain of command as the AI, and the AI HQ treats them as units it can task. Draft, to be refined:

**Roles**
- **Soldier:** joins a faction and drops into an AI squad (taking a free or AI-held slot). Fights normally, receives the squad's current task through the squad leader, sees the faction picture on the map (section 8).
- **Squad leader:** takes over an AI group. AI members follow the player; HQ orders that would go to the AI leader go to the player instead. Higher echelons (platoon/company leader) follow the same pattern later.
- **HQ staff / observer** (optional): sees the faction picture, can talk to HQ (section 11); no direct unit control.
- Join and leave at any time: when a human leaves, the AI leader resumes; when one joins, the AI hands over without a hiccup.

**Orders as missions**
- The same command vocabulary (section 5) is rendered for humans as a **mission**: short briefing text (intent, objective, constraints, timeout), objective and route on the map, priority. Rule commander uses text templates, the LLM writes the prose; the structured order stays identical.
- **Choice, not coercion:** HQ offers a squad leader a **menu of 2-4 assignments** (e.g. secure the ridge, recon the crossroads, ambush the road, support the neighbouring squad), each with its intent, risk and value to the plan. The player picks one (or declines all); HQ plans around the choice. Offers are generated from the commander's real needs, so every option is useful, and varied enough that play does not become a forced, repetitive checklist. Unchosen offers expire or go to AI squads. Soldiers can vote or follow their leader's pick. Tuning knobs: number of offers, how binding a pick is (reward/priority vs. HQ planning on it), time to decide.
- Player responds: **Accept / Cannot comply (reason) / Complete**; HQ treats a refusal like an AI "cannot comply" and re-plans.
- **Who gets waypoints vs missions (decided 2026-10-11):** an AI-led squad always gets plain waypoints from HQ, even when a human is a normal member (the soldier gets the task through the squad leader and may follow or leave). Only a squad whose **leader is a human** gets orders as missions (accept/decline, no forced waypoints). Missions need the vanilla task system, M2. Open `[verify]`: whether an AI leader obeys waypoints while a player is in the squad.
- **No forced waypoints on human groups.** The leader may deviate; HQ sees the deviation through reports and the squad's actual position and adapts.
- A player-led squad produces SITREPs automatically from its real state (position, strength, ammo, contacts); the leader can add a quick report (section 8, option B).
- Mission delivery uses the vanilla task system if it fits `[verify]`, else our own small task list + map markers.

**Consequences for the design**
- Faction join, spawn/loadout, respawn (tied to the resource ledger: supplies or tickets, spawn at base/FOB/squad), and group join/leave are now core, not optional: M0 must let a player join a side and spawn as a soldier.
- Human squads are scored for the commander's picture like any other group, but with higher-uncertainty behaviour (they may refuse, deviate or die without warning).
- Fog of war for humans = the faction's known picture, shown on their map, nothing more.
- Server authority: all state on the server, mission UI on clients.

Open questions: how many offers is right and how to keep them distinct; how a leader hands a squad back to AI; whether a lone player may refuse missions indefinitely (ROE for AI HQ); PvP balance when one side has more humans; how many AI slots a player may displace; late-join in the middle of an assault.

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
| Human participation | Allowed sides, slots per side, roles (soldier / squad leader / HQ staff), AI backfill of empty slots, respawn rules (section 2a) |
| Commander (per faction) | Rules (difficulty) / LLM (with Rules fallback) |
| LLM settings | Model/endpoint, planning interval, "think budget", dry-run flag |
| Win conditions | Capture/destroy HQ, hold objectives for N minutes, attrition threshold, time limit |
| Other | Time of day / weather, reinforcement waves, whether LLM may use arty/air, difficulty (AI skill), random seed |

Generated scenario = a data object (`ScenarioConfig`) that is saved/loaded, so a setup can be repeated, shared or run headless.

### 3a. Story mode (far-future idea)

An optional mode in the initial scenario setup (section 3), next to the usual settings (force strengths of BLUFOR/REDFOR, mechanization level, location). Besides the sandbox "realistic war", the host can pick or generate a **story and a plan for each side**, and the two HQs then play *loosely along that storyline* instead of purely optimising.

- **Story generator:** an LLM drafts the narrative from the scenario (setting, forces, location, tone): background, each side's objectives and intended arc (e.g. "REDFOR seizes the harbour at dawn, BLUFOR counterattacks from the hills at dusk"), key beats and a rough timeline. The host can edit, reroll or write it by hand; one story brief per side plus a shared omniscient "director" view.
- **Influence, not scripting:** the story is fed to the HQ agents as soft guidance (intent, expected beats, things to avoid), so behaviour stays reactive to the real battle but drifts toward the narrative. Deviations are allowed and the director can nudge back.
- **Narrative cheats allowed:** the director may bend realism to serve the story: spawn in reinforcements, vehicles or objectives ignoring the economy ledger (section 6), trigger scripted beats (a convoy arrives, a radio message), as long as it is logged and optional (setting: "strict economy" / "story may cheat").
- **Optional audio:** story intro, radio chatter and briefings as generated voice clips (TTS or another external audio API). Playing arbitrary audio in-game via the REST bridge is probably not possible (Reforger cannot stream audio from HTTP); candidates are pre-generated clips packaged as mod sounds, or playing them on the dashboard / host machine instead. [verify]
- Builds on: M1 (LLM bridge), M2 (orders/missions), the dashboard (12a) as director/storyboard UI, and the squad-leader agents (4a).

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

### 4a. Squad-leader agents (very late feature, not planned soon)

Not only HQ decides: AI **squad leaders can run their own LLM agent loop**, giving a proper hierarchical tree. HQ makes high-level decisions (troop movements, objectives) and hands each squad leader a **goal**, not a step-by-step order. The leader then works toward that goal autonomously over a longer period instead of waiting for HQ on every move.

- Each leader agent gets: its objective, its own squad state and local contacts (own fog of war), and a small command set for its squad: **move**, **attack**, **defend**, **report to HQ**, **cancel / fall back** when too dangerous, **request new or further instructions** from HQ.
- Runs its own agent loop (observe -> decide -> act) at a slow cadence, separate from HQ, with its own token budget; falls back to the rule behaviour when offline or over budget.
- HQ stays in charge of the big picture: it can reassign or recall a leader, and receives its reports and requests as SITREPs (section 4).
- Builds on M1 (LLM bridge) and the M2 order/mission model; fits the "full chain" echelon setting of M4.

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

## 5a. Freeform agent tools (open decision: A vs B)

> **Status: OPEN DECISION.** Two candidate approaches, not yet chosen: **(A)** extend the fixed vocabulary (section 5) with a handful of player-request commands (`FORM_SQUAD <composition>`, `REQUEST_RESUPPLY`, `REQUEST_RECON`, ...) that HQ grants or refuses under the same ledger, authority and validation rules; **(B)** the tool registry described below (optionally with the raw-command escape hatch). Evaluate both before committing: compare player experience, implementation cost, safety and token cost, ideally with a small prototype of (A) after M2 and a look at what Game Master functions are scriptable. Until decided, design the validator/executor so either can plug in.

Section 5 is the **predictable core**: a small fixed set of commands the Rules commander and the LLM both use for the war itself. Humans asking HQ things in natural language (chat now, voice later) need more: *"Can I get a jeep and 3 infantry for a recon?"* HQ must be able to reason, decide, and act with a broad set of game functions, as an agent (reason -> act -> observe -> reply). Example: grant the request by spawning or reassigning a jeep and three infantry, forming a new squad, handing it to the player as leader, and answering by chat.

**Three tiers, one validator/executor pipeline**

| Tier | What | Who may use it | Default |
|---|---|---|---|
| **1. Command vocabulary** | Fixed meta-commands (section 5) | Rules and LLM | on |
| **2. Tool registry** | Broad catalogue of **typed tools**, each with a name, description, parameter schema, authority level, cost and validator. The agent can list the registry (`list_tools`) and call any entry. Examples: `spawn_group`, `reassign_units`, `form_squad`, `assign_leader`, `create_task`, `place_marker`, `grant_supplies`, `set_group_behaviour`, `teleport_to_base`, `query_world` (read-only, fog-limited), `reply_to_player` | LLM (ReAct loop), triggered by player requests or by HQ's own planning | on |
| **3. Raw game command** | `execute_game_command("<name>", args)`: call a server/Game Master function by name when no typed tool exists | LLM, only with an **allowlist** | **off**; admin opt-in, always logged, dry-run first |

Recommendation: build tier 2 and grow the registry, with tier 3 as an escape hatch. Free-form code or arbitrary function execution is deliberately not offered: the LLM hallucinates, players try to abuse it, and a bad call on a multiplayer server is hard to revert. A typed registry gives the agent nearly the same reach while staying checkable. The tool catalogue is seeded from the Game Master / editor actions the base game already exposes `[verify what can be called from script]`, plus our own functions.

**Guard rails for every tool call**
- **Authority:** each tool has a minimum level (HQ-only, player request, admin). A player cannot make HQ do what only an admin could.
- **Grant logic:** requests are decisions, not entitlements. HQ weighs the war plan, the resource ledger (a jeep and 3 infantry cost supplies/manpower, same as everything else), priority, and the requester's role and trust; it may grant, partly grant, counter-offer, or refuse with a reason.
- **Budget and rate limits:** per-faction spawn budget and the AI cap (section 9) apply to tier-2/3 spawns too; per-player request cooldown.
- **Fog of war:** `query_world` returns only what the faction knows.
- **Validate, dry-run, log, revert** as in section 12; every call is recorded with who asked and why.
- **Human in the loop:** big or odd actions can ask an admin/commander to confirm.

**Where it applies**
- Player chat/voice requests (section 11): the agent parses intent, calls tools, replies in chat/radio.
- HQ's own planning may use tier 2 when the fixed vocabulary is too narrow (e.g. create a temporary quick-reaction force).
- The Rules commander stays on tier 1; it can answer a few canned player requests (request resupply, request recon, request reinforcement) through fixed intents, but open-ended requests are an LLM feature, with a polite "HQ cannot process that" fallback.

Open questions: which Game Master functions are scriptable from a mod; how to describe 100+ tools to the LLM without blowing the token budget (tool search / grouped catalogues / on-demand `list_tools`); how to scale trust for requests from low-rank players; whether tier 3 is worth shipping at all.

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
- **Human report channels (API checked 2026-10-11, not yet tested in play):**
  - A, marker parsing: `SCR_MapMarkerManagerComponent` syncs player markers to the server (`OnAddSynchedMarker`, `GetStaticMarkers()`); parse type/icon/text into a `Contact`.
  - Chat command: `SCR_ChatPanelManager.GetCommandInvoker("report")` registers e.g. `/report 3 inf 250 N`; cheapest channel.
  - B, quick menu: custom radial/layout UI, most work.
  - Order: A, then chat, then B (all M3). Contacts -> faction map markers (server `InsertStaticMarker` with faction flags, removed on expiry) is automatic and goes into M0.
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
- Natural-language requests from players are handled by the LLM agent, using whichever approach section 5a settles on (fixed request commands or a tool registry), and answers by chat/radio.

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

### 12a. HQ dashboard (build early: read-only first, control later)

Because the game already talks to the sidecar over HTTP, the sidecar can serve a **web dashboard** showing what each HQ knows and does. Build the basic version early; it also makes LLM debugging far easier.

- **Basic (early, read-only):** per faction: map with own squads, known contacts (age/confidence) and active orders; squad list (strength, mode, human/AI led); latest situation sent, raw LLM reply, validation drops, fallback events; request latency and token/cost counters. Fed by the `/orders` traffic the sidecar already sees (keep a ring buffer of recent situations/replies), served as a static page + JSON/SSE endpoint on the sidecar port.
- **Advanced (later, editable):** pause/resume the LLM per faction, switch commander mode (Rules / LLM), edit the prompt or settings live, issue or cancel orders by hand from the map, approve/reject LLM orders before execution, replay a timeline of the war. Needs an authenticated command channel back to the game (game polls the sidecar for queued commands, section 1) and must respect the same validation rules as LLM orders.

---

## 13. Roadmap

Own game mode `CLW_GameMode` (see `notes/decision-game-mode.md`). Live status: `PROGRESS.md`.

| Milestone | Contents | Proves |
|---|---|---|
| **M0 - Skeleton** | Game mode + minimal world, player can join a side and spawn as a soldier in an AI squad, one AI squad per side, per-faction contacts, RuleCommander issues MOVE/ATTACK/DEFEND waypoints, logs. Spikes: perception tap, order loop | Perception + waypoint APIs, core loop, playable without LLM |
| **M1 - LLM bridge** | REST spike (POST/headers/polling), sidecar, RemoteCommander, schema validation, fallback to Rules | LLM loop |
| **M1.5 - Dashboard** | Read-only web dashboard served by the sidecar (section 12a basic); editing/commands later | Observability |
| **M2 - Real war** | Player squad leaders receive HQ orders as missions (choose from offered assignments, accept/refuse/complete), more commands, SITREPs, win conditions, spawn budget, resource ledger, bases, supply convoys | AI vs AI with economy |
| **M3 - Mechanized & intel** | Player requests via chat, approach to be chosen (section 5a, option A or B); Vehicle groups, embark/dismount, convoys; map markers, expiry, player reports, setup screen | Vehicle AI, humans in the loop |
| **M4 - Scale & realism** | Chain-of-command echelons with delayed reports, population manager (cleanup, collapse/expand), FOBs | Size of war |
| **M5 - Content compat** | RHS / WCS profiles, role classifier | Custom factions |
| **Later** | Story mode (section 3a), Squad-leader LLM agents (section 4a), Voice/radio, virtual battlefield (experimental), Conflict adapter | - |

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
