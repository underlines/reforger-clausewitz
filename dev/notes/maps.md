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
3. AI: the wrapper writes **no AIWorld prefab** (see the risk paragraph). The layer's `SCR_AIWorld CLW_AIWorld` entity references
   - the map's own AIWorld prefab when it references `.nmn` files (maps usually do, e.g. `{3DE6090FC602E566}Prefabs/AI/SCR_AIWorld_Takistan.et`), or
   - otherwise (navmeshes found but no such prefab, or `--force-nav`) the vanilla `SCR_AIWorld.et` with the soldier + vehicle navmesh components written
     inline as entity component overrides (`components { ... }` in the layer; name heuristic: `veh|btr|car|wheel|tank|apc|truck` = vehicle,
     `lowres` ignored, rest = soldier; override with `--soldier-nav` / `--vehicle-nav`), or
   - (`--no-nav`, or no navmesh at all) plain vanilla `SCR_AIWorld.et` and a warning: **AI will not move** (generate navmeshes in Workbench, or use a
     map that ships them).
4. Spawn points: reads the map's location layers (`Prefabs/World/Locations/...Urban...` with `MainType Town/Village/City`) and puts the two spawn
   points in the two most distant settlements of the central 40 (+40 m east, not in the sea). No location data: 2048 0 2048 with a warning,
   or pass `--spawn "x,y,z;x,y,z"` (USSR;US).
5. Writes `<out>/<Name>/addon.gproj` (id `Clausewitz_<World>`, fresh random GUID, dependencies = Clausewitz `C1A05E7B3D9F4A28` + the map addon),
   `Worlds/CLW_<World>.ent` (+ `.meta`) and `Worlds/CLW_<World>_Layers/default.layer`: four files, fresh GUIDs (checked against the map's rdb and the
   Clausewitz addon).
6. **Reference check** (printed every run, exit code 3 on offenders): every `{GUID}path` in the generated files must be registered outside the wrapper:
   in the map (`.meta` / rdb), in the Clausewitz addon (`.meta` / rdb) or in the base game (path exists in the game paks, `--game <dir>`, default
   `_dedicated/server`, and the GUID is seen in a file that loads). Output: `references: 9, all registered (map 2, base game 4, Clausewitz 3; ...)`
   or the offenders.
7. Prints a ready-to-paste PowerShell block for the local DS: junctions for the map, its dependency addons (found as `<Name>_<GUID>` folders next to the
   map, transitive; base game `58D0FB3206B6F859` is not a pack) and the wrapper, the `packs-<world>.txt` content (dependencies in order, map, wrapper;
   the start script adds Clausewitz) and the `start-server.ps1 -World ... -Packs ...` line. `--ds <dir>` overrides the DS root
   (default `D:\GitHub\ReforgerMods\_dedicated`).

Verified dry runs (2026-10-12, temp folder): Takistan (packed, own AIWorld + navmeshes: the layer entity references the map prefab, 9 references all
registered), Kadykchan (packed, no navmesh anywhere: vanilla AIWorld, warns), Takistan `--force-nav` (vanilla AIWorld + both navmesh components inline,
11 references all registered). Dedicated-server finding behind the current design: a wrapper with its own AIWorld prefab failed with
`Wrong GUID/name for resource "{...}Prefabs/AI/CLW_AIWorld_Takistan.et" in property "inherited-name"` (no rdb) and AI did not move.

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
$map = "C:\Users\Jan\Documents\My Games\ArmaReforger\addons\Takistan_615EEBD9BDFEEE9B"
node D:\GitHub\ReforgerMods\Clausewitz\dev\tools\new-map-wrapper.mjs $map --out D:\GitHub\ReforgerMods\_dedicated\wrappers
# the run ends with "references: N, all registered" and a PowerShell block (junctions for map + dependencies + wrapper, packs-<world>.txt,
# start-server line): paste it. Add faction packs you want to the packs file by hand.
```
The shipped wrappers need no packs: `start-server.ps1 -World Worlds/CLW_Eden.ent` (Everon) and `-World Worlds/CLW_Cain.ent` (Kolguyev).

What to look for in `profile\logs\...\console.log` / `script.log`:
- no `Failed to open ... Worlds/CLW_<Name>.ent`, no `Wrong GUID/name for resource` / `Unknown GUID` (see risk below);
- `Compiling Game scripts` then `CLW: scenario <name> (source ...)` (WP1) and `war RUNNING` after the auto start;
- AI: `CLW:` lines about group orders, no `Navmesh ... not found`; a map without navmesh shows AI standing still.


Risk: `resourceDatabase.rdb` (hit on the DS with Takistan). A generated wrapper addon has no rdb, so the server cannot resolve its resources by GUID.
The rule is therefore **no wrapper-internal GUID references**: the generated files reference only resources registered in the map addon, the Clausewitz
addon or the base game, and the world is passed by path (`-server Worlds/CLW_<World>.ent`; its layer is found by path). The generator's reference
check enforces this on every run. Do not reintroduce a prefab in the wrapper. (The shipped Arland / Eden / Cain wrappers live in the Clausewitz addon,
whose tracked `resourceDatabase.rdb` Workbench maintains.)
