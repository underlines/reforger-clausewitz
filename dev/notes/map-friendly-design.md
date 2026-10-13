# Friendly units on the player's map: setup switches (2026-10-13, owner request; spike DONE, decided design + spec)

Replaces the single "See friendly map markers" level of VISION 8a with four independent on/off switches in the setup menu.
Status: research spike finished 2026-10-13 (vanilla scripts read with `game_read`, nothing run in game). Items tagged **[verify]** must be confirmed
on the dedicated server + a human client (test plan, section 6). Labels: **[SRC]** = read in vanilla scripts/configs, **[hyp]** = inference.

## 1. The four switches (war-mode params, `CLW_ModeParam.CreateBool`, setup menu + `/war start key=value` + overrides.json preset)
| key | label | shows on the map of a player of faction F |
|---|---|---|
| `mapMe` | Show me | the player's own position (+ heading) |
| `mapSquadmates` | Show squad members | every member (AI + human) of the player's own group, individually |
| `mapSquads` | Show friendly squads | ONE symbol per friendly group (at its leader), no individual members, human-led and AI groups |
| `mapVehicles` | Show friendly vehicles | every friendly vehicle with crew or claimed by a vehicle unit (symbol by kind from data, not hard-wired) |

- Independent, combinable. `mapSquads` does not imply `mapSquadmates`/`mapMe`. Dedupe: the player's own group is drawn as members when
  `mapSquadmates` is on, else as one squad symbol if `mapSquads` is on. A squad whose leader sits in a vehicle that is drawn (`mapVehicles` on)
  is not drawn again as a squad.
- Per war (mode param), same value for all players. The enemy is never shown beyond known contacts (existing `CLW_ContactMarkers`). No leak: the
  server only sends a client what its switches allow, and only its own faction's data.
