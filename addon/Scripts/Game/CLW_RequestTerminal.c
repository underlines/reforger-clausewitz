// Request terminal REMOVED (M4.6 13b): the HQ box (prefab, user actions, spawning) is gone; the persistent U menu (CLW_HqRadial,
// CLW_HqMenuDialog) opens everything. A request at a FOB / HQ is now bound to the base through the depot menu (CLW_DepotAction,
// CLW_DepotLookup) or is an HQ request (chat / U menu, no position).
// The static API below is kept as harmless stubs because CLW_Fob.c and CLW_Rematch.c still call it; delete the stubs together
// with those calls (CLW_Fob: GetOnLost().Remove/Insert(OnFobLost); CLW_Rematch: RemoveAll(), Reset(); CLW_Services: none left).

class CLW_RequestTerminals
{
	// no-op stub (callers: CLW_Rematch, CLW_Services)
	static void Reset()
	{
	}

	// no-op stub (callers: CLW_Rematch, CLW_Services)
	static void RemoveAll()
	{
	}

	// no-op stub (CLW_FobManager.GetOnLost() callback in CLW_Fob)
	static void OnFobLost(string fobId, string sideKey)
	{
	}

	// no-op stub, no terminal is spawned any more (caller: CLW_Services.SpawnOne)
	static void OnServicePlaced(string sideKey, string baseId, string kind, vector servicePos, float radius)
	{
	}

	// stub: there is no terminal, always null (caller: none outside this package after M4.6)
	static IEntity FindAt(vector pos)
	{
		return null;
	}
}
