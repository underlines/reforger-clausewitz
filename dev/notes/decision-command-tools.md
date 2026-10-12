# Decision: commands vs tool registry (VISION 5a) - decided 2026-10-12

Status: **approved by the owner 2026-10-12**. Research: `research/llm-tool-design-at-scale.md`.

## Answer: both, layered, with the registry as the mechanism
Not A or B. One **action registry** in the game is the single source of truth; what the LLM *sees* is a small, scoped,
intent-level slice of it. "Give the agent all tools" works in 2026 only because of tool search and scoped toolsets, not
by putting hundreds of schemas in the prompt (Anthropic: tool search +9..25 points accuracy, -85 % tokens; weaker models
fall apart fastest with many tools). None of those vendor features reach us through OpenRouter chat-completions, so
scoping and search live in our sidecar.

**Owner constraints (2026-10-12):** sources older than 2026 count as background only (agent patterns move fast);
the agent loop uses an existing framework, not a hand-rolled loop.

## Framework (2026 research, pass 2 of the research note)
**Vercel AI SDK core in the existing Node sidecar** (`ai` + `@openrouter/ai-sdk-provider` + `zod`): smallest thing that
gives us control, any OpenRouter model.
- Strategic tick: single shot, `generateText` + `Output.object(zod)`, no tools (can stay on raw `fetch` until wanted).
- Agent turn: `ToolLoopAgent`, ~4 steps max, `toolSearch` with our own ranker over deferred registry entries,
  `activeTools` for scoping; every tool's `execute` wraps authority / validation / budget. Async, replies by chat.
- Hidden behind `callModel` / `runAgentTurn`, so the SDK can be swapped.
- Rejected: Claude Agent SDK (spawns the Claude Code binary, ~1 GiB/session, tool search off on non-first-party base
  URLs), OpenAI Agents JS (tool search needs the Responses API, not available via OpenRouter chat-completions),
  smolagents (Python only, needs a real sandbox for code actions; second runtime), Mastra / ADK (more than we need).
- **No MCP server in the sidecar for now:** the stateless MCP spec (2026-07) can't scope tools per caller, and the 10 s game
  poll would stall the loop. Queued action tools return an accepted id at once (+ `command_status` read tool); reads
  carry `ageSec`. An MCP view can be added later for external clients.
- One planner with per-echelon instructions, not a model per echelon.
- Verified 2026-10-12 (phase A, official docs + the installed packages, pinned in `dev/tools/package.json`): `ai` **7.0.137** (Apache-2.0; npm `latest`, v6 is the `ai-v6` tag), `@openrouter/ai-sdk-provider` **3.1.0** (Apache-2.0, peer `ai ^7`), `zod` **4.6.6** (MIT; the SDK accepts 3.25.76+ or 4.1.8+). Node >= 20 (tested on 24).
  - API in v7: `new ToolLoopAgent({model, instructions, tools, stopWhen, activeTools, prepareStep, ...})` then `await agent.generate({prompt, abortSignal})`; default stop is `isStepCount(20)` (exported also under the old name `stepCountIs`), we use 4. `tool({description, inputSchema: zod, execute})`. `generate()` returns `text`, `steps[]` (each with `toolCalls`, `toolResults`, `usage`, `providerMetadata`).
  - **Tool search is built in and client side**: `toolSearch({search, maxResults})` plus tools marked `deferLoading: true`; deferred tools are not sent to the provider until a search matched them (matches become available on the next step). `search` is our own ranker (name/description token score). Works through OpenRouter chat completions, tested end to end (find_tools -> deferred REQUEST_ARTILLERY -> call). `activeTools` exists on the agent and per step (`prepareStep`) but is not needed with deferLoading.
  - **Cost**: `openrouter.chat(model, {usage: {include: true}})` puts the real cost in each step's `providerMetadata.openrouter.usage.cost` (USD); `step.usage` has the token counts. The sidecar sums the steps and feeds `recordUsage` (budget view). A typical HQ chat turn with `inclusionai/ling-3.0-flash` costs about $0.00003 (2 model calls, ~3.7k input tokens, mostly the tool schemas) and 2-6 s.
  - **Schemas**: zod tuples become JSON-schema `prefixItems`, which an upstream (DeepInfra) rejected ("invalid parameters schema"): use `z.array(z.number()).length(2)` for positions.
  - Structured output for the strategic tick (`generateText` + `Output.object`) is not verified or needed yet: the tick stays on raw fetch + JSON extraction. Per-model tool-calling reliability: the eval per OpenRouter model is still open (ling-3.0-flash called the right tool in 8 of 8 fixture messages, reply language followed the player).
  licenses. The sidecar has no `package.json` yet (zero-dependency today): adds one.

## The layers
| Layer | What | Who | Where |
|---|---|---|---|
| **1. Intent vocabulary** | ~10-15 operational commands (today MOVE/ATTACK/DEFEND/RECON/FALL_BACK + buy + missions; M3 adds RESUPPLY, ESTABLISH_FOB, CONVOY, ...). Some are **macros** (a step list over primitives, defined in `.conf`) | Rule commander + LLM strategic tick | single-shot JSON (protocol v2/v3), every 10-25 s |
| **2. Primitives / rare actions** | typed tools: `form_squad`, `assign_leader`, `spawn_vehicle_at`, `grant_supplies`, `place_marker`, `reply_to_player`, read-only `query_*` (fog-limited) | LLM **agent turn** only (player requests, exceptions) | capped plan-then-execute (max 4 steps), async, sidecar side |
| **3. Raw game command** | `execute_game_command` | nobody | dropped for now |

Fixed player requests (option A) stay as the **player-facing fast path**: `/request resupply|recon|reinforce|vehicle`,
the quick menu and building actions map to registry entries and work without the LLM (rule grant logic). Free text
goes to the LLM agent turn, which calls the same entries. One validator/executor for all paths.

## Registry entry (data, not code paths)
`name`, `layer` (intent | primitive), parameter schema, `authority` (hq | player | admin), `cost` (supplies/manpower),
`requires` (war-mode capabilities: `economy`, `fob`, `logistics`, `vehicles`, ...), `tags` (for scoping/search),
`executor` (script class) or `steps` (macro). The game sends the **manifest** for the current mode and faction to the
sidecar at war start, and the strategic tick's command list is generated from it, so prompt, validator and executor
cannot drift.

## Answers to the owner's concerns
- **Hundreds of tools overwhelm / micromanage:** the HQ tick never sees primitives, only intents; executors (rules) do the
  detail. Agent turns see ~20-30 tools filtered by tags + a sidecar `find_tools`.
- **Meta-commands:** macros in `.conf`; candidates promoted from logged LLM call chains (dashboard can list frequent
  chains), reviewed by hand.
- **Future game modes:** a mode declares capabilities; entries with unmet `requires` drop out of the manifest. New mode =
  new entries + config, no protocol change.
- **Model choice:** a small tool-selection eval per OpenRouter model (fixed situations -> expected intent), re-run when
  entries are added.

## Plan
M3 step 1: registry + manifest, port the existing commands/buy/missions onto it with no behaviour change. Then the M3
entries (requests, FOB, resupply/convoy), then the agent turn for chat requests.
