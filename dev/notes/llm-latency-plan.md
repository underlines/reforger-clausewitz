# LLM latency plan (next milestone)

Owner request 2026-10-15, after the DS e2e: the commander call takes ~20-40 s and times out on `inclusionai/ling-3.1-flash`
(timeoutSec 20 -> fallback gets the rest -> `orders=0`, rules do all the work). `/hq` was moved to its own fast route (WP11) already.

## Evidence (synthetic probe, ~50 KB situation-like prompt, JSON reply for 30 squads)
| model | in tok | out tok | time |
|---|---|---|---|
| ling-3.1-flash | 15.6k | 2.4k | 22.3 s |
| ling-3.0-flash | 15.6k | 2.5k | 11.9 s |
| gemini-2.5-flash | 15.5k | 1.9k | 19.2 s |
| gpt-4.1-mini | 13.1k | 1.2k | 8.0 s |
Tiny prompts: all < 1.5 s (API healthy). Latency is dominated by OUTPUT tokens (sequential decode) x model decode speed;
input (prefill) matters less. Confirm with real numbers (step 1) before tuning.

## Steps (likelihood / payoff)
1. **Measure** (certain, cheap): per LLM call log + dashboard: input/output tokens, time to first token, total ms, prompt size per
   section (system, manifest/commands, situation keys, journal, SITREP rules). Baseline for everything below.
2. **SITREP out of the commander call** (high, ~30-50% less output time, future-proof): own call every 3-5 min, cheap/weak model,
   parallel; input = situation + the commander's last orders/reasons (no information lost vs. the combined prompt).
3. **Terse orders output** (high, ~30-60% fewer output tokens): short keys, unit ids, reason capped (~12 words), structured
   output / JSON schema or a single `issue_orders` tool call (also removes the "no JSON object in the reply" rejections).
4. **Split by responsibility and cadence** (medium-high, most future-proof): orders (frequent, small, fast model) / planner for
   buys, logistics, FOBs (every few min or on supply/front change) / SITREP + journal (occasional, cheap) / HQ radio (done).
   Each prompt carries only its own rules and data; new features add calls instead of growing one prompt. Cross-links via
   shared state (orders see "planned buys" and the FOB plan of the last planner run).
5. **Lean situation input** (medium; cost, some speed, better attention): deltas between full snapshots; relevance filtering
   (commandable units, near/recent contacts, catalog without descriptions or a catalog id seen once); tabular encoding
   (header + rows instead of repeated objects, ~30-50% fewer tokens).
6. **Conditional prompt sections + prefix caching** (medium): FOB / logistics / help-call / env rules only when active;
   stable parts (system, manifest) first, volatile situation last, so provider prompt caching (OpenAI/Anthropic/Gemini via
   OpenRouter) cuts time to first token and cost.
7. **Model tiering per call type** (high for speed): fast model for routine ticks, strong only on important events or for the
   planner; per-call timeout and fallback.
8. **Prompt budget test** (future-proofing): a test builds prompts from a recorded large situation and fails when a section
   exceeds its token budget, so features must fit or justify more.

## Open: /hq fast path does not reach the sidecar in game (DS e2e 2026-10-15, build #119)
`CLW: hq #1 fast path failed (timeout), situation path stays in charge` after 30 s, and the sidecar logged nothing for it.
A direct `curl POST /hq` against the same sidecar answered in 1.4 s (`hq fast ... 1200ms`), so the route works. Likely cause:
the game's RestContext (CLW_SidecarLink.Context, shared with the commander POST /orders) runs requests one at a time, so /hq
queues behind the ~40 s orders call. Fix first: a separate RestContext for /hq (verify how GetRestApi().GetContext keys contexts),
then retest. Steps 2 + 3 below also shrink the queue time.

Update (same night): fixed with one RestContext per channel (`CLW_SidecarLink.Context(timeout, channel)`: hq, cmd-<faction>;
sidecar strips `/ch/<name>/`). /hq then answered in 3-5 s (report + cancel worked). One more failure: msg 3 hung in the sidecar because
the HQ agent shares the strong slot with the commander; the commander's timeouts put that model on cooldown, the HQ turn went to the
backup model and the call never returned (fixed: hard cap on the whole turn). Still to do: give the HQ agent its own slot
(fast model, own cooldown) so commander failures never touch /hq (fits step 7).

## Test checklist (DS e2e, RHS_USAF vs RHS_AFRF, 72 packs)
- [x] 1. `/hq 2 enemy infantry about 100m north of my position` -> fast path, REPORT_CONTACT 2 inf 100 m N, grid 025-018 (2026-10-15, build #120; 18 s right after war start)
- [ ] 2. `/hq wir werden beschossen, brauchen Hilfe` -> REQUEST_BACKUP, nearest free group sent. Failed once: HQ turn hung on the backup model (hard cap added since); re-test after the HQ slot split (step 7).
- [x] 3. `/hq cancel my last report` -> REPORT_CANCEL removed the report at grid 025-018 (5 s)
- [ ] 4. `/backup taking fire` (chat, no LLM) -> group sent. The radial "Backup!" already passed (RHS_USAF_Car3 sent, 191 m).
- [ ] 5. After steps 1-3 of this plan: commander call on ling-3.1-flash well under 10 s, `orders>0` again (sidecar log `orders faction=... <ms>`).

Recommendation: 1 + 2 + 3 first (small, high payoff, low risk; expected commander call on ling-3.1-flash well under 10 s), then 4
with 5 + 6 inside it; 7 + 8 as standing practice. Implementation via Sonnet subagents (disjoint files), lead does the DS e2e.
