# RHS prop style sets (M4 WP: pack prop sets)

Sources (read only): `RHS-StatusQuo_595F2BF2F44836FB`, `RHS-ContentPack01_1337C0DE5DABBEEF`, `RHS-ContentPack02_BADC0DEDABBEDA5E`
(Workshop installs under `Documents\My Games\ArmaReforger\addons`). Roles in use by the scripts: service.command_post, service.supply,
service.arsenal, service.vehicle_depot, service.medical, fob.arsenal_box, fob.supply_box, fort.sandbag_long (nothing else is referenced in `Scripts/`).

## Addons
- `RHS_Core` (StatusQuo, GUID 595F2BF2F44836FB) is the core: it defines the factions, all prefabs/configs below, and depends on
  `RHS_Content_01` (1337C0DE5DABBEEF) and `RHS_Content_02` (BADC0DEDABBEDA5E) (and the base game 58D0FB3206B6F859).
- ContentPack01/02 contain NO `.et`/`.conf` files (only models, materials, textures, sounds). So every prefab below is in StatusQuo, which
  always pulls both packs in: a server running an RHS faction always has it.

## RHS faction keys (Configs/Factions in StatusQuo)
| Key | Config | Friendly base faction | Style |
|---|---|---|---|
| `RHS_USAF` | RHS_US_USMC.conf (+ _Campaign) | US | US-like -> set `rhs_usaf` |
| `RHS_AFRF` | RHS_RF_MSV.conf (+ _Campaign) | USSR | RU-like -> set `rhs_afrf` |
| `RHS_ION` | RHS_ION.conf | (US identity/callsigns, own USSR-textured box) | own box -> set `rhs_ion` |
| `RHS_CIV` | RHS_CIV.conf (inherits vanilla CIV, no own FactionKey) | civilian | not a playable side, no set |

## Chosen entries (PropSets.conf)
| Set | Role | `{GUID}path` | Addon | Why it fits / what was checked |
|---|---|---|---|---|
| rhs_usaf | service.arsenal, fob.arsenal_box | `{3970981B94D3F882}Prefabs/Systems/Arsenal/ArsenalBoxes/US/ArsenalBox_USMC.et` | StatusQuo | Child of vanilla `ArsenalBox_US` (same look incl. clutter props + an RHS statue prop). Has `SCR_ArsenalComponent`, `SCR_FactionAffiliationComponent "RHS_USAF"` (vanilla box says `US` and would show US items only), `SCR_ResourceComponent`. Faction-locked to the RHS faction, which is what we want. GUID from StatusQuo `resourceDatabase.rdb`. |
| rhs_afrf | service.arsenal, fob.arsenal_box | `{FB95D1B918E908A9}Prefabs/Systems/Arsenal/ArsenalBoxes/USSR/ArsenalBox_RHS.et` | StatusQuo | Child of vanilla `ArsenalBox_Base` (SCR_ArsenalComponent, service point ARMORY, resupply station inherited), USSR-textured stack + 5.45/7.62 ammo clutter, affiliation `RHS_AFRF`. |
| rhs_ion | service.arsenal, fob.arsenal_box | `{691477161637ECDB}Prefabs/Systems/Arsenal/ArsenalBoxes/ION/ArsenalBox_RHS_ION.et` | StatusQuo | Same structure as the AFRF box, affiliation `RHS_ION`. |

Notes on the arsenal boxes: the three RHS boxes override `SCR_ResourceComponent` consumer `m_fBuyMultiplier 0` and the generator multiplier 0
(arsenal items cost no supplies; the vanilla `Consumer_Arsenal.conf` is the inherited default for the vanilla box). `m_eSupportedArsenalItemModes 246`
(vanilla base 254). The items an arsenal offers come from the faction affiliation, so the vanilla box (affiliation `US`/`USSR`) would be wrong/empty
for an RHS player; that is the reason these entries are worth having. Runtime item content was not tested (no e2e per task rules).

## Roles with NO RHS equivalent (left out, fall through to faction data / vanilla set)
- service.command_post / service.medical / service.supply / service.vehicle_depot / fort.sandbag_long / fob.supply_box:
  StatusQuo ships `PrefabsEditable/Auto/ConflictRHS/E_Headquarters_S_{USMC,AFRF}_01.et`, `E_FieldHospital_M_*`, `E_AmmoStorage_S_*`, `E_VehicleMaintenance_*`,
  `E_Sandbag*_{USMC,AFRF}_01.et` etc., BUT each is only a wrapper `GenericEntity : "...E_Headquarters_S_US_01.et"` that overrides
  `SCR_EditableEntityComponent` (m_sFaction `RHS_USAF`, label FACTION_RHS_USAF). Geometry is the identical vanilla US/USSR composition, they are
  Editor (`E_`) entities with `SCR_EditorLinkComponent` children, and not the `Prefabs/Compositions/Slotted/...Conflict_*` prefabs the vanilla sets use.
  Not RHS-looking, not functionally different -> not listed.
