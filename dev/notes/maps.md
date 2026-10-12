# Maps: wrapper worlds and the wrapper generator (M4 WP2)

Clausewitz needs a world that holds its management entities (AIWorld, PerceptionManager, FactionManager, `CLW_GameMode`, EditorSettings,
two SpawnPoints, LoadoutManager, Chat). Each map gets a small **wrapper world**: a `SubScene` whose parent is the bare map world plus one
layer with those nine entities. The game mode entity sets NO scenario config: `CLW_GameMode` picks a scenario by world name
(`Configs/CLW/Scenarios/<world file name>.conf`), a scenario that lists the world, or the auto scenario (WP1).

## Shipped wrappers (in the Clausewitz addon)
| World file | Parent (base game) | AIWorld prefab | Navmeshes |
|---|---|---|---|
| `Worlds/CLW_Arland.ent` | `{A9806AF617972E97}worlds/Arland/Arland.ent` | `Prefabs/AI/CLW_AIWorld_Arland.et` | `CTI_Campaign_Arland(.nmn / _vehicles.nmn)` |
| `Worlds/CLW_Eden.ent` (Everon) | `{853E92315D1D9EFE}worlds/Eden/Eden.ent` | `Prefabs/AI/CLW_AIWorld_Eden.et` (parent `SCR_AIWorld_Eden.et`) | `{A07353FE17EB2C49}` `CTI_Campaign_Eden.nmn`, `{3D08F0F26F771347}` `..._Eden_vehicles.nmn` |
| `Worlds/CLW_Cain.ent` (Kolguyev) | `{1EA95DAE3230BEB0}worlds/Cain/Cain.ent` | `Prefabs/AI/CLW_AIWorld_Cain.et` (parent `SCR_AIWorld_Cain.et`) | `{B51562AFF15F6CCB}` `CTI_Campaign_Cain.nmn`, `{7D656156ECDE1C1C}` `..._Cain_vehicles.nmn` |

Resource GUIDs (fresh, random): `CLW_Eden.ent.meta` `A00206B333AA89A1`, `CLW_Cain.ent.meta` `7B50B30E5805ED32`,
`CLW_AIWorld_Eden.et.meta` `4C5F3BD39A62569C`, `CLW_AIWorld_Cain.et.meta` `3F13B448A29F3581`. (`CLW_Arland.ent.meta` is parent GUID + 1, which looks
like a Workbench artefact; there is no rule behind it.)

Spawn point positions in the Eden / Cain layers are the map centre area (Eden 7000/5800 x 6400, Cain 4600/3600 x 4096, y 0) because no map data
for the vanilla worlds is readable offline (the vanilla `.ent` is binary). The game re-places base spawn points at runtime. If the server log
or the editor shows a spawn in the sea, move the two `SCR_SpawnPoint` entities in `Worlds/CLW_<Name>_Layers/default.layer`.

## Generator: `dev/tools/new-map-wrapper.mjs`
Creates a tiny compat addon for any map mod (packed Workshop addon or loose). Node >= 20, no npm dependencies. The pak reader comes from the
Enfusion MCP build (`D:/GitHub/enfusion-workbench-mcp/dist`, env `ENFUSION_MCP_DIST` overrides). It only reads the map addon.

```powershell
cd D:\GitHub\ReforgerMods\Clausewitz\dev\tools
$Map = "C:\Users\Jan\Documents\My Games\ArmaReforger\addons\Takistan_615EEBD9BDFEEE9B"
node new-map-wrapper.mjs $Map --list                 # worlds (scored), navmeshes, AIWorld prefabs, places found
node new-map-wrapper.mjs $Map --out D:\GitHub\ReforgerMods\_dedicated\wrappers
```
What it does:
1. Reads `addon.gproj` (id, GUID, dependencies). Reads the file list from `data.pak` (or the folder) and resource GUIDs from loose `.meta` files or
   `resourceDatabase.rdb` (record: u32 length+1, path, 0, 6 bytes, GUID little-endian; verified against the Takistan GM scene).
2. World: picks the `.ent` whose `<name>_Layers/default.layer` starts with `GenericWorldEntity` and whose path is not a game master / empty / test scene.
   Ambiguous (tie): prints the candidates and exits with code 2; give `--world <virtual path>` (or `--yes` to take the first).
3. AI: if the map ships an `AIWorld` prefab that references `.nmn` files (maps usually do, e.g. `SCR_AIWorld_Takistan.et`), the wrapper's AIWorld
   prefab simply inherits it (no override). Otherwise it inherits the vanilla `SCR_AIWorld.et` and writes the two navmesh components (soldier +
   vehicle) from the `.nmn` files it found (name heuristic: `veh|btr|car|wheel|tank|apc|truck` = vehicle, `lowres` ignored, rest = soldier;
   override with `--soldier-nav` / `--vehicle-nav`, force with `--force-nav`). **No navmesh at all: it still writes the world and warns, AI will not
   move** (generate navmeshes in Workbench, or use a map that ships them). `--no-nav` skips the navmesh step.
