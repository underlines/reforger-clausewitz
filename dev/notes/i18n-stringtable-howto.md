# Stringtable how-to (P2 research, 2026-10-13)

Short facts for the Clausewitz i18n work. Design: `i18n-design.md`. Code: `addon/Scripts/Game/CLW_Loc.c`.

## How Reforger stringtables work
- **Source `*.st`** (String Editor, Resource Manager > Editors > String Editor): one item per key, class `CustomStringTableItem`
  (script `scripts/Game/Language/CustomStringTableItem.c`), one member per language: `Target_en_us`, `Target_de_de`, `Target_fr_fr`,
  `Target_it_it`, `Target_es_es`, `Target_cs_cz`, `Target_pl_pl`, `Target_ru_ru`, `Target_ja_jp`, `Target_ko_kr`, `Target_pt_br`,
  `Target_zh_cn`, `Target_uk_ua` (+ `Target_en_us_edited`, `Comment`, `Status`, ...). An empty language column falls back to English.
  Key naming: `<Game>-<Area>_<Name>`; vanilla `AR-...`, ours `CLW-<Area>_<Name>`. In layouts, configs and scripts a key is written with a
  leading `#`: `#CLW-Setup_Title`. The `.st` is NOT read by the game.
- **Runtime tables `*.<lang>.conf`** are what the game loads (String Editor: Table > Build Runtime Table, Ctrl+B). Format VERIFIED
  against vanilla `Language/localization.de_de.conf` (read from the pak):
  ```
  StringTableRuntime {
   Ids {
    "AR-Account_Login"       // keys without '#', sorted case-insensitively
   }
   Texts {
    "Anmelden"               // parallel array, same order; \" for a quote
   }
  }
  ```
- **Registration** in the mod's `*.gproj`, per platform configuration, `WidgetManagerSettings > StringTables > StringTableDefinition`:
  `StringTableSource "{GUID}Language/X.st"` + `Languages { LanguageDefinition { Code "de_de" StringTableRuntime "{GUID}Language/X.de_de.conf" } }`
  (vanilla `ArmaReforger.gproj` is the reference). Language codes are `en_us`, `de_de`, ... The game picks the column of the
  game language; a language without a definition shows the English one. Workbench options (Widget Manager Settings > String Tables) write the same.
  Our `addon.gproj` registers `en_us` + `de_de` under `PC`; `HEADLESS : PC` so the dedicated server has the table too.
- **Use**: any `TextWidget`/`RichTextWidget.SetText("#KEY")` and layout `Text "#KEY"` is translated by the engine.
  `WidgetManager.Translate(text, p1 .. p9)` translates in script and fills the params; `%1..%9` in the text are the params,
  `%%` is a percent sign. No `%` in LLM/other text that goes through a translation layer (the mod's `CLW_Sitrep.DisplaySafe` handles that).
  A text that does not start with `#` is shown as it is. Diag menu > UI > Disable > Disable Loc shows the raw keys (to find missing ones).
- Numbers, dates and percentages are not formatted by the table: pass them as params.

## What the mod does
- Single source: `dev/i18n/CLW.keys.txt` (`KEY ::: English ::: German`, KEY without `#`). `node dev/i18n/gen-stringtable.mjs`
  writes `addon/Language/CLW.st`, `CLW.en_us.conf`, `CLW.de_de.conf` (+ the 29 map-marker size labels `CLW-Marker_V<v>_I<i>`).
  `--check` validates (duplicates, empty texts, `%n` mismatch between EN and DE). More languages = one more column in the script
  (`LANGS`), one more `.conf` and one more `LanguageDefinition` in `addon.gproj`.
- The `.meta` files carry hand-picked GUIDs (`7D3A91C45E4000xx`), registered resources use `{GUID}path`.
- **Server text is sent as key + params**, never finished: `CLW_Loc.W("#CLW-Key", p1, ...)` builds the "wire"
  (`#CLW-Key^p1^p2`), any existing string RPC carries it, the client renders it (`CLW_Loc.Resolve` / `Fmt`).
  Params starting with `#` are translated too (faction names `#AR-Faction_US`, nested sentence parts). Details in the header of `CLW_Loc.c`.
- Dashboard / sidecar text (rules SITREP JSON, mode param labels) is rendered on the server with the same table (English).

## Unverified (check once in Workbench)
- The `.st` item layout (`CustomStringTableItem <KEY> { Target_en_us "..." Target_de_de "..." }`) is written from the script class and
  the wiki; the String Editor may want a header (Target Prefix `Target_`, Item Class Name `CustomStringTableItem`). Open `CLW.st` once;
  if it does not load, create a new GUI string table with the String Editor, paste the items, then Build Runtime Table (that regenerates the confs).
- `HEADLESS : PC` and `WidgetManager.Translate` on the dedicated server (the server renders English text for the sidecar/dashboard).
