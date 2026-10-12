# Research: faction SITREP and player briefing (2026-10-12)

Question: which real military report formats to borrow, and which vanilla Reforger UI hooks can show a short briefing to players
(deploy screen, in game) without building a custom layout. Design built on this: `notes/sitrep-design.md`.

## 1. Real formats (summary; sources at the end)
- **US/NATO SITREP**: header (unit, date-time group, reporting period) then situation (location, friendly, enemy, environment),
  operations (progress, significant events, casualties), intelligence, logistics (ammo, fuel, food, medical shortages), personnel
  strength, sometimes commander's assessment / next 24 h plan.
- **SMEAC / five-paragraph order**: Situation, Mission, Execution, Administration & Logistics, Command & Signal. Same thing as the
  vanilla Reforger journal entry types: `SCR_EJournalEntryType` = Situation, Mission, Execution, Signal, Intel, Custom (titles
  `#AR-RespawnMap_FRAGO_*`). So the vanilla briefing UI is already SMEAC shaped.
- **SALUTE** (enemy report): Size, Activity, Location, Unit, Time, Equipment. Used for the Enemy section (size, kind, location,
  age; the contact book has count, kind, position, age; activity/unit are not known to the rules, the LLM may add them).
- LACE (liquids, ammo, casualties, equipment) is the logistics checklist; we map it to supplies, caps, weak units.
- Decision: five sections **Situation, Enemy, Mission, Execution, Logistics** + one headline line. Short (<= 420 chars each,
  ~1.2 k total). Squad level later reuses the same sections (own strength, local contacts, task, next step, ammo).

## 2. Vanilla UI hooks (verified by reading the vanilla scripts in the pak, no Workbench run yet)
Paths below are inside the game data (`game_read scripts/Game/...`; lowercase `scripts/` works).

| Hook | What it is | Verdict |
|---|---|---|
| **Map journal** `scripts/Game/Map/ComponentsUI/SCR_MapJournalUI.c` | Tool-menu "!" entry on the map. Entries come from `SCR_RespawnBriefingComponent` (`m_sJournalConfigPath` -> `SCR_JournalSetupConfig`, entries per faction key, empty key = default). `MapSpawnMenu.conf` AND `MapFullscreen.conf` contain `SCR_MapJournalUI`, so it shows on the **deploy screen map and the in-game map**. `m_bShowJournalOnStart` opens it automatically on first map open. Entry text is a RichText (`<br/>` breaks). | **Chosen as the full SITREP.** Only needs the component on the game mode prefab + a small conf. |
| `SCR_RespawnBriefingComponent.RewriteEntryMain(factionKey, entryId, text, params)` | **Local** rewrite of an entry (updates the widget in place if open). `RewriteEntry_SA` (server) broadcasts to everybody = enemy leak, so we do NOT use it. | We call `RewriteEntryMain` on each client with only that client's faction text. `GetJournalConfig(key)` falls back to the empty-key journal, so one data journal serves every faction (no faction keys in data). |
| **Hint system** `scripts/Game/Components/Hints/SCR_HintManagerComponent.c` | `ShowCustomHint(description, name, duration)` (static), `RepeatHint`, `Toggle`. The component binds input action **`HintToggle`** (vanilla "H"): hides the hint or shows `m_LatestHint` again; `HintContext` opens the Field Manual link. Present in `GameMode_Base.et`. | **Chosen as the short popup.** The custom hint becomes `m_LatestHint`, so H re-shows it. Caveat: any later vanilla hint replaces it; hints can be disabled in settings (fallback: chat line). |
| Field Manual (`UI/Menu/FieldManual`) | Static pages, opened from pause menu / hint context. | Not used (static content, `EFieldManualEntryId` enum is code-fixed). |
| `SCR_PopUpNotification.PopupMsg` | Short toast (already used for mission popups). | Too short for a SITREP; fine for "new SITREP". Not used. |
| `SCR_ConfigurableDialogUi` | Modal dialog (used by our setup menu). | Would pause/block play; rejected for an in-game popup. |
| Chat command invoker | `/war` already registered the same way. | `/sitrep` re-shows the hint at any time (fallback when H shows another hint). |
| `SCR_WelcomeScreenComponent` | Welcome screen with server info at join. | Not studied further; no per-faction dynamic text. |

Custom input actions would need a modded `chimeraInputCommon.conf` (action + default key + context): intrusive and
unverified, so no new key; H + map journal + `/sitrep` cover "reachable like the in-game help".

## 3. Open verification items for the e2e run
- The "!" journal tool appears on the deploy screen map and the in-game map (needs `SCR_RespawnBriefingComponent` loaded; the log
  line `CLW: sitrep journal unavailable` appears otherwise).
- `RewriteEntryMain` updates the open journal text; `<br/>` renders as a line break (RichText), headline shows in "Situation".
- Hint description (a `TextWidget`) renders `\n`; if not it shows one line (harmless).
- `HintToggle` key default is H (read from the action name, not from the input config).
- `GetCommandInvoker("sitrep")` accepts a new command name (same call as "war").

## Sources
- US Army SITREP examples / format summaries (web search 2026-10-12): irp.fas.org/doddir/army/fm34-35/Appc.htm (SALUTE),
  trngcmd.marines.mil FGHT 1004 "Introduction to the Operations Order" (five-paragraph order),
  unitedtaskforce.net/handbook/group/219/smeac-5-paragraph-operations-order (SMEAC).
- Vanilla scripts read from the pak: SCR_RespawnBriefingComponent.c, SCR_JournalConfig.c, SCR_MapJournalUI.c, SCR_HintManagerComponent.c,
  SCR_DeployMenuBase.c, Configs/Map/MapSpawnMenu.conf, Configs/Journal/JournalConflict.conf.