4. Spawn points: reads the map's location layers (`Prefabs/World/Locations/...Urban...` with `MainType Town/Village/City`) and puts the two spawn
   points in the two most distant settlements of the central 40 (+40 m east, not in the sea). No location data: 2048 0 2048 with a warning,
   or pass `--spawn "x,y,z;x,y,z"` (USSR;US).
5. Writes `<out>/<Name>/addon.gproj` (id `Clausewitz_<World>`, fresh random GUID, dependencies = Clausewitz `C1A05E7B3D9F4A28` + the map addon),
   `Worlds/CLW_<World>.ent` (+ `.meta`), `Worlds/CLW_<World>_Layers/default.layer`, `Prefabs/AI/CLW_AIWorld_<World>.et` (+ `.meta`), all with fresh GUIDs
   (checked against the map's rdb and the Clausewitz addon). It prints the `-addons` list and the `-World` value for the server.

Verified dry runs (2026-10-12, output in a temp folder): Takistan (packed, own AIWorld + navmeshes: inherits, spawns at two real towns),
Kadykchan (packed, no navmesh anywhere: warns, no place data: warns), Takistan `--force-nav` (writes both navmesh components with correct GUIDs),
Takistan GM scene via `--world`.

Limits:
- The map's world is assumed to be a bare terrain world (like `Arland.ent`). A world that already contains a game mode / AIWorld / faction manager
  would get duplicates; the heuristic prefers the terrain scene over the GM scene for that reason.
- Dependencies of the map (terrain core packs, prefab packs) are NOT in the wrapper's gproj (the map's own gproj lists them); the server needs
  all of them in `-addons` (the generator prints the list, in order).
- The generated spawn points are placeholders for the engine; WP1 places the real HQs / bases at runtime.
- A faction pack works the same as on the vanilla maps (the wrapper is faction agnostic).

## Run a wrapper on the local dedicated server
Workspace tooling: `D:\GitHub\ReforgerMods\_dedicated\` (`README.md`, `start-server.ps1 -World <world> -Packs <file>`). The server reads addons as loose
folders or packed Workshop folders from `_dedicated\addons\` (junctions); `-addons` is `Clausewitz GUID` + the comma separated list of the packs file.
```powershell
$ds  = "D:\GitHub\ReforgerMods\_dedicated"
$map = "C:\Users\Jan\Documents\My Games\ArmaReforger\addons\Takistan_615EEBD9BDFEEE9B"
node D:\GitHub\ReforgerMods\Clausewitz\dev\tools\new-map-wrapper.mjs $map --out "$ds\wrappers"      # prints the GUIDs below
# junctions (once): the map addon and its dependencies (terrain core packs: see "map needs" in the output) and the wrapper
New-Item -ItemType Junction -Path "$ds\addons\Takistan_615EEBD9BDFEEE9B" -Target $map
New-Item -ItemType Junction -Path "$ds\addons\Clausewitz_Takistan"       -Target "$ds\wrappers\Clausewitz_Takistan"
# dependency junctions, same way: ...\addons\ArmaTerrainCore_60B524F7A5AE96A8, ...\OutsideTerrainCore_61F0D506562D9BC9
# packs file: the generator's "-addons list" without the leading Clausewitz GUID (+ faction packs you want)
Set-Content "$ds\packs-takistan.txt" "60B524F7A5AE96A8,61F0D506562D9BC9,615EEBD9BDFEEE9B,<WRAPPER GUID>"
& "$ds\start-server.ps1" -World "Worlds/CLW_Takistan.ent" -Packs packs-takistan.txt
```
The shipped wrappers need no packs: `start-server.ps1 -World Worlds/CLW_Eden.ent` (Everon) and `-World Worlds/CLW_Cain.ent` (Kolguyev).

What to look for in `profile\logs\...\console.log` / `script.log`:
- no `Failed to open ... Worlds/CLW_<Name>.ent`, no `Resource ... not found` / `Unknown GUID` for the wrapper resources (see risk below);
- `Compiling Game scripts` then `CLW: scenario <name> (source ...)` (WP1) and `war RUNNING` after the auto start;
- AI: `CLW:` lines about group orders, no `Navmesh ... not found`; a map without navmesh shows AI standing still.

Risk: `resourceDatabase.rdb`. The Clausewitz addon carries a tracked `resourceDatabase.rdb` that Workbench updates; the new Eden / Cain wrapper
resources and a generated wrapper addon have no rdb entries until Workbench (or the server, if it rebuilds it for loose addons; not verified)
registers them. If the server reports unknown resources, open the addon once in Workbench (File > open the wrapper `addon.gproj`, or load Clausewitz
and let it index) and commit the updated `resourceDatabase.rdb`. Resource references inside the files use explicit `{GUID}path` forms, so only
the registration (path -> GUID) is at stake.
