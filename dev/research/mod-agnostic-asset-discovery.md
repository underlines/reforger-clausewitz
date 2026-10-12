## Goal

Make the High Command mod work with arbitrary faction/content mods without hard dependencies on RHS, WCS, GRS, etc.

The HC should automatically discover available:

- weapons and equipment
- characters
- groups
- vehicles
- faction ownership
- basic roles/capabilities

The LLM should reason about normalized capabilities, not raw prefab names.

---

## 1. Primary Source: Faction Entity Catalogs

Use Reforger's Entity Catalog system as the authoritative source.

Relevant catalog types include:

- `ITEM`
- `VEHICLE`
- `CHARACTER`
- `GROUP`

Research APIs around:

```cpp
SCR_EntityCatalogManagerComponent
GetFactionEntityCatalogOfType(...)
GetAllFactionEntityCatalogs(...)
GetFactionArsenalItems(...)
GetAllArsenalItems(...)
```

Expected behavior:

```text
Loaded mods
   ↓
Faction Entity Catalogs
   ↓
HC Asset Registry
```

If RHS/WCS/etc. correctly register their content in faction catalogs, compatibility should require no special integration.

---

## 2. Build an HC Asset Registry

Convert engine/mod content into normalized records.

Example:

```yaml
id: wcs_m1a1
prefab: "{GUID}Prefabs/Vehicles/..."
source_addon: WCS

faction: US
side: BLUFOR

domain: ground
category: armored
role: MBT

crew: 4
passengers: 0

capabilities:
  anti_infantry: high
  anti_vehicle: very_high
  transport: none
```

Do the same for weapons, equipment, groups and characters.

The rest of HC should only query this registry.

---

## 3. Arsenal Support

Prefer faction `ITEM` catalogs.

Discover:

```text
weapons
magazines
attachments
clothing
armor
helmets
grenades
medical
radios
launchers
```

Then build loadouts from role/doctrine rather than fixed item IDs.

Example:

```yaml
rifleman:
  primary: assault_rifle
  magazines: 7
  frag: 2
  smoke: 2
  armor: required
  helmet: required
```

Selection resolves dynamically against available modded items.

---

## 4. Vehicle / Depot Support

Use the faction `VEHICLE` catalog for discovery.

Do not couple HC logic directly to vanilla depot UI.

Prefer:

```text
HC requests capability
       ↓
Asset Registry
       ↓
select concrete vehicle prefab
       ↓
logistics / procurement validation
       ↓
spawn at suitable depot/base/location
```

Example:

```text
HC requests: MBT

Registry candidates:
- M1A1
- Leopard 2A6
- T-72

Doctrine + faction + availability selects one.
```

Research how vanilla Conflict handles:

- vehicle categories
- resource costs
- rank restrictions
- vehicle spawners
- size/type restrictions
- faction ownership

---

## 5. Fallback Discovery

Some mods ship prefabs but do not correctly register them in faction catalogs.

Use layered discovery:

```text
1. Faction Entity Catalog
2. Global/general Entity Catalog
3. Loaded-addon prefab discovery
4. Manual compatibility overrides
```

Research existing approaches used by mods such as:

- AutoArsenal
- GG Everything Arsenal
- Arsenal allowlist mods
- vehicle depot compatibility mods

Avoid blindly enabling every prefab.

Filter out:

```text
wrecks
test assets
prototype prefabs
decorative variants
turrets
damaged variants
internal/editor-only entities
```

---

## 6. Prefab Introspection

For unknown assets, inspect Enfusion components to infer semantic properties.

Example vehicle:

```text
tracked
turret
large-caliber cannon
armor
4 crew
```

Infer:

```text
role ≈ MBT
```

Example:

```text
helicopter
8 passenger seats
door guns
no AT missiles
```

Infer:

```text
role ≈ utility_transport_helicopter
```

Research what information can be extracted from:

- vehicle components
- weapon components
- seat/compartment components
- turrets
- magazines/ammunition
- mobility type
- armor/damage components
- prefab inheritance
- tags/catalog metadata

---

## 7. Compatibility Overrides

Provide a small declarative override layer for assets that cannot be classified reliably.

Example:

```yaml
WCS_M1A1:
  faction: US
  role: MBT
  depot_class: HEAVY
  hc_enabled: true

SomeTestVehicle:
  hc_enabled: false
```

Overrides should augment automatic discovery, not replace it.

Core HC must not depend directly on specific content mods.

---

## 8. Separate Content Discovery from Doctrine

Keep these independent:

```text
Content:
"What assets exist?"

Doctrine:
"How should this faction use them?"
```

Example:

```text
Asset Registry:
M1A1 → MBT

Doctrine:
MBTs form platoons of 4
MBTs avoid dense urban areas
MBTs require infantry support
```

The LLM should reason about:

```text
MBT
IFV
APC
transport helicopter
AT launcher
machine gun
recon group
```

not addon-specific prefab paths.

---

## Target Architecture

```text
Loaded Addons
      ↓
Entity Catalog Discovery
      ↓
Optional Prefab Scan
      ↓
Prefab Introspection
      ↓
Compatibility Overrides
      ↓
HC Asset Registry
      ↓
┌─────────────┬─────────────┬──────────────┐
Loadouts    Group Creation   Procurement
Arsenal     Reinforcement    Vehicle Spawn
      ↓
High Command / LLM Agents
```