- service.supply / fob.supply_box: RHS has no supply stack/cache. Only `CamoNet_StorageVeh*` (vehicle camo-net parts with a 1-unit
  `SCR_ResourceContainer`, vehicle storage) and `CISS/SupplyDrop` equipment boxes (inventory crates). Not equivalent.
- service.vehicle_depot: vanilla `VehicleService_US.et` is a depot because of `SCR_CatalogEntitySpawnerComponent` (m_eType LIGHT_VEHICLE_DEPOT,
  delegate `ServicePointDelegate_VehicleDepotLight.et`, catalog type VEHICLE) plus `SCR_FactionAffiliationComponent "US"`. RHS has no vehicle-service
  prefab (no match for VehicleService / SCR_CatalogEntitySpawnerComponent depot in the pak). Note: the vanilla depot keeps affiliation `US`/`USSR`
  and will spawn that faction's catalog vehicles; for RHS this needs script-side handling (not part of this WP).
- service.repair / other `fort.*`: not used by the scripts.
- Open question for the caller: the Campaign factions use `m_BaseBuildingHQ = E_Headquarters_S_US_01` / `..._USSR_01` (vanilla) for RHS too,
  i.e. RHS itself reuses the vanilla HQ composition. That supports leaving command post to the vanilla fall-through.

## WCS_Hescos (6A20948448913FD7, depends on WCS_Core + base game) - listing only, no set
Hesco barrier compositions/props, all in sizes 1x1, 1x4, 2x4 and variants Desert, Forest, plain (Terrain = terrain-conforming):
- `Prefabs/Props/Military/Fortifications/Hesco/Hesco_{1x1,1x4,2x4}_{Desert,Forest,}{_Terrain,}.et` (15 props)
- `Prefabs/Compositions/Misc/FreeRoamBuilding/Hesco/Hesco_*.et` (15 compositions, same names)
- `Prefabs/Items/Misc/Hesco/DeployablePreview_Hesco_*.et` + `Part_Hesco_{Base,Desert,Forest,Forest_Preview}.et` (deployable kit items)
- `PrefabsEditable/Auto/...Hesco/E_Hesco_*.et` and `.../Hesco/Deployable/E_Hesco_*_Deployable.et` (editor entities)
GUIDs: run the script below with `list Hesco`.

## Rerun script (node, no deps; needs the Enfusion MCP dist)
Save as `rhs.mjs`:
```js
// node rhs.mjs <addonDir> list <regex> | grep <regex over .et/.conf contents> | cat <path> | guid <path> | count
import { readFileSync, existsSync, readdirSync } from "node:fs";
import { join } from "node:path";
import { pathToFileURL } from "node:url";
const D = process.env.ENFUSION_MCP_DIST || "D:/GitHub/enfusion-workbench-mcp/dist";
const { PakVirtualFS } = await import(pathToFileURL(join(D, "pak/vfs.js")).href);
const [root, cmd, arg] = process.argv.slice(2);
const vfs = new PakVirtualFS(readdirSync(root).filter((f) => /^data\d*\.pak$/.test(f)).sort().map((f) => join(root, f)));
const files = vfs.allFilePaths();
const rdbP = join(root, "resourceDatabase.rdb");
const rdb = existsSync(rdbP) ? readFileSync(rdbP) : null;
function guid(v) { // rdb record = u32 len+1, path, 0, 6 bytes, GUID little-endian
  if (!rdb) return null;
  const pb = Buffer.from(v, "latin1"), key = Buffer.concat([Buffer.alloc(4), pb, Buffer.from([0])]);
  key.writeUInt32LE(pb.length + 1, 0);
  const i = rdb.indexOf(key); if (i < 0) return null;
  const g = i + 4 + pb.length + 1 + 6; let h = "";
  for (let k = 7; k >= 0; k--) h += rdb[g + k].toString(16).padStart(2, "0");
  return h.toUpperCase();
}
if (cmd === "list") { const re = new RegExp(arg, "i"); for (const f of files) if (re.test(f)) console.log(f, guid(f) ?? ""); }
else if (cmd === "grep") { const re = new RegExp(arg, "i"); for (const f of files) if (/\.(et|conf)$/i.test(f)) { let t; try { t = vfs.readFile(f).toString("utf8"); } catch { continue; } const m = re.exec(t); if (m) console.log(f, "::", m[0]); } }
else if (cmd === "cat") console.log(vfs.readFile(arg).toString("utf8"));
else if (cmd === "guid") console.log(guid(arg));
else console.log(files.length);
```
Examples: `node rhs.mjs "<addons>/RHS-StatusQuo_595F2BF2F44836FB" list "ArsenalBox"`, `... grep "FactionKey\s+\"[A-Za-z_]+\""`.
Vanilla parents were read with enfusion-mcp `game_read` (ArsenalBox_US, ArsenalBox_Base, VehicleService_US, E_Headquarters_S_US_01).
