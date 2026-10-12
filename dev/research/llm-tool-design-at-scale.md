# LLM tool design at scale: meta-commands vs typed tool registry

Research for the open decision in `dev/VISION.md` 5a (A: fixed meta-command vocabulary, B: typed tool registry with a ReAct loop).
Date: 2026-10-12. Method: web research, primary sources preferred. Items marked `[secondary]` are blog/aggregator claims, not verified against the paper. Items marked `[inference]` are my reading, not a published result.

**Structure: sections 1-5 are pass 1 (2023-2025 sources, now BACKGROUND, see the re-check table in section 6). Sections 6-9 and the final list are pass 2 (2026 sources) and take precedence.**

## 1. (Background) How accuracy and cost scale with tool count

**Token cost (Anthropic, primary):**
- 5-server MCP setup = 58 tools = ~55K tokens before the first user message; Jira alone ~17K; Anthropic saw 134K tokens of definitions before optimisation. Per tool that is roughly 0.5-1K tokens for real-world MCP tools. [Introducing advanced tool use](https://www.anthropic.com/engineering/advanced-tool-use)
- "The most common failures are wrong tool selection and incorrect parameters, especially with similar names" (same source). Their own guidance: consider tool search when you have 10+ tools, >10K tokens of definitions, or selection errors; skip it for <10 tools or compact definitions.

**Accuracy vs. tool count:**
- Anthropic internal MCP eval with tool search on (large libraries): Opus 4 49% -> 74%, Opus 4.5 79.5% -> 88.1%. Token use ~77K -> ~8.7K (-85%). So even a frontier model lost ~10 points with everything loaded. (advanced-tool-use post above)
- RAG-MCP (Gan & Sun, May 2025): with all MCP tools in the prompt, selection accuracy 13.62%; with retrieval of a shortlist 43.13%, prompt tokens -50%+. Stress test, small/older models, many *similar* tools, so the absolute numbers are pessimistic; the direction is robust. [arXiv 2505.03275](https://arxiv.org/abs/2505.03275)
- LangChain ReAct benchmark (Feb 2025) `[secondary via StackOne]`: same task, tools of 1 domain vs 7+ domains in scope: GPT-4o fell to 2%, Llama-3.3-70B to 0%, o1 held ~71%. Reasoning models degrade much less. [StackOne summary](https://www.stackone.com/blog/agent-tool-count-ceiling/)
- AppSelectBench (Nov 2025) `[secondary]`: 100 apps, GPT-5 63.3%. Frontier models "resist longer, do not escape".
- OpenAI caps function definitions at ~128 per request `[secondary]`; practitioners report degradation beyond ~20 tools `[secondary, anecdotal: tianpan.co, kblip.com]`.
- BFCL v3/v4 (Berkeley) measures multi-turn, missing-function, missing-parameter and long-context separately; the "missing functions" and "long-context" splits are where models are weakest, i.e. knowing when no tool fits is hard. [BFCL V3 blog](https://gorilla.cs.berkeley.edu/blogs/13_bfcl_v3_multi_turn.html) (no clean accuracy-vs-N curve is published there).

**Practical thresholds** (synthesis `[inference]`):
- <= ~15-20 well-differentiated tools in context: fine for all current models.
- 20-50: needs good naming/namespacing and examples; frontier models OK, cheap models (Haiku/Flash/Llama class) start to mis-pick.
- 50+ or >10K tokens of schemas: use search/deferral or role-scoped subsets. The failure mode is silent (plausible adjacent tool), not an error.
- Confusability matters more than raw count: 100 clearly distinct tools beat 30 near-duplicates.

## 2. Best practice for large tool sets (2025 sources, background)

| Technique | What | Evidence | Portable via OpenRouter? |
|---|---|---|---|
| Tool search / deferred loading | Send only a search tool + a few hot tools; deferred defs expanded on demand | Anthropic: `defer_loading: true`, regex/BM25/custom search; -85% tokens, +9 to +25 pts accuracy; keeps prompt cache intact. OpenAI has an equivalent `tool_search` (hosted or client-executed), Responses API, gpt-5.4+ only. [OpenAI](https://developers.openai.com/api/docs/guides/tools-tool-search) | No. Vendor-specific, Anthropic Messages / OpenAI Responses. Chat-completions via OpenRouter has none. |
| Client-side tool search | You expose `find_tools(query)`; your code returns the matching schemas and re-sends `tools` next request | RAG-MCP 13.6% -> 43.1%; StackOne search+execute pair ~900 tokens vs ~150K for 500 tools | Yes. OpenRouter requires `tools` on every request ("so the router can validate"), so you just change the array per turn. |
| Programmatic tool calling / code mode | Model writes code that calls tools; only final output enters context | Anthropic PTC: avg tokens 43.6K -> 27.3K (-37%), fewer inference passes; Cloudflare "Code Mode"; MCP code-exec post: 150K -> 2K tokens (-98.7%) [post](https://www.anthropic.com/engineering/code-execution-with-mcp) | Concept yes (your own sandbox, e.g. a tiny DSL), Anthropic's hosted `allowed_callers` no. Needs a safe interpreter: heavy for a game server. |
| Namespacing / consolidation | `faction_`, `squad_`, `economy_` prefixes; fewer, broader tools | Anthropic: prefix vs suffix has "non-trivial" eval effects, vendor-dependent; measure it | Yes |
| Role/context-scoped toolsets | Expose only the subset valid for this actor and game phase | StackOne: "group by task, add search past a few dozen" | Yes (it is just the `tools` array) |
| Sub-agents / hierarchy | Planner delegates to narrow agents, each with 5-15 tools | Common pattern; multiplies LLM calls | Yes, but latency/cost scale with agents |
| Tool use examples | Few-shot per tool in the description | Anthropic: schemas cannot express conventions; examples help | Yes (put in description) |

Portable subset on OpenRouter (OpenAI-compatible): `tools` (JSON-schema functions), `tool_choice`, `parallel_tool_calls`, streaming tool calls, and optional interleaved thinking on supporting models; list supporting models via `?supported_parameters=tools`. Models vary in parallel-call quality, strict-schema adherence and max tool count. [OpenRouter tool calling](https://openrouter.ai/docs/guides/features/tool-calling)
Not portable: Anthropic `defer_loading` / tool search / PTC / `mcp_toolset`; OpenAI `tool_search`. So any search/deferral must be implemented in our sidecar.

## 3. (Background) Abstraction level: intent tools vs primitives

Anthropic, [Writing effective tools for agents](https://www.anthropic.com/engineering/writing-tools-for-agents) (Sep 2025), the main primary source:
- "More tools don't always lead to better outcomes." Common error: tools that merely wrap existing API endpoints.
- Consolidate frequently chained, multi-step operations into one tool: `schedule_event` (finds availability and books) instead of `list_users` + `list_events` + `create_event`; `get_customer_context` instead of three getters; `search_logs` instead of `read_logs`.
- "Too many tools or overlapping tools can also distract agents from pursuing efficient strategies."
- Return high-signal, human-readable fields; replace UUIDs with names or 0-indexed ids (measurably reduces hallucinated ids); offer a `concise`/`detailed` response_format (72 vs 206 tokens in their example).
- Build an eval from realistic multi-call tasks, track tool-call count/errors/tokens; repeated call sequences in transcripts are the signal to consolidate.
- Namespacing choices must be evaluated per model.

Micromanagement: the literature does not give a clean number, but the mechanism is documented. Low-level primitives force the model to do orchestration in tokens (one inference per step), and LLM-PySC2 reports LLMs "cannot constantly generate correct decisions, especially in the full pysc2 action space (hundreds of actions) and multi-agent settings; hallucinations and inefficient collaboration" ([NeurIPS 2025](https://papers.nips.cc/paper_files/paper/2025/hash/b9523d484af624986c2e0c630ac44ecb-Abstract-Conference.html)). Every game LLM paper below that works well gives the LLM macro/intent actions.

Learned macros / composite tools:
- Voyager: skill library of verified, executable, composable code skills retrieved by embedding; 3.3x more unique items, 15.3x faster tech milestones vs prior SOTA; skills transfer to new worlds. Action space = code, not primitive commands. [paper](https://arxiv.org/abs/2305.16291)
- Anthropic: repeated tool-call chains in eval transcripts are the signal to add a consolidated tool. `[inference]` For us: log the LLM's chains (offline), promote frequent ones to macros by hand; auto-learned macros on a multiplayer server need review/allowlist (Voyager had no adversarial players).

Takeaway: the "meta-command vs all tools" dichotomy is false. Best practice is layered: few high-level intent tools as the default surface, primitives behind search/escalation.

## 4. (Background) LLMs in games, RTS, wargames

| System | Action interface | Result / lesson |
|---|---|---|
| TextStarCraft II (NeurIPS 2024) [arXiv 2312.11865](https://arxiv.org/abs/2312.11865) | Text observation; action space = StarCraft primitives plus *macro actions* (each a sequence of primitives for a subgoal: produce units, build, research). Text-to-action adapter parses LLM text into moves. "Chain of Summarization": single-frame and multi-frame summaries, and *action queue extraction*: LLM emits several actions per round, executed over time | LLM agents beat built-in AI up to "Harder" (Lv5) as Protoss vs Zerg; the macro-action layer and summarisation were the enablers; LLM latency forced queued multi-action output |
| LLM-PySC2 (NeurIPS 2025) | Full pysc2 action set (hundreds), async query so latency constant w.r.t. agent count | Wins possible but unreliable; hallucination and poor collaboration in full action space (see section 3) |
| CivRealm LLM env [docs](https://bigai-ai.github.io/civrealm/advanced_materials/llm_agent.html) | Per-unit/city NL observations and a *restricted list of currently valid actions* in NL; BaseLang agent also has a "manual and history search" command (vector DB lookup) plus "final decision" | Valid-action filtering is built into the env "to reduce meaningless actions": scoped toolsets by state. One LLM per unit, so context per call stays small |
| Voyager (Minecraft) | Code as action space + growing skill library | See section 3 |
| COA-GPT (US Army Research Lab) [arXiv 2402.01786](https://arxiv.org/abs/2402.01786) | Text/image mission info in; doctrine excerpts in context; outputs COAs for human approval, "within seconds", refinable by commander feedback | Single-shot structured generation + human in the loop, not a tool loop |
| Command-Agent (2026) [ScienceDirect](https://www.sciencedirect.com/science/article/pii/S2214914725002776) | LLM integrated with warfare simulation for command decisions | Not read in depth; same pattern of LLM at command level over a simulator |
| Reforger LLM Squad (m0nklabs) [GitHub](https://github.com/m0nklabs/Reforger-LLM-Squad) | SITREP every 30 s -> LLM -> one of ENGAGE/HOLD/MOVE/FLANK/RETREAT/MOUNT/DISMOUNT; OPFOR "Stavka" LLM picks spawn/flank/hold/reinforce; Enforce mod talks HTTP to a Python bridge, local llama3.2-3b via Ollama proxy | Closest prior art: fixed small verb set, periodic situation report, no tool loop, small local model. Confirms feasibility; shows no evidence of a large registry |

Pattern across all of them: LLM acts at the macro/command layer on a slow tick; execution, pathing, micro are deterministic game code; the valid-action set is restricted per state; output is a batch/queue of commands rather than one step per inference. None of the cited systems exposed 100+ flat tools.

## 5. (Background) Latency and cost: ReAct loop vs single-shot JSON

Facts:
- Each ReAct step is a full inference pass; "hundreds of milliseconds to seconds" per round trip (Anthropic, advanced tool use). A 4-step loop with a mid-size model at ~2-6 s per step is 8-25 s, i.e. the whole 10-25 s tick budget, plus it re-sends the growing context each step (prompt caching mitigates the static prefix only).
- LLMCompiler (plan once, execute function calls in parallel): up to 3.7x lower latency, 6.7x lower cost, ~9% higher accuracy vs ReAct. [arXiv 2312.04511](https://arxiv.org/abs/2312.04511)
- ReWOO (plan without interleaved observations): 5x token efficiency, +4% accuracy on HotpotQA; allows offloading to a 7B model. [arXiv 2305.18323](https://arxiv.org/abs/2305.18323)
- Anthropic PTC: -37% tokens, +3-5 pts accuracy on their benchmarks, by moving orchestration into code.
- Tool search itself adds a round trip (Anthropic: "delivers best ROI when savings outweigh additional latency").

Hybrid patterns that fit a periodic tick `[inference]`:
1. **Plan-then-execute (ReWOO/LLMCompiler style):** one call returns a batch of intent tool calls (what we do today as JSON). Observations (validation errors) come back next tick, or one repair turn only on rejection.
2. **Two speeds:** periodic single-shot "strategic tick" on a cheap/fast model; event-triggered or player-request "agent turn" (ReAct, 2-5 steps, can take 20-60 s, async) on a stronger model. Player chat is already asynchronous so latency is tolerable there.
3. **Escalation:** default reply contains orders from a small core; if the model needs a rarely used action it emits `need_tools: ["fortify","artillery"]` or calls `find_tools`, and the sidecar re-prompts with the expanded subset (one extra call only when needed).
4. **Cache-friendly prefix:** keep system prompt + core tool defs byte-stable so provider prompt caching applies; put the changing situation last.
5. **Action queues:** like TextStarCraft's action queue, let one reply schedule multi-step sequences (macros) so the next LLM call is not needed to continue.

---

# Pass 2 (2026 sources only)

Owner steer: pre-2026 material above is background. This pass uses 2026 docs/papers and re-checks the earlier claims. Dates are from the pages read on 2026-10-12.

## 6. Re-check of pass 1 against 2026 sources

| Pass 1 finding | Status | 2026 evidence |
|---|---|---|
| Accuracy degrades with tool count; use search past ~10-50 tools | **Holds** | Claude Agent SDK docs now state "tool selection accuracy degrades with more than 30-50 tools loaded"; "50 tools can use 10-20K tokens"; one extra round trip per search; below ~10 tools load everything upfront ([tool search](https://code.claude.com/docs/en/agent-sdk/tool-search)) |
| Consolidate chained steps into intent tools | **Holds** (guidance unchanged) | Still the reference doc; all 2026 framework docs push tool search *in addition*, not instead |
| Tool search/deferral is vendor-specific, must be built in the sidecar | **Superseded** | Vercel AI SDK has provider-agnostic `toolSearch()` with `deferLoading: true` tools and a custom ranking callback ([doc](https://ai-sdk.dev/docs/ai-sdk-core/tool-search)). OpenAI's is Responses-API-only. Claude SDK's auto-disables on non-Anthropic base URLs (below). Building it yourself is now ~30 lines or one import |
| Code mode / PTC needs a sandbox, defer it | **Mostly holds** | AI SDK ships `experimental_codeModeTool` (experimental); still needs an isolate. Not needed for our tool counts |
| MCP: servers keep session state, per-connection tool lists | **Superseded** | MCP spec 2026-07-28 is stateless: no `Mcp-Session-Id`, no initialize handshake, `tools/list` "no longer varies per-connection", tasks moved to an extension, `subscriptions/listen` replaces GET stream ([changelog](https://modelcontextprotocol.io/specification/2026-07-28/changelog)) |
| LangChain 7-domain and RAG-MCP numbers (GPT-4o 2%, 13.6% -> 43.1%) | **Background only** | 2025 models; direction confirmed by 2026 docs but do not quote the numbers as current |
| Game LLM interfaces (TextStarCraft macros, CivRealm valid-action lists, Reforger-LLM-Squad) | **Background, not re-searched** | All 2023-2025. The 2026 additions are in section 9. Treat "macro layer + restricted valid actions" as design reasoning, not as a 2026 finding |
| ReAct loop costs a full inference per step; plan-then-execute is cheaper | **Holds** (reasoning) | 2026 benchmarks below show slow reasoning models failing time-sensitive tasks |

## 7. Framework choice (2026)

Our sidecar today: `dev/tools/llm_sidecar.mjs`, 1439 lines, **zero dependencies, no package.json**, plain `http` server and raw `fetch` to Anthropic or an OpenAI-compatible/OpenRouter endpoint, JSON extracted from reply text (no schema-enforced output, no tools), own cost tracking via OpenRouter `usage.cost`, own scheduler/budget. Any SDK adds `node_modules` and a build/packaging step.

Needs: (1) **tick**: single-shot structured output, any OpenRouter model, lowest latency. (2) **agent turn**: capped tool loop for player requests with our own tool scoping/search over the action registry and authority + validation on every call.

| | Claude Agent SDK (TS) | OpenAI Agents SDK (JS) | Vercel AI SDK 6 (`ai`) | Mastra | Google ADK (TS) | smolagents |
|---|---|---|---|---|---|---|
| What it is | Library that **spawns the Claude Code binary** as a subprocess per session | Agent loop, handoffs, guardrails, tracing, hosted multi-agent | Library: `generateText`, `ToolLoopAgent`, MCP client, tool search | Full TS framework: agents, workflows, memory, evals, server | Code-first agent toolkit, Node 20.19+ | Python library, ~1,000-line core, CodeAgent |
| OpenRouter / other models | Via `ANTHROPIC_BASE_URL=https://openrouter.ai/api` + token env vars ([OpenRouter doc](https://openrouter.ai/docs/guides/community/anthropic-agent-sdk)). Effectively Claude-protocol models only | Chat Completions mode works with any OpenAI-compatible endpoint; Responses-only features lost | `@openrouter/ai-sdk-provider` (`createOpenRouter`), any model string at runtime ([doc](https://openrouter.ai/docs/guides/community/vercel-ai-sdk)) | Model router, "40+ providers", `provider/model` strings | Gemini-first; other models via adapters (not verified) | LiteLLM (100+ LLMs) and OpenAI-compatible servers incl. OpenRouter |
| Tool search / deferral | Built in, **auto-disabled when base URL is not first-party** (proxies drop `tool_reference` blocks); `ENABLE_TOOL_SEARCH` override is untested through OpenRouter | `toolSearchTool()`, `deferLoading` **require the Responses API**; Chat Completions provider rejects them; AI SDK adapter does not support them ([models doc](https://openai.github.io/openai-agents-js/guides/models/)) | `toolSearch()` provider-agnostic, custom `search` callback, `maxResults`, honours `activeTools`; works with MCP tools | Not verified | Not verified | n/a (code agent sees tool stubs as Python functions) |
| MCP | In-process `createSdkMcpServer` + external | Yes (hosted and local) | `@ai-sdk/mcp` `createMCPClient`, HTTP transport recommended | Client and server | Yes | Yes (any MCP server) |
| Structured output for the tick | `outputFormat` JSON schema/Zod, SDK re-prompts on mismatch, error after retry limit | `outputType` | `Output.object(...)` with Zod, also on agents | Yes | Yes | Weak (code agents emit Python) |
| Sessions / memory | Sessions on local disk per subprocess, resumable; each session ~1 GiB RAM, 1 CPU, 5 GiB disk guidance ([hosting](https://code.claude.com/docs/en/agent-sdk/hosting)); `startup()` pre-warms | Sessions API, Responses conversation state (Responses-only) | Stateless; you pass messages (we already hold state) | Built-in memory | Sessions | Agent memory object |
| Latency overhead | Highest: subprocess + Claude Code system prompt/tools; pre-warm needed. Wrong shape for a 10-25 s single-shot tick | Low in Chat Completions mode | Lowest (thin wrapper over fetch) | Low-moderate | Low | Python process, plus sandbox startup if remote |
| Long-lived local server | Possible, heavy (one subprocess per session) | Yes | Yes (any Node server) | Yes (own server) | Yes | Yes, but a second runtime next to Node |
| License / deps | Anthropic Commercial Terms; ships a binary | MIT-style OSS (not re-verified); needs `openai` client | Apache-2.0 (not re-verified); small dep tree + provider pkg + zod | **Apache-2.0 core plus an `ee/` enterprise-licensed dir** (dual license, per repo README) | Apache-2.0 | Apache-2.0 |

### smolagents specifically
- Pattern: the model writes a Python snippet that calls tools, so loops/conditionals/composition happen in code (the 2025 research claim; the library also has a classic `ToolCallingAgent`). v1.26.0 in 2026, Apache-2.0 ([GitHub](https://github.com/huggingface/smolagents)).
- Sandboxing: default is a restricted in-process interpreter, which third-party analyses call a filter, not a boundary (same env vars, same network); remote executors are Blaxel, E2B, Modal, Docker ([2026 guide](https://www.pandastack.ai/blog/how-to-give-a-smolagents-agent-a-code-execution-sandbox/)). A Wasm option exists (`smolagents-capsule`, runs Python/JS in Wasm).
- Python only. Cost for us: a second runtime beside the Node sidecar, cross-process calls for every registry tool, and sandbox startup inside a 10-25 s tick. Players' chat text reaches the model, so generated code is attacker-influenced; a sandbox is mandatory, not optional.
- JS port: only a small community one, `ikangai/smolagents-js` (Feb 2026, ~340 lines, zero deps, browser-first via OpenRouter). Not a maintained equivalent. Reproducing the pattern in Node is feasible (QuickJS-Wasm isolate exposing registry tools as functions) and is exactly what AI SDK's `codeModeTool` provides experimentally.
- Verdict: wrong fit now. Code-as-action pays off when a task needs many chained calls; our agent turn is 1-4 intent calls with validation per call, where typed JSON tools give a cleaner authority/validation choke point.

### Other 2026 notes
- Claude Agent SDK is an *agent runtime for coding-style agents* (files, bash, permissions, hooks). We would carry that surface for a game HQ and lose tool search on OpenRouter by default. Strong only if we commit to Claude models and want its permission hooks and sessions.
- OpenAI Agents SDK tool search needs OpenAI Responses models (gpt-5.4+), contradicting "any OpenRouter model".
- Microsoft Agent Framework (.NET/Python) and TanStack AI / Effect AI SDK exist in the OpenRouter integration list; not evaluated (wrong runtime or immature for us).

### Recommendation
**Smallest thing that gives us control: Vercel AI SDK core (`ai`, `@openrouter/ai-sdk-provider`, `zod`) in the existing Node sidecar; no big agent SDK.**
- Tick: `generateText` + `Output.object(zodSchema)` (no tools) against any OpenRouter model. Replaces regex JSON extraction with schema-enforced output and typed retries. Keep our scheduler, budget and cost tracking; read `usage.cost` from provider metadata (verify it is exposed, otherwise keep raw fetch for the tick).
- Agent turn: `ToolLoopAgent` (default 20 steps, set `stopWhen: isStepCount(4)`), tools built from the registry, `toolSearch({search: ourRanker})` for deferred non-core entries, `activeTools`/`prepareStep` for scope by actor and game mode, and **`execute` wrapping authority + validation + budget**. Every call passes our choke point regardless of framework.
- Migration is incremental: the tick can stay on raw `fetch` until the structured-output benefit is wanted. Packaging cost: introduce a `package.json` (the sidecar has none today).
- Pin the version, wrap the SDK behind `callModel(kind, ctx)` and `runAgentTurn(ctx)` so swapping to Claude Agent SDK / OpenAI later touches one file.
- Revisit Claude Agent SDK only if: we standardise on Claude models, need its sessions/permissions/hooks, and tolerate ~1 GiB per concurrent session.

## 8. Action registry as an MCP server inside the sidecar?

Idea: game posts manifest; the sidecar exposes it as MCP; tool calls queue for the game's next poll; read-only tools answer from the latest snapshot.

What 2026 sources say:
- Spec 2026-07-28 made MCP stateless. Servers needing cross-call state mint explicit handles passed as ordinary tool arguments; `tools/list` must not vary per connection; deterministic list order recommended for prompt-cache hits; results carry `ttlMs`/`cacheScope`; async work uses the **tasks extension** (poll `tasks/get`). ([changelog](https://modelcontextprotocol.io/specification/2026-07-28/changelog))
- Governance of MCP calls (OWASP MCP cheat sheet, Microsoft "control plane" and Governance Toolkit posts, Apr 2026) centres on least-privilege scoping, credential minimisation, approval and audit across model trace, server and downstream user: i.e. what our authority/ledger layer already is.
- Microsoft (Sep 2026) frames the choice as "agent per specialist" vs "one orchestrator that loads skills/instructions and calls typed MCP tools"; the second avoids extra model executions ([post](https://devblogs.microsoft.com/agent-framework/from-specialist-agents-to-distributed-skills-over-mcp/)).
- I found no 2026 write-up of anyone exposing a live game or simulation this way; treat this as an untested pattern.

Pitfalls specific to our design `[inference]`:
1. **Poll latency breaks observe-act loops.** The game polls every 10 s. A tool that waits for execution blocks the agent step for 10+ s. Queued tools must return immediately ("accepted, id=...") after sidecar-side validation against the snapshot, with a separate `command_status(id)` read tool. Optimistic acceptance, results visible next tick.
2. **Snapshot staleness.** Read-only tools answer from the last POST; include `ageSec` in every result so the model does not act on stale contacts.
3. **Per-actor scoping cannot live in `tools/list`** (no longer per-connection). Scope via separate endpoint paths per role/mode, or pass `actor`/`mode` as arguments validated server-side, or build the tool array per call in-process (no MCP).
4. **MCP hop adds nothing in-process.** With the AI SDK the registry maps straight to `tools`; an MCP layer only adds JSON-RPC overhead and an HTTP listener. Its value is external clients (Claude Code or Inspector for debugging, future third-party agents). Cheap compromise: generate the MCP server from the same registry later.
5. **Manifest changes mid-run:** list-changed notifications moved to `subscriptions/listen`; deterministic ordering and stable defs matter for caching. Only register entries on game start/manifest change.
6. **Idempotency and replay.** Dropped streams lose in-flight requests (resumability removed): every queued command needs a client-supplied id so a retry does not double-spawn.
7. **Authority is not an MCP feature.** Tokens/roles must be enforced per call in the executor; model-visible descriptions must never be the only guard (prompt injection via player chat).
Recommendation: do not start with MCP; keep the registry as an internal module with an MCP adapter as an optional later view.

## 9. 2026 patterns for periodic planning agents

- **Gaia2** (ICLR 2026 oral, [arXiv 2602.11964](https://arxiv.org/abs/2602.11964)): benchmark in environments that evolve independently of the agent, with time constraints. Best overall GPT-5 (high) 42% pass@1 but "fails on time-sensitive tasks"; Claude-4 Sonnet trades accuracy and speed for cost; best open model (Kimi-K2) 21%. Lesson: heavy reasoning loops lose when the world keeps moving; match model effort to the tick.
- **AgenticCache** (MLSys 2026): cache plans and update asynchronously because the synchronous plan-act loop is the bottleneck. Supports the "single-shot tick + queued multi-step orders" design.
- **Echelons:** per Microsoft's 2026 guidance, a model per specialist is justified only when the specialist needs autonomy, own context and a substantial workflow. For a faction HQ the cheaper default is one planner with echelon-specific *instructions/skills* loaded per scope; real subagents (squad-leader agents, VISION 4a) come later and run async on their own tick.
- **Background agents:** the pattern in coding agents (long task on its own thread, results injected later) maps to our player-request turn: run async, reply in chat when done, never block the tick.
- Not found for 2026: a published RTS/wargame LLM interface comparison newer than the 2025 papers above. State this as a gap.

## Implications for Clausewitz (updated)

Unchanged from pass 1 (still supported):
- Layered design: fixed small intent vocabulary on the tick (core, always loaded, ~10-15 commands incl. macros); registry internally with `core`/`tags`/`modes`; macros as data; cap visible tools ~20-30 (2026 SDK docs: degradation beyond 30-50).
- Primitives only in agent turns behind authority/rate limits; raw `execute_game_command` off by default; validate every call; return names not ids, with `ageSec`.

New or changed:
- **Framework: Vercel AI SDK core in the Node sidecar** (OpenRouter provider, `Output.object` for the tick, `ToolLoopAgent` + `toolSearch` with our ranker for agent turns). No Claude Agent SDK (subprocess, ~1 GiB/session, tool search off via OpenRouter), no OpenAI Agents SDK (tool search needs Responses/OpenAI), no smolagents (Python + sandbox), no Mastra/ADK (more framework than needed; Mastra has an `ee/` license split).
- Wrap the SDK behind two functions (`callModel`, `runAgentTurn`); keep our scheduler, budget and cost tracking; verify cost metadata survives the SDK before migrating the tick.
- Tool search is now cheap to adopt portably: implement it only when the non-core registry passes ~30 entries; below that use `activeTools` scoping.
- MCP inside the sidecar: not now. If added, use stateless HTTP, queue-and-ack semantics (command ids, `command_status`), no per-connection scoping, snapshot reads carry `ageSec`.
- Agent turn budget: <= 4 steps, async, answer by chat; tick stays single-shot structured (Gaia2: slow reasoning loses on time-sensitive tasks).
- Open verification items before building: `usage.cost` via `@openrouter/ai-sdk-provider`; structured output reliability per target OpenRouter model (some models lack strict JSON schema); exact licenses of `ai` and `@openai/agents` (not re-read); `ENABLE_TOOL_SEARCH` override behaviour on OpenRouter (untested).
