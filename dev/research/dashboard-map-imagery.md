# Dashboard map imagery: research

Goal: real map image behind squads/contacts/orders in `dev/tools/dashboard/app.js` for any loaded Reforger map (Arland, Everon, Cain/Kunar-like, modded), no per-map hardcoding.
Labels: **[V]** verified by me this session (file listed, bytes decoded, URL fetched), **[U]** unverified/inferred.

## TL;DR

The base game already ships a ready-made 4096x4096 map background and a vector map file (.topo) for every vanilla terrain, referenced by each world's `MapEntity`. I extracted and decoded them: **it works and looks good** [V]. Recommended: the sidecar reads the game's own pak files on the user's machine, decodes the EDDS (BC7) and serves it to the dashboard, keyed by world name sent from the game. Nothing is redistributed (important: APL-SA is non-commercial). Roads/towns/bases are added as a vector overlay sent by the game (`RoadNetworkManager`, `MapEntity` descriptors / Conflict bases) later.

## Ranked options

| # | Option | Feasibility | Effort | Quality | Legal | Any map? | Verdict |
|---|--------|-------------|--------|---------|-------|----------|---------|
| 1 | **Sidecar extracts `UI/Textures/Map/worlds/**/*Rasterized.edds` (and `Everon.edds`) from installed game/addon paks, decodes BC7, serves as PNG/raw** | [V] decoded all four | S-M (about 150 lines node + WebGL or JS BC7 decoder) | Good: shaded relief + forest + bathymetry, 1-3 m/px; roads only on `Everon.edds` | Clean if not redistributed (user's own install) | Vanilla yes; mods yes if mod ships a MapEntity+edds (or exports one, see #2) | **Recommended** |
| 2 | One-off Workbench export of any world: *Export Map Data* tool, type Rasterization (-> TGA, flip) / Geometry2D (-> .topo); scriptable via `MapDataExporter.ExportRasterization(...)` | [V] documented on BIKI + API exists | M (manual per mod terrain, Workbench) | Same as #1, tunable colours/scale | Own content, fine | Yes for any terrain you have the .ent for | Fallback for modded terrains without a map texture |
| 3 | Game sends vector data (roads via `RoadNetworkManager.GetRoadsInAABB`+`BaseRoad.GetPoints`, locations/bases, coastline from heightmap sampling) and dashboard draws it | [V] APIs exist, [U] not run | M-L | Schematic, crisp, tiny payload | Clean | Yes, fully generic | Complement to #1 (overlay), also stand-alone fallback when no image |
| 4 | Third-party tiles `reforger.recoil.org/map-tiles/<map>/<lod>/<x>/<y>/tile.jpg` | [V] URLs return 200 | S | Good (Workbench screenshots), Everon/Arland/Kolguyev | Gray: no terms stated; derived from BI content (APL-SA); hotlinking someone's server | Only those 3 maps, no mods | Dev-time reference only, not a dependency |
| 5 | Run EnfusionMapMaker (nickludlam, APL-SA) to screenshot-tile any world in Workbench | [V] README | M-L (manual, 1440p, camera tweaks, Python) | High-res but screenshots with artifacts | APL-SA (non-commercial, share-alike) | Any world | Only if #1/#2 imagery is too abstract |
| 6 | In-game script capture of pixels / minimap to REST | Not realistic [U] | - | - | - | - | Reject |
| 7 | Generate from heightmap + own colouring (hillshade) | Feasible [U] | M | Plain | Clean | Needs heightmap export (Workbench) or script height sampling | Only as vector-ish fallback |

## (1) Base-game assets: what exists [V]

Listed with `game_browse` / `readpak`-style pak access (`E:/Games/steam/steamapps/common/Arma Reforger/addons/data/*.pak`):

| Path | Size | Notes |
|------|------|-------|
| `UI/Textures/Map/worlds/Arland/ArlandRasterized.edds` | 4.3 MB | 4096x4096 BC7_UNORM_SRGB, 13 mips. Used by `Prefabs/World/Game/Arland/MapEntity_Arland.et` ("Satellite background image") |
| `UI/Textures/Map/worlds/Arland/Arland.edds` | 11.2 MB | older/other background (not decoded: exceeds the 8 MB read cap of the MCP's PakVirtualFS) |
| `UI/Textures/Map/worlds/EveronRasterized.edds` | 7.9 MB | 4096x4096, decoded |
| `UI/Textures/Map/worlds/Everon.edds` | 2.8 MB | 2048x2048, decoded: dark satellite-ish look **with roads baked in** |
| `UI/Textures/Map/worlds/Cain/CainRasterized.edds` | 5.3 MB | 4096x4096 (Cain/Kunar-like), header read |
| `worlds/Arland/Arland.topo` | 1.4 MB | "Map Geometry Data": roads, buildings, airfields; magic `TOPO`. Eden.topo 9.9 MB, Cain.topo 7.2 MB. Binary, format undocumented [U] |
| `worlds/<W>/Terrain/.Data/Terrain_N_supertexture.edds` + `Terrain_N.ttile` (+ `_layer`, `_normal`) | ~85 KB each, hundreds of tiles | Terrain "super texture" tiles for close-range rendering; would need stitching + tile layout from `Terrain.terr`. Not worth it. |
| `worlds/<W>/<W>.smap/.smd` | 1.6-16 MB | shore map / distance field data |

- MapEntity prefab fields (verified in `MapEntity_Arland.et`, `MapEntity_Cain.et`): `"Map Geometry Data"` -> `.topo`, `"Satellite background image"` -> `.edds`.
- BIKI "2D Map Creation" (via `wiki_read`) confirms the tool chain: World Editor *Export Map Data* (Geometry2D -> .topo, Rasterization -> 4096x4096 upside-down TGA -> flip -> PNG -> register, Color Space ToSRGB -> .edds). `MapDataExporter` (`ExportData`, `SetupColors`, `ExportRasterization(path, world, scaleLand, scaleOcean, heightScale, depthScale, ...)`) is in the script API, so the export can be scripted in a WorldEditorTool [V API exists, U not run]. BIKI warns Geometry2D export is slow/RAM-hungry and "do not try on large maps like Everon".
- Image content [V, rendered]: shaded relief (grey), forest (green), sea gradient (blue), airfield visible on Arland. **No roads or labels** in `*Rasterized` (roads are vector in .topo); `Everon.edds` has roads.
- Georeferencing [U, standard assumption]: image covers the map extent `MapEntity.Offset()` .. `Offset()+Size()` (Everon 12800 m -> 3.125 m/px, Arland 4096 m? -> ~1 m/px). Check orientation/flip with a known coordinate (my render has north up for Everon; Arland airfield west/north-west) before trusting.

### EDDS format (decoded by me, working code was 40 lines of JS) [V]
- `DDS ` header (128 B) + DX10 header (20 B; dxgiFormat 99 = BC7_UNORM_SRGB; 98 = BC7_UNORM expected on others) + mip table of N x {fourcc `COPY` or `LZ4 `, u32 size} (smallest mip first) + mip bodies in the same order.
- `LZ4 ` body: u32 LE total uncompressed size, then chunks `[u32: (flag<<24)|compressedSize][LZ4 block]`, each chunk decodes to 64 KB. **Gotcha:** chunks are *linked*: matches may reference the previous chunk's output, so decode into one contiguous buffer (decoding each chunk alone gives scattered black/garbage 4x4 blocks; I hit exactly this). Last chunk has flag 0x80.
- Pixel decode: ffmpeg's dds decoder and Windows WIC both fail on BC7 here. What worked: WebGL `EXT_texture_compression_bptc` in the browser (upload the raw BC7 data with `compressedTexImage2D`, draw, `toBlob`). Alternatives: `wrdg/edds2png` (C#), `WoozyMasta/edds` (Go; reads BC7, LZ4) as references; I could not `go get`/npm in this sandbox.
- Pak reading: `D:\GitHub\enfusion-workbench-mcp\dist\pak\vfs.js` `PakVirtualFS.readFile()` works (used by `_tools/readpak.mjs`) but refuses entries > 8 MiB (blocks `Arland.edds`, `Eden.topo`, `.ent`). A sidecar needs its own small pak reader or the cap raised.

Local test renders are in the session scratchpad only (not committed): Everon/Arland/Cain Rasterized + `Everon.edds`.

## (2) In-game script: image vs vector [partly V]

**Pixel/minimap capture: reject.** I found no Enforce API to read texture pixels or render a widget to a buffer (api_search for pixel/texture only returns widget `SetTexture`) [U: not exhaustive]. Even if it existed, a 4096 px RGBA is 64 MB; BC7 16 MB; RestContext limits are unverified [U] and base64 inflates 33%. Not worth it, and the image is already on disk (option 1).

**Vector data the game CAN send (generic, any map):**
- World identity/size: `GetGame().GetWorldFile()` returns the world resource path (e.g. `worlds/Arland/Arland.ent`) [V API]; `GetGame().GetWorldEntity().GetWorldBounds(mins, maxs)` is used by vanilla `SCR_MapLocator` [V]; `MapEntity.Offset()` / `Size()` (offset, x/z size) and `SCR_MapEntity.GetMapSizeX/Y()` are the exact values vanilla uses for map<->world mapping [V, SCR_MapEntity.c]. Note `SCR_MapEntity` sizes only exist once the map entity is initialised; fall back to world bounds.
- Roads: `RoadNetworkManager.GetRoadsInAABB(min,max,out array<BaseRoad>)` + `BaseRoad.GetPoints(out array<vector>)` + `GetWidth()` [V API; obtained via `SCR_AIWorld/ChimeraAIWorld.GetRoadNetworkManager()`]. Query in tiles (e.g. 1 km AABBs), dedupe, Douglas-Peucker simplify. Estimated size: Everon a few thousand polylines, ~100-300 KB JSON after simplification [U estimate]. Send once per world in chunks of ~16-32 KB (RestContext body limit unknown [U]; keep chunks small and POST several).
- Locations/bases/towns: `MapEntity.GetByType(out items, EMapDescriptorType.MDT_NAME_VILLAGE/CITY/TOWN/..., MDT_BASE, MDT_AIRPORT, MDT_PORT)` then `MapItem.GetPos()/GetDisplayName()` [V API; U whether populated without the map UI opened: call `AbsorbData()` first]. `MDT_*` also covers MDT_ROAD, MDT_BUILDING, MDT_HILL etc. For Conflict: `SCR_MilitaryBaseSystem` / `SCR_CampaignMilitaryBaseComponent` give bases with names [V classes exist].
- Terrain/coast: `BaseWorld.GetSurfaceY` sampling (grid 64 m on 12.8 km = 40k samples, too many for REST; coarser 256 m = 2.5k points is fine) -> land/sea contour in the dashboard. [U not run]
- Topo (`.topo`) is read by the engine only; no script access to its geometry, and the file format is undocumented [U]. Do not plan on parsing it.

## (3) Web/community [V unless noted]

| Source | What | Tile scheme | Coverage | Terms / notes |
|--------|------|-------------|----------|---------------|
| reforger.recoil.org (Bewilderbeest) | Leaflet maps with markers | `https://reforger.recoil.org/map-tiles/<everon\|arland>/<lod>/<x>/<y>/tile.jpg`. 542x542 px JPEGs; "lod" counts coarse->fine downwards: Everon lod5 = 4x4 tiles, lod4 8x8, lod3 16x16, lod2 32x32; lod0/1 -> 404 for Everon; Arland lod1 exists. Not standard TMS/XYZ, y direction unknown [U] | Everon, Arland, Kolguyev; no mods | No terms page; robots.txt allows all; headers `cache-control public max-age=604800 immutable` (200 on sample tiles). Hotlinking not addressed: ask or self-host. Source of tiles = Workbench screenshots by EnfusionMapMaker |
| github.com/nickludlam/EnfusionMapMaker | WB tools: Auto Screenshot Maker (FOV 15, far plane >4500, F11) + `ResourceQueryWorldEditorTool.c` (entity JSON export) + Python `crop_screenshots.py`, `create_zoom_levels.py` (Pillow), optional ImageMagick | produces `Web/<map>/LODS/` | any world you can open | **APL-SA** (non-commercial, share-alike, Arma-only) |
| xgamingserver.com/tools/arma-reforger/map | Leaflet map of Everon+Arland towns/bases | tiles not inspected | 2 maps | uses EnfusionMapMaker data |
| ArmaCartography (Workshop 66AB26E51093F530, by Kaffeina-, APL) | replaces vanilla 2D map textures for Everon/Arland (MapEntity overrides) | n/a (in-game textures) | 2 maps | APL; shows that mods CAN override the map edds |
| Enhanced Maps (Workshop 644B042109700804) | similar in-game map replacement | n/a | [U] | not inspected |
| jetelain GameMapStorage / Arma3Map (maps.plan-ops.fr) | Leaflet tile infra for Arma 3 | Reforger not live; issue #94: "planned", export process missing, in April 2026 planning to import EnfusionMapMaker tiles | Arma 3 only for now | maps.plan-ops.fr is Arma 3 only [V page], no license text |
| ReforgerSatGenerator (granQ) | Python: world mask export + ground textures -> satellite.png | n/a | any world | no license stated |

APL-SA text [V, bohemia.net]: NonCommercial, ShareAlike, attribution required, material usable only for Arma-series games (a companion web tool is a gray area). Conclusion: do not bundle or redistribute extracted/derived BI imagery in the repo or Workshop addon; reading it from the user's own install at runtime avoids the question.

## (4) Other ideas
- **Workbench screenshots**: EnfusionMapMaker above (also `wb_*` capture is not needed if option 1/2 suffice).
- **Mod maps**: if a mod terrain has a `MapEntity` with a "Satellite background image", it lives in the mod's pak (or loose files in `addons/<mod>`); option 1's pak scan covers it. Otherwise run *Export Map Data -> Rasterization* once in Workbench (4096 px TGA, then flip), or ship the PNG next to the sidecar config as `maps/<WorldName>.png` + `maps/<WorldName>.json` (origin/size). This keeps "any map" without code changes.
- **Terrain_N_supertexture tiles**: real close-range ground imagery but needs `.terr` layout parsing and stitching hundreds of tiles; poor value.
- **Everon.edds** (2048, roads baked in) is probably the best single "topographic" look for Everon; Rasterized for the others.

## Recommended approach

Sidecar-side map resolver, keyed by world path, with graceful fallback to today's grid.

1. **Game side (tiny)**: extend the situation POST (or add a one-time `POST /api/world`) with `{worldFile: GetGame().GetWorldFile(), boundsMin, boundsMax, mapOffset, mapSize}`. `GetWorldFile` returns `{GUID}worlds/Arland/Arland.ent`; strip to the path. Use `MapEntity` Offset/Size when available, else world bounds. [API V, not run]
2. **Sidecar `/api/map` endpoint**: given world name (`Arland`), look for, in order: (a) user override `dev/tools/dashboard/maps/<World>.png|.json`; (b) in game `addons/data` + other `addons/*/data.pak` + Workshop folders a file matching `UI/Textures/Map/worlds/**/<World>*Rasterized.edds` or `<World>.edds` (Eden = Everon: map `Eden` -> `Everon*`; the internal world name differs from the display name [V: Eden.ent is Everon]); (c) none -> grid.
   Better resolution key: parse the world's `MapEntity` prefab ("Satellite background image" GUID) — needs GUID->path lookup (see `_tools/guid-lookup.ps1`), more work, do later.
3. **Pak access**: reuse `enfusion-workbench-mcp` `PakVirtualFS` (raise the 8 MiB cap or add a streaming read) or write a minimal reader. Cache the decoded result in `%TEMP%` or `dev/tools/dashboard/.cache/` (gitignored).
4. **Decode**: implement EDDS parser per the format above (contiguous LZ4 buffer!). Two options: (a) serve the raw BC7 blob + width/height and let `app.js` upload it with WebGL `EXT_texture_compression_bptc` then draw to an offscreen 2D canvas once (zero server-side image code; works in Firefox [V], Chrome desktop expected [U]); (b) write a ~200-line BC7 decoder in the sidecar and emit PNG (no browser dependency, needs partition tables). Prefer (b) for robustness if Chrome lacks BPTC; (a) for speed of first delivery.
5. **Dashboard**: replace `WORLD = 4096` constant with world size from the POST; draw the image under the grid at `[offset, offset+size]` (flip y as the canvas already does, `sy(z) = oy + (WORLD - z)*s`); opacity/dim slider; keep grid as fallback. Calibrate orientation once with a known landmark (Arland airfield, Everon lake) and a squad position.
6. **Vector overlay (phase 2)**: game posts roads (`RoadNetworkManager`), named locations/bases (`MapEntity.GetByType`, `SCR_MilitaryBaseSystem`), once per world in <=32 KB chunks; dashboard draws them on top. This also works when no imagery is found (modded terrains).
7. **Mods**: document `maps/<World>.png` + `.json` override and the Workbench *Export Map Data -> Rasterization* recipe in the dashboard README.

## Risks / unknowns
- Image extent may not equal `Offset..Offset+Size` exactly for every terrain [U]; add a manual calibration (offset/scale in the override .json).
- `Arland.edds` (11 MB) and other >8 MiB paks entries blocked by MCP's VFS cap; Rasterized files are all under the cap (Everon 7.9 MB is close).
- Game updates may rename/relocate textures; the resolver must degrade to the grid, never crash.
- Workshop mods store textures in their own pak (possibly with different compression/format e.g. BC1/DXT5): the parser needs more formats than BC7 for generality [U].
- RestContext POST body limit and timeouts unknown [U]; keep vector payloads chunked.
- `MapEntity.GetByType` may be empty until the player opens the map / `AbsorbData()` runs [U].
- Legal: do not commit extracted images; APL-SA is non-commercial/Arma-only. Third-party tiles: no explicit terms, do not hotlink in a distributed tool.
- Collateral note: while testing, I killed all `node.exe` processes once, which briefly disconnected the enfusion-mcp and camoufox servers (they reconnected); no repo files were touched.

## Sources
- BIKI 2D Map Creation: https://community.bistudio.com/wiki/Arma_Reforger:2D_Map_Creation [V, via wiki_read]
- BIKI Terrain Entity (super texture, tiles): https://community.bistudio.com/wiki/Arma_Reforger:Terrain:_Terrain_Entity [V]
- Script API: `MapDataExporter`, `MapEntity`, `MapItem`, `EMapDescriptorType`, `RoadNetworkManager`, `BaseRoad`, `Game.GetWorldFile`, `BaseWorld.GetBoundBox` [V via api_search/game_read]
- Base game files: `Prefabs/World/Game/Arland/MapEntity_Arland.et`, `Prefabs/World/Game/Cain/MapEntity_Cain.et`, `scripts/Game/Map/SCR_MapEntity.c`, `scripts/Game/Entities/SCR_MapLocator.c`
- EnfusionMapMaker: https://github.com/nickludlam/EnfusionMapMaker (APL-SA) [V]
- reforger.recoil.org tiles, e.g. https://reforger.recoil.org/map-tiles/everon/5/2/2/tile.jpg [V 200]
- xgamingserver map: https://xgamingserver.com/tools/arma-reforger/map [V]
- ArmaCartography: https://reforger.armaplatform.com/workshop/66AB26E51093F530-ArmaCartography [V]
- jetelain/Arma3Map issue 94 (Reforger): https://github.com/jetelain/Arma3Map/issues/94 ; maps.plan-ops.fr [V Arma 3 only]
- EDDS references: https://github.com/WoozyMasta/edds , https://github.com/wrdg/edds2png [V pages, code not run]
- ReforgerSatGenerator: https://github.com/granQ/ReforgerSatGenerator [V]
- APL-SA: https://www.bohemia.net/community/licenses/arma-public-license-share-alike [V]
