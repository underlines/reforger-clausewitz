# Test projects for mod-pack factions (dev only, never shipped)

`ClausewitzRHS/addon.gproj` and `ClausewitzWCS/addon.gproj` are empty Workbench projects whose only purpose is to put
Clausewitz and a set of Workshop packs into one dependency chain, so the faction choice / catalog discovery can be tested
without Clausewitz depending on any pack (`addon/addon.gproj` stays untouched: base game only). Background:
`dev/research/modpacks-factions-testing.md`.

Project GUIDs: ClausewitzRHS `D2B16F8C4EA05B39`, ClausewitzWCS `D2B16F8C4EA05B3A`. Dependency GUIDs: base `58D0FB3206B6F859`,
Clausewitz `C1A05E7B3D9F4A28`, RHS Content Pack 01 `1337C0DE5DABBEEF`, Pack 02 `BADC0DEDABBEDA5E`, RHS Status Quo `595F2BF2F44836FB`;
WCS list in `ClausewitzWCS/addon.gproj`.

## 1. Make the dependencies visible to Workbench
Workbench resolves dependency GUIDs from the game `addons/`, its own `Documents\My Games\ArmaReforgerWorkbench\addons`, the
folders next to the opened project and launcher-registered projects. It does NOT search the Workshop folder
(`Documents\My Games\ArmaReforger\addons`). Proposed (not run by anyone yet; the lead asks the user first), PowerShell, no
admin needed for junctions. Fall back to a real copy only if Workbench ignores a junction.

```powershell
$wb  = "C:\Users\Jan\Documents\My Games\ArmaReforgerWorkbench\addons"
$ws  = "C:\Users\Jan\Documents\My Games\ArmaReforger\addons"
# Clausewitz itself (the project folder is the addon/ subfolder)
New-Item -ItemType Junction -Path "$wb\Clausewitz_C1A05E7B3D9F4A28" -Target "D:\GitHub\ReforgerMods\Clausewitz\addon"
# RHS (3 folders)
foreach ($n in "RHS-ContentPack01_1337C0DE5DABBEEF","RHS-ContentPack02_BADC0DEDABBEDA5E","RHS-StatusQuo_595F2BF2F44836FB") {
  New-Item -ItemType Junction -Path "$wb\$n" -Target "$ws\$n" }
# WCS: one junction per folder of the list in section "WCS test" below (folder name = <Name>_<GUID> as in $ws)
```
Verify with `workshop_check_deps gproj_path=D:\GitHub\ReforgerMods\Clausewitz\dev\testpacks\ClausewitzRHS\addon.gproj`:
every dependency must be `wb-visible`. Remove the junctions afterwards with `Remove-Item <junction>` (no `-Recurse`; this
removes only the link, never the target).

## 2. Duplicate EnfusionMCP handler scripts
`wb_launch` copies `Scripts/WorkbenchGame/EnfusionMCP/*.c` into the OPENED project. `Clausewitz/addon/Scripts/WorkbenchGame/`
(gitignored) normally holds a copy too, so two copies are in the dependency chain and clash (duplicate classes).
Before opening a test project: run `wb_cleanup` for the Clausewitz addon (or delete `addon/Scripts/WorkbenchGame/`), open
the test project (its copy is created), and after the test `wb_cleanup` again. The copies in `dev/testpacks/**` are gitignored.

## 3. Launch
`wb_launch gprojPath=D:\GitHub\ReforgerMods\Clausewitz\dev\testpacks\ClausewitzRHS\addon.gproj`, first Play asks "allow
scripts" -> "Yes, all". Open `{GUID}Worlds/CLW_Arland.ent` (Clausewitz GUID prefix) via `wb_open_resource`, Play.
8.4 GB of RHS data: first load is slow.

## RHS test: what to verify (logs, prefix `CLW:`)
- Setup menu "Side A / Side B" lists the RHS factions (`RHS_USAF`, `RHS_AFRF`, `RHS_ION`) next to US / USSR / FIA; CIV and
  RNGD are absent (scenario ignore list). `CLW: setup server started; factions offered:` shows the list.
- Pick `RHS_USAF` vs `RHS_AFRF`, start: `CLW: sides applying`, `sides faction ... playable=`, `sides ... hostile=true`,
  `bases side 0 re-keyed US -> RHS_USAF`, `points ... re-keyed`, `war ledgers re-keyed`, `sides removing unused scene group`
  (the US/USSR scene squads), `registry re-keyed`, `sides catalog RHS_USAF: N enabled entries` (discovery log shows the kinds
  found), `sides faction RHS_USAF has N player loadout(s)` (no ERROR line).
- Players: the player is moved to side A (`sides moving player`), respawns at the side A base as an RHS rifleman and joins
  a RHS squad (first adopted squad if the preferred scene squad was removed).
- Start forces appear for both sides (`kind match` lines when the type ids of the data do not exist in the pack catalog).
- `GetFactionsCount` expectation: 4 vanilla (US, USSR, FIA, CIV) + 3 RHS.

## WCS test: what to verify
Folders to junction (each `<Name>_<GUID>` from the Workshop dir): WCS_Core, WCS_Armament, WCS_VehicleLock, SpaceCore,
WCS_Vehicles, M1Abrams, T-72MainBattleTank, M2BradleyFightingVehicle, BMP-1IFV, BMP-3IFV, STRYKER, FMTV.
- WCS adds no factions. Start US vs USSR: the catalog log of `US` / `USSR` shows additional vehicle entries; M1A1/M2A2/T-72
  etc. are classified `armor` / `mech` by the generic classifier (e.g. an M1 without cargo = `armor`), appear in the buy list.
- Buying a WCS tank spawns it crewed; check the crew count and that HQ can order it.
- Expect WCS_Core side effects (below); do not put "WCS" in a test server name (licence watermark).

## Deliberately excluded (and why)
- `WCS_Core` AI changes are accepted only because `WCS_Vehicles` requires it (modded `SCR_AIGetEmptyCompartment`, `SCR_AIGroup`,
  wreck deletion after 130 s that disturbs our vehicle-destroyed timing): watch for AI boarding differences.
- `WCS_VehicleLock` (hard dependency of `WCS_Vehicles`): key system for players; AI boarding should be unaffected, players may
  not be able to enter spawned vehicles (check).
- Not listed at all: `WCS_Commands`, `WCS_Squads`, `WCS_Rules`, `WCS_AFKKick` (kicks idle testers), `WCS_SpawnProtection`,
  `WCS_Interface`, `WCS_VON`, `WCS_LoadoutEditor`, `WCS_Settings`, `WCS_Helicopters`, `WCS_Hescos`, WCS map scenarios,
  `WCS_NATO` / `WCS_RU` (weapon packs, no factions or vehicles), `WCSNatotoUS` (depends on RHS), all `RHStoVanilla*`,
  `WCS_RHS_*`, `ADSSway-*`, `AdvancedAICommand*`, `AIAirborne`, `AIUseDrones`, `CRX_EAI`, `ACE*`, `ServerAdminTools`, `TAO`,
  `REAPER`, `VIC-Limiter`: unrelated or they change AI/player behaviour we want to observe unmodified.
- RHS and WCS are never mixed in one test project (WCS vehicle test stays vanilla-faction based).
