# Own-faction roles (squad roles / role picker, WP6)

## Problem
Dedicated-server e2e, RHS_USAF vs RHS_AFRF (RHS + WCS + GRS + TacticalFlava): AI soldiers and spawn-screen slots showed vanilla (1980s) gear.
- `CLW_RolePicker` filled every ROLE_* the faction has no character for with a kin prefab (kin = friendly factions, `CLW_ArsenalPool.GetKin`), e.g.
  `ROLE_GRENADIER=Character_US_GL.et`, `ROLE_MEDIC=Character_US_Medic.et`, `ROLE_AMMOBEARER=Character_USSR_AMG.et`. These became AI squad members
  (composition swap in `CLW_SquadRoles`) and spawn slots ("Grenadier", "Medic").
- `CLW_ForceCatalog.DiscoverGroups` took `Group_USSR_RifleSquad.et` (members `Character_USSR_*`) as a group of RHS_AFRF.

## Rules
1. Own faction first. Per role the picker takes an own CHARACTER catalog prefab (`own`).
2. A role without an own character is SUBSTITUTED by the closest own role (`sub(<ROLE>)`), chains in `Configs/CLW/SquadCompositions.conf`
   (`m_aSubstitutes`, `CLW_SquadCompositionStore.StandIns`). The first stand-in that has a real (own / kin) prefab wins; stand-ins that are themselves
   substitutes never chain. No listed stand-in fits: any role the faction has (default slot order). A role missing from the file defaults to
   `ROLE_RIFLEMAN`. Defaults: GRENADIER/MEDIC/AMMOBEARER/SAPPER/RADIOOPERATOR/MACHINEGUNNER/LEADER -> RIFLEMAN; ANTITANK -> GRENADIER, RIFLEMAN;
   SHARPSHOOTER <-> SCOUT, then RIFLEMAN; RIFLEMAN -> GRENADIER, AMMOBEARER, MEDIC, SCOUT, SAPPER, RADIOOPERATOR, MACHINEGUNNER, ANTITANK, SHARPSHOOTER, LEADER.
3. Kin prefabs only for a TINY pack: fewer than `CLW_RolePicker.MIN_OWN_ROLES` (2) own roles. Logged as WARNING
   `CLW: squad roles picker <faction> TINY pack ...`; roles then show `kin`.
4. Spawn slots (`CLW_LoadoutSlots`, source=role) list `CLW_RolePicker.Roles()` = own roles (kin in a tiny pack) only. Substituted roles get no slot
   (a "Grenadier" slot with a rifleman prefab would be a duplicate). Fewer slots is intended. The custom entry (slot 0) uses the rifleman prefab
   (own, or the stand-in).
5. AI squads (`CLW_SquadRoles.Prepare`): a composition role the faction lacks is played by the stand-in's own prefab; the plan role stays the
   wanted role (kit lookup by role). Pack groups with distinct slot roles keep their designed composition. A `kin` prefab never replaces a slot.
6. Rifleman tie-break: among candidates labelled ROLE_RIFLEMAN the one the faction's own GROUP catalog entries field most often wins (plain
   rifleman is the common member; a GL man labelled only ROLE_RIFLEMAN appears once per squad). Catalog data only, no name heuristics.
   Open: a real "has a grenade launcher" signal (weapon/inventory data of the prefab) is not used, the API was not verified without Workbench.
7. Force catalog: `DiscoverGroups` classifies each group; a group is FOREIGN when none of its member prefabs is in the faction's own CHARACTER
   catalog and at least one is in another faction's CHARACTER catalog (`CLW_RolePicker.IsForeignGroup`). A foreign group is skipped for every kind
   (infantry / recon) the faction has an own group for; if it has none it is kept with a WARNING. Dedupe order and ids are unchanged for factions
   without foreign groups (US, USSR, FIA).

## Why a vanilla group was in RHS_AFRF
`Discover` only reads the faction's own GROUP catalog (`GetFactionEntityCatalogOfType(GROUP)`). Kin imports are tagged `kin_*` / `source=kin`.
The unit `RHS_AFRF_Mot1_Inf` was a normal discovered/own entry, so the most likely cause is that the RHS_AFRF GROUP catalog itself lists vanilla
`Group_USSR_*` entries (pack catalogs often keep the base faction's entries or are merged with them) whose members are not in RHS's CHARACTER
catalog. This could not be confirmed offline: check the new line `catalog discovery <faction>: group ... -> ... foreign=<bool>` in the log. The
other possible route is a motorized entry whose `m_CargoGroupPrefab` came from a kin clone; the log shows `source=kin` then and the kin fill is
by design.

## Data
- `Configs/CLW/SquadCompositions.conf`: `m_aCompositions` (per kind) and `m_aSubstitutes` (per role, `m_aStandIns`).
- Code constants: `CLW_RolePicker.MIN_OWN_ROLES`.

## Log lines
```
CLW: squad roles picker RHS_USAF ROLE_LEADER=own:Character_..._SL.et ROLE_RIFLEMAN=own:... ROLE_GRENADIER=sub(ROLE_RIFLEMAN):Character_RHS_..._Rifleman.et ...
CLW: squad roles picker <faction> TINY pack: only 1 own role(s), kin characters fill in (N roles now)      (WARNING, tiny packs only)
CLW: loadouts slot A 3 role ROLE_MACHINEGUNNER own: Character_...      (spawn slots, one per role slot)
CLW: squad roles plan <unit> <kind> source=<slots|default|dashboard> swapped=N substituted=N fallback=N
CLW: catalog discovery <faction>: group <prefab> skipped (its members are another faction's characters, ...)
CLW: catalog discovery <faction>: group <prefab> -> auto_infantry_N (infantry, 6 members, foreign=false)
```
Watch for: any `kin` in a picker line of a normal faction (must not happen), `Character_US_` / `Character_USSR_` in RHS slot or `slots` lines,
`Group_US*`/`Group_USSR*` in RHS `catalog ... group=` lines, spawn slot count per side, `fallback>0` in plans.
Vanilla US / USSR / FIA: all roles `own` (or `sub(...)` for roles they lack), groups `foreign=false`; behaviour as before.

## Risks
- Client/server picker determinism relies on catalogs only; tiny-pack kin selection depends on `GetKin`, which uses the scenario enemy key (known on
  the server only), as before.
- Rifleman frequency tie-break loads every GROUP catalog prefab once per faction and machine.
- `IsForeignGroup` needs the other factions' CHARACTER catalogs to list the group's members; uncatalogued members count as neither.
