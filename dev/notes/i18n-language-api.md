# Player language: script API research (2026-10-13)

Static research only (Script API search); nothing run yet.

- Client UI/game language: `WidgetManager.GetLanguage(out string languageCode)` (Enfusion, static; counterpart `SetLanguage`). It is the
  language the UI/stringtable runs in. Exact string format (`en`, `en_us`, `english`?) is UNVERIFIED: `CLW_PlayerLang.Normalize` cuts
  `xx_YY` / `xx-YY` to `xx` and rejects anything else (logged as `CLW: lang report ... ignored`).
  The log line `CLW: lang client reports game language '<raw>'` shows the raw value at e2e.
- Dedicated server: the DS has no UI/local player, so it never calls it. Each real client calls it and sends the value to the server
  via RPC (`CLW_LangPlayerController.c`, once, on the player's first controlled entity). A listen-server host calls the store directly.
- Persistent identity: `BackendApi.GetPlayerIdentityId(playerId)` (via `GetGame().GetBackendApi()`). Fallback key `n:<player name>` when empty.
- Fallback if the client report is unusable: server default (`overrides.json` `language`) then `en`; players can always set the override
  (`/hq language`, HQ menu) which wins over the report.
