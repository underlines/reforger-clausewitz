# Multilinguality (i18n): design (2026-10-13, not started)

Goal: every player reads the mod in their own language. Mod text and the sidecar dashboard ship in EN + DE (more later);
LLM text follows each player's language, any language. Decided with the owner 2026-10-13.

## 1. Kinds of text
| Kind | Examples | How |
|---|---|---|
| Static UI | setup menu, HUD, HQ menu, journal titles | stringtable keys `#CLW-...` in layouts and scripts, translated on the client |
| Server-built templates | rule SITREP, mission offers, request acks, end screen, rule commander messages | server sends **key + params** (RPC), client formats with `WidgetManager.Translate` + `%1..%n` |
| LLM text | `/hq` replies, LLM SITREP prose, HQ announcements | generated or translated into the player's language (section 4) |
| Dashboard UI | sidecar web dashboard labels, views, settings | EN + DE dictionaries in the browser (section 6a) |
| Never translated | logs, debug chat, command names (`/sitrep`, `/hq`), conf ids, action manifest, LLM reasoning, exchange/debug log | English |

Text built on the server is never sent finished: the server has no client language, clients may differ.
Content names (factions, places, units) still come from content display names (`#AR-...` keys, pack strings), never hard-wired.

## 2. Stringtable (EN + DE)
- `addon/Language/CLW.st`, English source, German column; keys `#CLW-<Area>_<Name>` (e.g. `#CLW-Sitrep_Holding`).
- `CLW_Loc`: `Fmt(key, params)`; serializable `CLW_LocMsg { string key; array<string> params; }` for RPCs. A param that is a
  key (`#AR-Faction_US`, place display names) is translated too (as `CLW_Places` does today).
- More languages later = one more stringtable column, no code change.
- Check: `ui_localization_audit` / `ui_extract_strings` find hard-coded strings left in layouts and scripts.
- A player override to a language without a stringtable column (e.g. FR) keeps the game language for mod text (Enfusion
  fallback); LLM translation of templates is deferred.

## 3. Player language (resolved on the server)
`/hq language <code>` or HQ menu option  ->  client game language (sent once on join)  ->  server default (`overrides.json`
`language`)  ->  `en`.
- ISO 639-1 codes, case-insensitive (`/hq language DE` = `de`); `/hq language auto` clears the override.
- Stored per player identity, kept across reconnects (profile file next to `overrides.json`).
- The override only steers LLM text; an English game can get German HQ messages.
- To verify: the script API call that gives the client's UI language. Fallback if none: the HQ menu option only.

## 4. LLM text: one rule
- **One reader** (`/hq` reply, request acks): the strong LLM writes `reply` directly in the asker's language, in the same
  structured response as its actions. One call, no translation.
- **Many readers** (SITREP prose, side-wide announcements): the strong LLM writes once in English (canonical). The sidecar
  collects the distinct languages of the recipients and makes one weak-LLM translation call per non-English language.
  Cost grows with languages, not players. Cache by `(text hash, lang)`.
- Decisions and text are never split: the weak LLM translates prose only and never decides anything.

## 5. Strong / weak LLM (sidecar)
- Config gets two provider slots, `strong` and `weak` (provider, model, base URL, key, timeout), both in the dashboard
  Configuration view. Every LLM call names a `role`; for now only `translate` maps to `weak`. Moving other cheap calls later
  is a mapping change.
- Fallbacks: no weak slot -> strong translates; translation error or > ~10 s -> canonical English, flagged in the exchange log.
- Fake provider: "translation" = `[<lang>] <text>`, so e2e can check routing without a key.

## 6. Protocol v4 (game <-> sidecar)
- `/hq` request: `lang`. Response: `reply` in that language.
- Broadcast-producing calls (SITREP, announcements): request `langs[]` (recipient languages); response
  `text_by_lang { en: ..., de: ... }` per text field. The server routes each player their version, `en` if missing.
- v3 stays accepted (no `lang` = English).

## 6a. Dashboard (EN + DE)
- `dev/tools/dashboard/i18n/en.json` + `de.json` (flat `key: text`, `{0}` params); `t(key, ...params)` in `app.js`/`journal.js`;
  static HTML via `data-i18n="key"` (and `data-i18n-title` / `-placeholder`), applied on load and on switch.
- Language: picker in the header (EN/DE), stored in `localStorage`, default from `navigator.language`, fallback `en`;
  missing key -> English, logged once to the console.
- Only the dashboard chrome is translated. Game data shown (SITREPs, chat, missions) is shown as stored: canonical English,
  plus a per-view language toggle where `text_by_lang` exists. Logs stay English.
- More languages later = one more JSON file.

## 7. Phases
1. Language resolution, `/hq language`, HQ menu option, join report.
2. `CLW_Loc` + `CLW.st` EN/DE; static UI and server-built templates moved to keys.
3. Protocol v4 `lang` for one-reader replies.
4. Weak slot + broadcast translation + cache.
5. Dashboard EN/DE (independent of 1-4, can run in parallel).
