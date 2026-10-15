# Default arsenal prices (WP10, 2026-10-15)

Problem: pack items (WCS weapons such as the SIG MCX Virtus, RHS ...) ship an `SCR_ArsenalItem` with `m_iSupplyCost 0` and are free in the vanilla arsenal.

## Mechanism (code: `Scripts/Game/CLW_ArsenalPrices.c`, data: `Configs/CLW/ArsenalPrices.conf`)
- Choke point (verified in vanilla `Scripts/Game/Components/Arsenal/SCR_ArsenalItem.c`): vanilla reads `m_iSupplyCost` only in `GetSupplyCost`,
  `GetMilitarySupplyAllocationCost` and `GetRefundAmountValue`. Enforce methods are virtual, so `modded class SCR_ArsenalItem` overrides all three.
  Vanilla arsenal UI/purchase, `SCR_PlayerArsenalLoadout.ComputeSuppliesCost` (loadout cost, WCS_LoadoutEditor, Bacon), weapon attachment/ammo
  sums (they call `GetSupplyCost` of the attached items' own `SCR_ArsenalItem`), `CLW_Kits.Cost/CostAt` (uses ComputeSuppliesCost) and
  `CLW_ItemInfo` (`GetSupplyCost(DEFAULT,false)`) all go through them.
- Rule: only when the item's own `m_iSupplyCost <= 0` (and no alternative cost > 0 for the asked cost type) the default is ADDED to the vanilla
  result (so attachment sums stay). Priced items are untouched. Refund of a defaulted item = its default price.
- Default = first path override ("thermal", "scope" ...) -> first type/mode rule -> `m_iFallbackCost`; times the multiplier of the arsenal cost
  type (GADGET_ARSENAL, RESPAWN_COST), at least `m_iMinimum` (unless 0). A matching rule with cost 0 means "stays free".
- Pure function of item data + table: same on server and clients. Active only while `CLW_ArsenalPool.IsActive()` and `m_bEnabled`.
  Clients without the Clausewitz game mode seen yet (before a taken-over mode's first status) show vanilla prices until then.
- Log (once per faction and machine, from the arsenal pool log): `CLW: services arsenal price <FACTION>: N items priced by default (rifle 31, magazine 90, ...) e.g.: <path>=<price> ...`; table load: `CLW: services arsenal price table loaded`.

## Table values (sources: vanilla US InventoryItems catalog, `Configs/EntityCatalog/US/InventoryItems_EntityCatalog_US.conf`, 203 entries)
Vanilla medians: pistol 5, rifle 8-12 (weapon), sniper 20-40, MG 40-50, launcher 30, MG ammo 5-30 (belts 10-20), rifle ammo 1-5 (2-3), pistol ammo 1,
mortar ammo 1-20 (6), frag grenade 3, smoke 1, explosives 4-15 (5), helmets 0-2 (2), vests 2-25 (2/25), backpack 3-15 (5), radio backpack 50,
attachments 0-75 (10 median, optics 32, thermal 75), plain equipment 0 (8 of 9), torso/legs/footwear/handwear all 0.
Shipped: pistol 5, rifle 12, sniper 30, MG 45, launcher 30, ammo: pistol 1, rifle/other 2, sniper 3, MG 10, launcher 15, mortar 6, medical 3,
attachment 10 (overrides: thermal 75, gpnvg 25, nvg/anpvs 15, scope/optic 20), frag 3, smoke 1, explosives 5, headwear 2, vest 4, backpack 5,
radio backpack 50, clothing 0, plain equipment 0 (weapon-like 2, support station 40), vehicles/helicopters 0, fallback 2.
Note: a weapon's price also includes its attached items' prices (vanilla `m_aAdditionalCosts`), so a weapon with a default scope/magazine costs more than the bare 12.

## Risks
- `modded class SCR_ArsenalItem`: it is a `[BaseContainerProps]` class used by vanilla catalogs (see reforger-script-dev: modding such classes broke SCR_AutoSpawnLogic). Check the first DS start for "Unknown class SCR_ArsenalItem"/empty arsenals.
  Fallback if it breaks: drop the modded class and write the price at the catalog level (needs a setter anyway), or accept vanilla prices.
- `.conf` enum names/array syntax unverified in game (rules use plain enum names like vanilla confs); resource DB rebuild needed for the new `.conf.meta`.
