# Dedicated server smoke test (2026-10-12, build 0.4.0 #36)

Setup, scripts and commands live in `D:\GitHub\ReforgerMods\_dedicated\README.md` (git-ignored install: SteamCMD + server app 1874900,
game 1.8.0.13, same version as the owner's client on `E:\Games\steam\steamapps\common\Arma Reforger`).

## How the unpublished addon gets onto the server
- **Works:** `ArmaReforgerServer.exe -server Worlds/CLW_Arland.ent -addonsDir <_dedicated\addons> -addons C1A05E7B3D9F4A28 -profile ... -maxFPS 60 -logStats 10000`.
  `_dedicated\addons\Clausewitz` is a junction to `Clausewitz\addon`; the server reads LOOSE files (no pak, no Workbench pack run), compiles the scripts itself at start.
  `-server` ignores the json config, so no mission header and no `scenarioId` are needed. Default ports: game UDP 2001, A2S 17777.
- **Does NOT work with unpublished mods:** `-config x.json` (scenarioId). Three separate failures, all verified:
  1. `-config cannot be used together with addons!` (fatal): `-addons` is forbidden with `-config`; mods must come from `game.mods`.
  2. `game.mods` with our GUIDs: `Addon C1A05E7B3D9F4A28 - Addon was not found on workshop. ... Cannot start until they are removed from server config.`
  3. `game.mods: []`: `Unable to read the mission header '{...}Missions/CLW_Arland.conf'` (the addon is not loaded).
  So the `scenarioId` route only becomes possible after the mod is published on the Workshop. `server_launch`/`server_config` (enfusion-mcp) generate that json
  route, so they do not fit the unpublished case; the `server_*` tools were not used (read their schemas only).
- **No mission header exists** in the addon: `addon/Missions/` is empty (the folder holds nothing; `server_scenario_picker` lists 0 project scenarios).
  Needed for the Workshop/`-config` route and for the in-game server browser/host menu: `addon/Missions/CLW_Arland.conf`, class
  `SCR_MissionHeader` with `World "{A9806AF617972E98}Worlds/CLW_Arland.ent"`, `m_sName`, `m_sAuthor`, `m_sDescription`, `m_sGameMode`, `m_iPlayerCount`
  and a `SystemsConfig` (vanilla headers set one, e.g. `{8DDC2A311929D52F}Configs/Systems/GameMasterSystems.conf` for GM,
  `ConflictSystems.conf` for Conflict; pick/verify the one our world needs), created in Workbench (Resource Browser, new Config, class
  `SCR_MissionHeader`) so the `.meta` gets a real GUID. Workbench "Host/Play multiplayer" lists scenarios by header too. NOT created (addon untouched).
- Packing (`-wbModule=ResourceManager -packAddon`) not run: needs the Workbench exe, owner may have it open. Only needed for the Workshop release.

## What was verified (run 1, `-server`, fake sidecar on 127.0.0.1:8788, no players)
Log: `_dedicated\profile\logs\logs_2026-10-12_06-43-17\console.log` (copy kept as `keep_run1_server_world`).
- Boot ~25 s to game mode: addon found `gproj: .../addons/Clausewitz/addon.gproj guid: 'C1A05E7B3D9F4A28'`, `Compiling Game scripts` with only obsolete-API warnings
  (RestCallback OnSuccess/OnError/OnTimeout, SCR_JsonLoadContext, ImportFromString: already known). **No `SCRIPT (E)`, no VM exceptions in 5 minutes.**
- `CLW: build 0.4.0 #36`, `game mode started`, `scenario 'Arland' loaded ... 2 factions, 6 objectives`, bases/points/markers, `war SETUP phase ... autoStartSec=180`,
  setup server, mission board, squad join hooks, rule commanders for US/USSR, activity management ready.
- Server-side world calls work headless: `world locations found n=25`, `world roads sent n=147`, `world buildings sent n=460` (POSTs to the sidecar).
- Auto start without a human: `06:46:36 CLW: war auto start (no setup happened)` -> `war RUNNING mode=short US tickets=400 ...` -> start forces spawned
  (AI 19 -> 141, vehicles up to 8), 60.0 FPS steady (`Player: 0, AI: 140, Veh: 1..3`), memory 217 MB.
- Sidecar traffic: counters requests=5, gated=43, errors=0, fallbacks=0; `sitrep US/USSR new version 2 (source rules)` then `version 3 (source llm)` (fake provider),
  `orders US US_Mech1 ATTACK -> objective B [llm, 1]`, vehicle orders applied (`MOUNTED_IDLE`).
- Game REST needs no extra permission on the DS (backend unreachable is irrelevant: `BACKEND (E): Curl error ... GameConfig/List` appears at start, harmless).

## Noise / issues seen (none blocking)
- `WORLD (E): Unknown keyword/data 'Parent'` / `'SlidingTrackMaterial'` for BRDM2/BTR70/M151 vanilla prefabs (also in the Workbench log; vanilla data, not ours).
- `RESOURCES (E): Wrong GUID/name for resource system/wbdata/Gizmo3D/sphere.xob` (Blast_small_Invisible_Trigger, vanilla, Workbench-only data missing on DS).
- `SCRIPT (W): RestCallback: Function was not set for event 'OnSuccess'` repeats for the WorldInfo POSTs (the sidecar still receives the data); belongs to the
  already-known obsolete RestCallback overrides: moving to `SetOnSuccess/SetOnError` would silence it.
- `CLW: orders USSR USSR_Inf2 ATTACK C -> failed: waypoints lost, 787 m from the target (after 20 s)` once, 40 s after start (AI far from any human, LOD/activity); not seen in the Workbench log, worth a look in a long run.
- `spawn place infantry/cargo ... via fallback` for every start unit (no road/free-spot placement hit on the DS run); cosmetic, check whether it is the same in Workbench.
- A server started with `-config` and a bad key (`playerCountLimit`) is rejected by the schema (`maxPlayers` is the key).

## Not testable without a human (still open from PROGRESS.md)
Group mirror RPC (`CLW_GroupMirrorController`, fires 3 s after a player registers), remote commanding of squads by a client, `SetIsPlayable`
replication, SITREP delivered per side to a client (`CLW: sitrep deliver / client received`), setup menu RPC to the setup owner, player teleport at war start,
ticket HUD broadcast (`CLW_RpcAsk_WarStatus`), mission popups. With zero players the server only exercises the server half (above).
Next step: owner joins the running local server (below) and the checks are read from `console.log` (`CLW: sitrep deliver`, `squad groups mirror`, ...).

## How to join (owner)
Server and client are on the same machine/version (1.8.0.13). The game client does NOT need the mod packed; it loads loose files too.
1. Start the sidecar (README section "LLM sidecar"; fake or your OpenRouter config) and the server: `D:\GitHub\ReforgerMods\_dedicated\start-server.ps1`.
   Wait for `CLW: points base marker USSR` in `_dedicated\profile\logs\<latest>\console.log` (~30 s). The war auto-starts 180 s after that if nobody uses the setup menu;
   the first player who spawns becomes setup owner (menu opens), so join within 3 minutes to test the setup menu.
2. Close Workbench is NOT required, but do not start the client with the same profile folder at the same time as a Workbench Play session (single instance of the game profile).
   Start the CLIENT (Steam game exe, PowerShell):
   ```powershell
   & "E:\Games\steam\steamapps\common\Arma Reforger\ArmaReforgerSteam.exe" -window -addonsDir "D:\GitHub\ReforgerMods\_dedicated\addons" -addons C1A05E7B3D9F4A28 -client 127.0.0.1
   ```
   (BI wiki, "Startup Parameters", `-client`: "connect the game to a locally hosted dedicated server and load the local MyCustomMod addon:
   `-client -addonsDir "C:\MyModsDir" -addons MyCustomMod`".) Add `:2001` is not needed. If the client prints a data/script checksum mismatch, both must use the same
   files: restart the server after any script change.
   Alternative without `-client`: Steam launch options for Arma Reforger `-addonsDir ... -addons C1A05E7B3D9F4A28`, then Multiplayer > Direct connect (IP Connect) > `127.0.0.1:2001`
   (the server is `visible:false` / not in the browser; `-server` mode is not registered with the backend, which is unreachable anyway).
   Using the `ArmaReforgerSteamDiag.exe` client gives script error dialogs and the diag menu (`-rpl-validation-*-disable` flags work only on diag).
3. After spawn: expect the setup menu (owner), then war start, SITREP popup for your side, group list with HQ squads, map markers. Check on the server:
   `Select-String -Path _dedicated\profile\logs\*\console.log -Pattern "CLW: (sitrep deliver|squad groups|setup)"` and on the client `...\Documents\My Games\ArmaReforger\logs\...\console.log`.
4. Workbench as a client (peer): not verified here. Workbench's Play dropdown offers the multiplayer test flow (server + peer clients inside Workbench), which
   tests replication without a separate server; connecting Workbench to an external dedicated server was not found in the wiki and is not recommended. Use the game client above.
5. Stop: `D:\GitHub\ReforgerMods\_dedicated\stop-server.ps1`; kill the sidecar by PID.

## Addon-side changes wanted for DS / Workshop (not done)
1. `addon/Missions/CLW_Arland.conf` (SCR_MissionHeader, see above), plus mission-header `SystemsConfig`, so the mod can be hosted via `scenarioId` and the Host menu once published.
2. Optional: set RestCallback via `SetOnSuccess/SetOnError` (silences the DS log warnings).
3. For long DS runs a `-logStats` capable session with a client is the missing piece; consider `m_iAutoStartSec` shorter than 180 for headless soak tests (server overrides file is for catalog only).