- **Defaults (owner decides): proposal `mapMe=true, mapSquadmates=true, mapSquads=false, mapVehicles=false`** (squad-level picture without
  the commander's overview, no free radar). Presets via overrides.json `setup`: "realism" = all false, "relaxed" = all true.
- Not in scope: per-player in-map toggle (later), enemy side, arrows for orders (VISION 8a tier 2 stays backlog).

## 2. Facts established by the spike (answers to the 4 questions)

### 2.1 What vanilla draws on the map (and how to switch it off)
Map configs: `Configs/Map/MapFullscreen.conf` (gadget map "M", `EMapEntityMode.FULLSCREEN`), `MapFullscreenCoop.conf`, `MapSpawnMenu.conf` (deploy
screen, SPAWNSCREEN) and the station maps (`MapOperationsStation.conf` etc. inherit MapFullscreen) all list `SCR_MapMarkersUI` in
`m_aUIComponents`; `MapEditor.conf` (Game Master map) does NOT [SRC]. Everything below therefore only exists where `SCR_MapMarkersUI` runs; the GM map is
untouched by this feature (GM sees the editor's own entity view).

| thing | vanilla behaviour [SRC] | how we switch it off |
|---|---|---|
| **local player** | **Nothing draws it.** `SCR_MapMarkerSquadLeader.UpdateLocalVisibility` / `SCR_MapMarkerSquadMember.UpdateLocalVisibility` explicitly hide the marker of the local player; `Character_Base.et` has no `MapDescriptorComponent`; `MDT_UNIT` descriptors are `m_bIsDefaultInvisible` (debug only, `SCR_MapDebugUI.ShowUnits`); Steam thread 2022 "player position indicator": players cannot see their position. **[verify in play: open M, no own arrow]** | nothing to suppress. Fallback if an engine self marker (`MDT_CURPOS`, imageset index 2 in `MapDescriptorDefaults.conf`) shows up: per layer `m_MapEntity.GetLayer(i).GetPropsFor(factionMapId, EMapDescriptorType.MDT_CURPOS).SetVisible(false)` (pattern of `SCR_MapDebugUI.SetPropsVisible`) on `SCR_MapEntity.GetOnMapOpen()`. |
| **group leaders** | `SCR_MapMarkerEntrySquadLeader` (listed in `Configs/Map/MapMarkerConfig.conf`) spawns one replicated `SCR_MapMarkerSquadLeader` entity per PLAYABLE group (`SCR_GroupsManagerComponent.GetOnPlayableGroupCreated`; our `CLW_SquadGroups` registers HQ AI squads there, so AI squads qualify [hyp]). Streamed only to the marker's faction (`SCR_MapMarkerManagerComponent.SetMarkerStreamRules`). The client shows it only if the local player is IN a group: a leader sees other leaders (`CanLeaderSeeOtherLeaders`), members see other leaders (`CanMemberSeeOtherLeaders`, prefab attrs default 1), own group's leader always. Flag + callsign widget. A group-less player sees none. | modded `SCR_MapMarkerSquadLeader.UpdateLocalVisibility()` (public, called from `OnUpdate` when `m_bDoLocalVisibilityUpdate`): when our layer is active keep the vanilla marker only if `mapSquads`, or `mapSquadmates` and `GetGroup()` is the local player's group; else `SetLocalVisible(false)` WITHOUT calling super (super's `SetLocalVisible(true)` would create and delete the widget on every update). Inactive (no CLW war / no header yet): call super = vanilla untouched. |
| **group members** | `SCR_MapMarkerEntrySquadMember` + `SCR_MapMarkerSquadMember` exist (only the group LEADER sees human members) but **the entry is not in `MapMarkerConfig.conf`**, so `InsertDynamicMarker` returns null: inert in vanilla. A pack could add it. | same pattern: modded `SCR_MapMarkerSquadMember.UpdateLocalVisibility()`: visible only if `mapSquadmates`. |
| friendly AI groups / vehicles | not drawn at all (only through the leader marker above). Conflict base/task icons (`SCR_MapCampaignUI`, `SCR_MapUIElementContainer`) are unrelated and stay. | n/a |
| our own server markers | `CLW_ContactMarkers`, `CLW_StructureMarkers`, `CLW_TaskMarkers`: static markers with faction flags. | unchanged |

Refresh after the switches arrive (map open or markers already created): iterate `SCR_MapMarkerManagerComponent.GetInstance().GetDynamicMarkers()`
and call `UpdateLocalVisibility()` on every `SCR_MapMarkerSquadLeader` / `SCR_MapMarkerSquadMember`.
Never touch `SCR_MapMarkersUI`, the vanilla config files or the replicated markers (a client cannot delete server markers anyway).

### 2.2 Mechanism: decision
| | A. client-local static markers (`PLACED_MILITARY`, local, never replicated) fed by one RPC picture | B. server static markers per unit | C. native `MapEntity.CreateCustomMapItem()` | D. new `SCR_MapUIBaseComponent` |
|---|---|---|---|---|
| bandwidth | one picture per client / 4 s (about 1-2 KB) | each `InsertStaticMarker`/`RemoveStaticMarker` is a **Broadcast RPC to every client** (`RPC_DoAddStaticMarker`); a move = remove + insert (what `CLW_ContactMarkers` does) => 2 broadcasts per unit / 4 s to ALL clients, plus the JIP `RplSave` of every marker | same as A | same as A |
| leak to enemy | none (server addresses faction clients) | markers reach every client, filtered client-side after arrival | none | none |
| per-player content (me, own group) | natural | impossible (faction-wide only) | natural | natural |
| JIP | next picture (<= 4 s) or `Resend` | `RplSave` of all markers | same as A | same as A |
| look | vanilla NATO frame + type icons + faction colour + label (same widget as contact markers) | same | imageset quad only, no NATO frame | own widgets/layout; the component list is data in `MapFullscreen.conf`, a mod cannot add a component without replacing vanilla map configs |
| risk | needs the small local insert helper below | bandwidth, marker-ID churn | poor symbol set | patching vanilla confs breaks other mods |

**Decision: A.** Local `SCR_MapMarkerBase` objects of type `PLACED_MILITARY` (built with `SCR_MapMarkerManagerComponent.PrepareMilitaryMarker`, as
`CLW_ContactMarkers` does server-side) inserted LOCALLY and owned by the client layer `CLW_FriendlyMapClient`. Why it works [SRC]:
- `SCR_MapMarkerManagerComponent.Update(timeSlice)` runs every frame while the map is open (`SCR_MapMarkersUI.OnMapOpen` -> `EnableUpdate(true)`) and calls
  `marker.OnUpdate(visibleMin, visibleMax)` for every static marker, which re-reads `m_iPosWorldX/Y` and repositions the widget (`WorldToScreen` + `FrameSlot.SetPos`).
  So moving a marker = `marker.SetWorldPos(x, z)` on the SAME object: no remove/insert, no widget churn.
- `SCR_MapMarkersUI.CreateStaticMarkers()` (map open) calls `OnCreateMarker(true)` for every listed marker with faction flags 0, so locally held markers survive
  map close/open without work of ours. Faction flags 0 also survives `OnPlayerFactionChanged` (it removes only markers whose flags exclude the new faction).
- Out-of-view culling and the widget handling are vanilla (`TestVisibleFrame`, `SetStaticMarkerDisabled`).
- Performance: about 70 markers (128 AI = about 14 squads per side + vehicles) is far below what players place; per-frame cost = 70 x `WorldToScreen`. Text is hidden above layer 1 by `SCR_MapMarkerEntryMilitary.OnMapLayerChanged`.

**Local insert helper (do NOT use `InsertStaticMarker(marker, true)`)**: it stamps the local player id as owner (marker becomes editable/deletable by the player:
`SCR_MapMarkersUI.OnInputMapSelect` opens the edit dialog on click, the radial menu offers delete) and calls `OnCreateMarker()` unconditionally, which would create
widgets in the stale `GetMapMenuRoot()` when the map is closed. Instead a modded `SCR_MapMarkerManagerComponent` (new file, `CLW_` prefixed methods; the lists are protected) does
what `OnAddSynchedMarker` does: `marker.SetMarkerOwnerID(-1)`, `m_aStaticMarkers.Insert(marker)`, and `marker.OnCreateMarker(true)` only if
`SCR_MapEntity.GetMapInstance().IsOpen() && mapEnt.GetMapUIComponent(SCR_MapMarkersUI)`. Removal: `marker.OnDelete()` and `RemoveItem` from BOTH lists (vanilla
`RemoveStaticMarker` only removes from the enabled list; a culled marker would stay as a ghost in `m_aDisabledMarkers`).
After each picture while the map is open: `SetStaticMarkerDisabled(marker, false)` for our markers (the next frame's `OnUpdate` culls out-of-view ones again) so a unit that moved
into view appears without a pan. Known vanilla quirk: `CheckMarkersUserRestrictions` blocks ALL static markers for accounts without the UGC privilege (parental controls), same as our contact markers.

**"Me" needs no server data**: the client draws its own controlled entity (`SCR_PlayerController.GetLocalControlledEntity()`: position + yaw) from a 0.5 s `CallLater` while the map
is open; the server only tells it the switch. Symbol: `PLACED_CUSTOM` marker (not NATO): icon index = first entry of `SCR_MapMarkerEntryPlaced.GetIconEntries()` whose quad is `arrow-small`
(fallback `dot-2`, then 0; found by name at runtime, never by a hard-wired index; via `GetIconResource`), colour entry named `white` or `orange` (fallback 0), `marker.GetMarkerComponent().SetRotation(yawDeg)`.
Removed when there is no controlled entity (dead / spawn screen).

### 2.3 Server data (reuse, no new world scans)
Per faction F, once per tick (4 s), only for factions with at least one connected recipient:
- **Squads**: `CLW_GameMode.GetRegistry()`: `GetIds(F, ids)`; skip `IsHidden(id)` (linked cargo groups); `Find(id)` -> `SCR_AIGroup`; kind `GetKind(id)` (`CLW_ForceKind`, "" -> `infantry`); name `GetName(id)`;
  alive `CLW_SquadInfo.AliveCount(group)`; position `CLW_SquadInfo.LeaderEntity(group).GetOrigin()` (player-led squads keep the human as leader; falls back to the slave leader).
  Human-led = `group.GetPlayerCount() > 0`; the label gets the leader's player name (`CLW_SquadInfo.HumanLeaderEntity` -> `PlayerManager.GetPlayerIdFromControlledEntity` -> `GetPlayerName`).
  Human-only groups (players grouped in the vanilla menu, no AI, not in the registry): `SCR_GroupsManagerComponent.GetInstance().GetPlayableGroupsByFaction(factionObj)` where `!registry.IsAdopted(g)` and `GetPlayerCount() > 0`: kind `infantry`, position = leader entity, label = leader name.
- **Members of the receiving player's own group**: `groups.GetPlayerGroup(pid)`; `CLW_SquadInfo.Members(group)` (AI incl. the slave group) + `group.GetPlayerIDs()` -> `GetPlayerControlledEntity`.
  Only `ECharacterLifeState.ALIVE` (dead and incapacitated drop out at once), never the receiver himself. Built per recipient (about 10 entities).
- **Vehicles**: (a) `CLW_GameMode.GetVehicleManager().GetUnits(F, units)`: `unit.GetVehicle()` (skip null), `unit.GetKind()` (`CLW_ForceKind`), `unit.GetVehicleTypeName()` (data name, e.g. from the catalog),
  `unit.GetCrewAlive()`; claimed vehicles stay while the unit is not dissolved, even if dismounted. (b) vehicles of human players of F that no unit owns (depot request, hijacked, flown): the player's entity
  `SCR_CompartmentAccessComponent.GetVehicle()` (root parent), de-duplicated by entity. Kind of (b): `CLW_FriendlyKinds.OfPrefab(prefabName)` from editor labels
  (`SCR_EditableEntityUIInfo.ExtractEditableUIInfoFromPrefab(prefab).GetEntityLabels()`): `VEHICLE_HELICOPTER` -> `air_rotary`, `VEHICLE_AIRPLANE` -> `air_fixed`, `VEHICLE_APC` -> `mech`,
  `TRAIT_ARMOR` -> `armor`, `VEHICLE_TRUCK` + `TRAIT_LOGISTICS` -> `logistics`, `VEHICLE_TRUCK` -> `motorized`, `TRAIT_ARMED` -> `light`, default `motorized`; cached per prefab. Same label logic as `CLW_ForceCatalog`
  (about lines 1119-1160); no prefab names in code, so RHS/WCS vehicles classify the same way. Unoccupied and unclaimed vehicles are never shown.
- Cost: 128 AI = about 14 registry squads per side; `AliveCount` is 9 x `FindComponent` per squad per 4 s; trivial. Log `CLW: friendly tick` with entry counts and ms (`System.GetTickCount`).

### 2.4 Symbols
Faction colours come from `MapMarkerConfig.conf` `SCR_MapMarkerEntryMilitary` [SRC]: `EMilitarySymbolIdentity` BLUFOR (blue), OPFOR (red), INDFOR (green), UNKNOWN (orange). Our
`CLW_ScenarioConfig.GetIdentity(factionKey)` returns it; the server puts the int into the picture header (the client has no scenario config). Dimensions: `LAND`, `AIR`, `INSTALLATION` (no sea).
Type flags (`EMilitarySymbolIcon`, combinable bit flags, the widget shows up to 2): `INFANTRY, MOTORIZED, ARMOR, ANTITANK, MORTAR, ARTILLERY, FIXED_WING, ROTARY_WING, RECON, SUPPLY, MAINTENANCE, MEDICAL, GRENADE, MACHINEGUN, SNIPER, LIGHT, MOBILEHQ, RELAY, RESPAWN, AIRBORNE, AMPHIBIOUS, SIGNAL, COMBINED`
(there is NO `MECHANIZED`, which is why `CLW_ContactMarkers.IconFor` falls back on every call). One table `CLW_FriendlySymbols.Resolve(kind, isSquad, out dimension, out iconFlags)`:
| kind (CLW_ForceKind / extra) | dimension | icon flags |
|---|---|---|
| `infantry` (default, members, unknown) | LAND | INFANTRY |
| `recon` | LAND | RECON |
| `motorized` | LAND | MOTORIZED (squad: INFANTRY\|MOTORIZED) |
| `light` | LAND | MOTORIZED\|RECON |
| `mech` | LAND | INFANTRY\|ARMOR |
| `armor` | LAND | ARMOR |
| `armored_recon` | LAND | ARMOR\|RECON |
| `logistics` | LAND | MOTORIZED\|SUPPLY |
| `air_rotary` / `air_fixed` | AIR | ROTARY_WING / FIXED_WING |
| `at` / `mortar` (later squad roles) | LAND | ANTITANK / MORTAR |

Enum members are resolved by name at load (`typename.StringToEnum(EMilitarySymbolIcon, "ARMOR")`; 0 -> skip that flag, log once) so a renamed member cannot break compilation.
Labels: human-led squad = "<squad name> (<leader player name>)"; AI squad = squad name; vehicle = data name (+ player name); member = player name or empty (AI).
For a non-default tint use `marker.GetMarkerComponent().SetColor(color)` after creation.

## 3. Architecture (decided)
```
setup menu / preset --> CLW_FriendlyMapSettings (4 bool mode params, mask)
                              | read (server)
 4 s CallLater --> CLW_FriendlyPicture.Tick()  per faction: squads + vehicles (cached); per recipient: own-group members
                              | per recipient: only if the wire changed / never sent
                              v Rpc(Reliable, Owner)  SCR_PlayerController.CLW_FriendlySend(wire)   (listen host: direct call)
                      CLW_FriendlyMapClient.Receive(wire) -- diff by key --> local PLACED_MILITARY markers (SetWorldPos / create / delete)
                              |                                              + "me" marker from the local entity (0.5 s)
                              +--> modded SCR_MapMarkerSquadLeader / SquadMember: suppress vanilla by the switches
```
- The picture is sent when its text differs from the last one sent to that player (positions quantised to 5 m so stationary units cost nothing), on join / faction change / `Resend`.
  An entry-less header-only picture is still sent (the client needs the switches). Rematch / war end: `RematchReset()` sends an "inactive" header to everyone: the client deletes all markers and falls back to vanilla.
- The wire carries the switch mask in EVERY picture (JIP-safe, no second channel). `active=0` (non-CLW scenario, between wars): the client layer does nothing, vanilla stays intact.
- GM / spectator / faction-less players get nothing (no faction => no picture) and keep the editor's own view.
- The only client->server RPC is `CLW_FriendlyRequest` (resend); the server takes the player id from the controller, never from a parameter.

### 3.1 Wire format v1 (one string, `CLW_Sitrep.ToWire` style; free text sanitised: `| ; ,` and control chars -> space, max 24 chars)
```
F1|<version>|<factionKey>|<active 0/1>|<switchMask>|<identity>|<count>#<entry>;<entry>;...
entry = <t>,<key>,<x5>,<z5>,<h10>,<kind>,<n>,<fl>,<label>
  t      m member | s squad | v vehicle
  key    stable id: squad = registry id ("US_G12"), vehicle = unit id or "p<playerId>" for player-driven non-unit vehicles, member = "<groupId>.<index>"
  x5,z5  world X / Z in 5 m units (int)  (12.8 km world => <= 2560)
  h10    heading in 10 degree steps 0..35
  kind   CLW_ForceKind string or air_rotary / air_fixed
  n      alive members (squad) / crew (vehicle) / 1 (member)
  fl     bit0 human-led or human-occupied, bit1 own group, bit2 damaged (reserved)
switchMask bits: 1 me, 2 squadmates, 4 squads, 8 vehicles        identity = int(EMilitarySymbolIdentity)
```
Size: about 35 bytes per entry; a 14-squad + 12-vehicle + 8-member picture is about 1.2 KB, capped at `MAX_ENTRIES = 120` / 4000 chars (squads first, then vehicles, then members; warn when capped).
Bandwidth: 1.2 KB / 4 s / client = 0.3 KB/s per client; 32 clients about 10 KB/s from the server; nothing goes to enemy clients. **[verify]** maximal string size of a Reliable RPC parameter (the SITREP wire reaches 2-3 KB without problems);
fallback if 4 KB fails: chunk as `F1/<part>/<parts>` (not built).
Client reconcile: `map<string key, CLW_FriendlyMarkerState>`; same key + same (kind, label, count) => `SetWorldPos` only; changed => delete + create; key missing => delete.

## 4. Work packages (disjoint files; one coder each; no e2e inside a WP, the owner compiles + tests at the end)
All new files in `addon/Scripts/Game/`, prefix `CLW_Friendly`. Log lines start with `CLW: friendly ...`. Repo style (tabs, early return), null guards everywhere, `// verify` where flagged.

### WP-A  Setup params, keys, integration hooks
Owns: `CLW_FriendlyMapSettings.c` (new), `CLW_WarMode_ShortConflict.c` (one line), `CLW_WarRuntime.c` (one line), `CLW_Rematch.c` (two lines), `dev/i18n/CLW.keys.txt` (+ regenerated `addon/Language/CLW.st`, `CLW.en_us.conf`, `CLW.de_de.conf` via `node dev/i18n/gen-stringtable.mjs`).
```c
class CLW_FriendlyMapSettings
{
	static const string KEY_ME = "mapMe", KEY_SQUADMATES = "mapSquadmates", KEY_SQUADS = "mapSquads", KEY_VEHICLES = "mapVehicles";
	static const int BIT_ME = 1, BIT_SQUADMATES = 2, BIT_SQUADS = 4, BIT_VEHICLES = 8;
	static void AddParams(notnull array<ref CLW_ModeParam> params);   // 4 x CreateBool(key, "#CLW-Param_<key>", "#CLW-Param_<key>_Desc", default); defaults me=true squadmates=true squads=false vehicles=false
	static int Mask();       // via CLW_GameMode.GetWarState().GetMode().GetIntParam(key, default) (BOOL = 0/1, same pattern as CLW_EconomyGuard.HqReserve); 0 when no war / mode
	static bool Has(int bit);
}
```
- `CLW_WarMode_ShortConflict` constructor: `CLW_FriendlyMapSettings.AddParams(m_aParams);` after the cap_ params.
- Hooks: in `CLW_WarRuntime` next to `m_Vehicles.Init(m_Registry)` (about line 317): `CLW_FriendlyPicture.Start();`; in `CLW_Rematch` next to both `CLW_SitrepStore.RematchReset();` calls: `CLW_FriendlyPicture.RematchReset();`.
- Keys (EN / DE): `CLW-Param_mapMe` Show me / Mich anzeigen, `_Desc` "Your own position and heading on the map." / "Deine eigene Position und Blickrichtung auf der Karte."; `CLW-Param_mapSquadmates` Show squad members / Gruppenmitglieder anzeigen ("Every member of your own group, AI and players." / "Jedes Mitglied deiner eigenen Gruppe, KI und Spieler."); `CLW-Param_mapSquads` Show friendly squads / Befreundete Trupps anzeigen ("One symbol per friendly group at its leader." / "Ein Symbol je befreundeter Gruppe an ihrem Anfuehrer."); `CLW-Param_mapVehicles` Show friendly vehicles / Befreundete Fahrzeuge anzeigen ("Crewed or claimed friendly vehicles, symbol by type." / "Besetzte oder zugeteilte befreundete Fahrzeuge, Symbol nach Typ.").
- Verify the setup dialog renders the bool rows generically (no UI code expected) and that `/war start mapSquads=true` and the overrides.json `setup` object set them.
- Does NOT create `CLW_FriendlyPicture.c` (WP-B); compile gate is the merged whole.

### WP-B  Server picture builder + wire + RPC
Owns (all new): `CLW_FriendlyWire.c`, `CLW_FriendlyPicture.c`, `CLW_FriendlyKinds.c`, `CLW_FriendlyPlayerController.c`.
```c
// CLW_FriendlyWire.c (server encodes, client decodes; unknown version => FromWire returns null)
class CLW_FriendlyEntry { string m_sType; string m_sKey; int m_iX5; int m_iZ5; int m_iH10; string m_sKind; int m_iCount; int m_iFlags; string m_sLabel; }
class CLW_FriendlyPic
{
	static const int FLAG_HUMAN = 1, FLAG_OWN_GROUP = 2, FLAG_DAMAGED = 4;
	string m_sFaction; int m_iVersion; bool m_bActive; int m_iMask; int m_iIdentity; ref array<ref CLW_FriendlyEntry> m_aEntries = {};
	string ToWire();
	static CLW_FriendlyPic FromWire(string wire);       // null when unreadable
	static string Clean(string text, int maxChars);
}
// CLW_FriendlyPicture.c (server only, static)
class CLW_FriendlyPicture
{
	static const int TICK_MS = 4000;  static const int MAX_ENTRIES = 120;
	static void Start();                        // server guard; CallLater(Tick, TICK_MS, true)
	static void Tick();
	static void Resend(int playerId);           // forget the sent text of that player, deliver now
	static void RematchReset();                 // inactive header to everyone, clear caches
}
// CLW_FriendlyKinds.c
class CLW_FriendlyKinds { static string OfPrefab(string prefabName); static string OfVehicle(IEntity vehicle); }   // label based, cached (2.3)
// CLW_FriendlyPlayerController.c  (pattern of CLW_SitrepPlayerController)
modded class SCR_PlayerController
{
	void CLW_FriendlySend(string wire);                                                              // Rpc(CLW_FriendlyRpcAsk_Show, wire)
	[RplRpc(RplChannel.Reliable, RplRcver.Owner)] protected void CLW_FriendlyRpcAsk_Show(string wire);  // -> CLW_FriendlyMapClient.Receive(wire)
	void CLW_FriendlyRequest();                                                                      // client -> server
	[RplRpc(RplChannel.Reliable, RplRcver.Server)] protected void CLW_FriendlyRpcAsk_Request();         // CLW_FriendlyPicture.Resend(GetPlayerId())
}
```
Tick algorithm: (1) `mask = CLW_FriendlyMapSettings.Mask()`; no war => nothing. (2) recipients = connected players with a faction that is `CLW_ScenarioConfig.IsClwFaction`, grouped by faction (loop shape of `CLW_SitrepStore.SyncPlayers` / `GetFactionPlayers`); none => return.
(3) per faction with recipients build the shared lists ONCE: squads (if mask has squads) and vehicles (if mask has vehicles), section 2.3. (4) per recipient: entries = [own-group members if SQUADMATES] + squads (minus the own group when SQUADMATES, minus squads riding a drawn vehicle when VEHICLES) + vehicles;
cap; quantise x/z to 5 m and heading to 10 degrees BEFORE comparing; encode with `m_bActive = true`; compare with the last wire sent to that player ignoring the version; send only on change.
(5) deliver like `CLW_SitrepStore.Deliver`: host (`playerId == SCR_PlayerController.GetLocalPlayerId()`) => `CLW_FriendlyMapClient.Receive(wire)` directly, else `controller.CLW_FriendlySend(wire)`; no controller => forget so the next tick retries. (6) forget disconnected players.
Log: `CLW: friendly tick <faction> squads=n vehicles=n recipients=n sent=n bytes=n ms=n` (only when something was sent; otherwise every 10th tick). Never include data of another faction. All lookups null-guarded (group deleted between ticks).

### WP-C  Client map layer + vanilla suppression
Owns (all new): `CLW_FriendlyMapClient.c`, `CLW_FriendlySymbols.c`, `CLW_FriendlyMapManager.c`, `CLW_FriendlyMapSuppress.c`.
```c
// CLW_FriendlyMapClient.c (client only: System.IsConsoleApp() => return)
class CLW_FriendlyMarkerState { SCR_MapMarkerBase m_Marker; string m_sKind; string m_sLabel; int m_iCount; }
class CLW_FriendlyMapClient
{
	static void Receive(string wire);                 // FromWire -> store -> Apply(); log "CLW: friendly client received v<n> <faction> entries=<n> mask=<m>"; active=0 => RemoveAll + vanilla restored
	static bool IsActive();                           // an active picture was received and the local faction key still equals m_sFaction
	static int Mask();
	static bool KeepVanillaLeader(SCR_AIGroup group); // !IsActive || mask squads || (mask squadmates && group is the local player's group)
	static bool KeepVanillaMember();                  // !IsActive || mask squadmates
	static void RefreshVanilla();                     // UpdateLocalVisibility() on all vanilla dynamic leader/member markers (also on map open and local faction change)
	protected static void Apply();                    // reconcile map<string, CLW_FriendlyMarkerState> with the picture (3.1); then SetStaticMarkerDisabled(marker, false) when the map is open
	protected static void UpdateMe();                 // 0.5 s CallLater while IsActive && mask me && map open (hook SCR_MapEntity.GetOnMapOpen/GetOnMapClose, register once); PLACED_CUSTOM marker, pos + yaw
	protected static void RemoveAll();
}
// CLW_FriendlySymbols.c
class CLW_FriendlySymbols
{
	static bool Resolve(string kind, bool isSquad, out EMilitarySymbolDimension dimension, out int iconFlags);   // table 2.4, enum names via typename.StringToEnum
	static SCR_MapMarkerBase CreateMilitary(int identity, string kind, bool isSquad, string label, int worldX, int worldZ);  // manager.PrepareMilitaryMarker + SetCustomText + SetWorldPos; null when the config lacks the identity/dimension
	static SCR_MapMarkerBase CreateMe();               // PLACED_CUSTOM, icon + colour entry found by name (2.2)
}
// CLW_FriendlyMapManager.c
modded class SCR_MapMarkerManagerComponent
{
	void CLW_AddLocalMarker(notnull SCR_MapMarkerBase marker);       // 2.2 helper: owner -1, m_aStaticMarkers.Insert, OnCreateMarker(true) only if map open + SCR_MapMarkersUI present, then marker.GetMarkerComponent().SetEventListening(false)
	void CLW_RemoveLocalMarker(notnull SCR_MapMarkerBase marker);    // OnDelete + RemoveItem from both lists
}
// CLW_FriendlyMapSuppress.c
modded class SCR_MapMarkerSquadLeader { override void UpdateLocalVisibility(); }  // if (!CLW_FriendlyMapClient.KeepVanillaLeader(GetGroup())) { SetLocalVisible(false); m_bDoLocalVisibilityUpdate = false; return; } super.UpdateLocalVisibility();
modded class SCR_MapMarkerSquadMember { override void UpdateLocalVisibility(); }  // if (!CLW_FriendlyMapClient.KeepVanillaMember()) { SetLocalVisible(false); m_bDoLocalVisibilityUpdate = false; return; } super...
```
Client rules: world position = x5*5 + 2, z5*5 + 2. Local faction key differs from `m_sFaction` => `RemoveAll()` until the next picture. The receiver's own entries never arrive (server filters). No new layouts, widgets or imagesets.
`SCR_MapMarkerManagerComponent` is already modded in `CLW_MapMarkerClient.c`; a second `modded class` in another file is fine, do not edit that file; unique `CLW_` member names.

### Interface freeze
- `CLW_FriendlyMapSettings.Mask()` int bits 1/2/4/8 (A -> B).
- `CLW_FriendlyPicture.Start()`, `.RematchReset()`, `.Resend(int)` (B; A calls the first two).
- `CLW_FriendlyPic.FromWire(string)` + `CLW_FriendlyEntry` fields exactly as above (B owns, C reads).
- `CLW_FriendlyMapClient.Receive(string)`, `.KeepVanillaLeader(SCR_AIGroup)`, `.KeepVanillaMember()` (C owns; B and the suppress hooks call).
- Kind strings = `CLW_ForceKind` values + `air_rotary`, `air_fixed`.

## 5. Risks and fallbacks
| risk | likelihood | fallback |
|---|---|---|
| Local `PLACED_MILITARY` marker misbehaves (hover, click edit dialog, flicker, z-order below vanilla markers) | medium | owner id -1 + `SetEventListening(false)` already removes edit/hover. If still bad: squads only as SERVER static markers via the `CLW_ContactMarkers` pattern (cost: 2 broadcast RPCs per moved squad per 4 s to all clients; no members/me/vehicles). Second fallback: native `MapEntity.CreateCustomMapItem()` items (no NATO frame). |
| Vanilla has an engine self marker (`MDT_CURPOS`), so "me off" is not off | low | hide via layer props (2.1). Test step 2. |
| Reliable RPC string too long (> about 4 KB) in big wars | low | MAX_ENTRIES cap (squads first), then chunking `F1/p/n`. The send log shows bytes. |
| Moving units invisible until a pan (culled/disabled list) | low | `SetStaticMarkerDisabled(marker, false)` after each picture while open (specified). |
| Vanilla leader markers reappear after a faction change | low | `RefreshVanilla()` on map open and on local faction change. |
| Group-less players with `mapSquads` | none | by design the picture is faction-level, independent of vanilla groups. |
| UGC-restricted accounts get all static markers blocked | low, vanilla | same as contact markers; documented, not fixed. |
| `SCR_MapMarkerSquadLeader.UpdateLocalVisibility` changes in a game update | low | loud compile error; it is the only coupling. |
| GM editor map shows nothing from this feature | by design | n/a |
| 4 s tick cost with 128 AI | low | measured by the tick log; raise `TICK_MS`; factions without recipients are skipped (specified). |

## 6. Test plan (dedicated server `_dedicated/` + one human client; skill `reforger-workbench-test`)
Start with overrides.json `setup` values or `/war start mapMe=1 mapSquadmates=1 mapSquads=0 mapVehicles=0`, large 128-AI war, the human takes an AI squad (`CLW_PlayerSquad.Join`).
1. Compile: DS log without script errors; `CLW: friendly` lines on start; setup menu shows four new rows with EN and DE text; `/war start` and overrides.json set them.
2. All four off, open M: no own arrow, no AI/vehicle symbols besides contact/structure markers; vanilla leader markers of other groups AND of the own group are gone (confirms 2.1 and the suppression hook).
3. `mapMe` only: arrow at the player, rotates with him, follows while the map stays open, gone on death/spawn screen; the server sent only header pictures (log `bytes`).
4. `mapSquadmates`: all own-group AI + other humans as infantry symbols with player-name labels; a killed member disappears within one tick; a member entering a vehicle moves with it; the receiver is not duplicated; a human in another squad sees HIS members.
5. `mapSquads`: one symbol per HQ squad at its leader, correct NATO kind (infantry/recon/mech/armor), human-led shows the leader name; own squad as a squad symbol only when `mapSquadmates` is off, as members when on; a second client of the enemy faction receives none of it (its log shows only its own faction).
6. `mapVehicles`: unit vehicles with crew (APC, truck), a depot-requested vehicle driven by the human, a helicopter (AIR); dismounted-but-claimed vehicle stays; destroyed vehicle disappears.
7. All four on: zoom through layers (text hides above layer 1), units move into view with the map open without a pan, map close/open keeps markers, JIP reconnect gets the picture within 4 s, faction switch / rematch clears markers and vanilla returns (`active=0`).
8. Performance: tick ms and bytes in the log with 128 AI and 4 clients (budget: tick under 5 ms, under 3 KB per picture); no visible client hitch with the map open.
9. GM takeover / GM map unaffected; no errors on the console-less DS (client classes guard `System.IsConsoleApp()`).
10. Non-CLW scenario with the mod loaded: vanilla map unchanged (leader markers as in vanilla) because no `active=1` picture arrives.
Result goes into `dev/PROGRESS.md`; gotchas into the `reforger-script-dev` skill.

## 7. Open questions left for the owner
- Default values (proposal in section 1).
- GM takeover / spectators: proposal = nothing from this feature (the editor map shows everything).
- Smooth movement between pictures (client lerp over 4 s) is a cheap later polish, not in the first cut.
