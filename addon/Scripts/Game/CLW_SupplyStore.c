// Per-base supply stock (M3 phase L, design section 5). STOCK MODEL (least invasive):
//   - the side ledger in CLW_WarState (GetSupplies / TrySpend / AddSupplies) IS THE HQ STOCK. Income lands there, every purchase and
//     every existing consumer (rule commander, LLM buy, HUD, situation "supplies") keeps working unchanged.
//   - a FOB has its own stock here (starts at 0, fed by convoys). The side total = HQ ledger + the FOB stocks (GetTotal).
//   - spending "at a base" goes through TrySpend(side, baseId, amount): HQ -> the ledger, FOB -> its own stock.
// With the `logistics` capability OFF there are no FOB stocks: every base id resolves to the one faction pool (the ledger) and the
// supply links mirror it, exactly as before. Server only. Log prefix: "CLW: logistics stock".
class CLW_SupplyStore
{
	protected static ref map<string, int> s_mStock = new map<string, int>();	// "<side>|<fobId>" -> stock

	static void Reset()
	{
		s_mStock.Clear();
	}

	protected static string Key(string side, string baseId)
	{
		return side + "|" + baseId;
	}

	// True when the base id names the HQ (or is empty).
	static bool IsHq(string baseId)
	{
		return baseId.IsEmpty() || baseId == CLW_Services.BASE_HQ;
	}

	// True when the base keeps a stock of its own: logistics on and a FOB id.
	static bool HasOwnStock(string side, string baseId)
	{
		if (IsHq(baseId) || !CLW_Capabilities.Has("logistics"))
			return false;

		return CLW_FobManager.Find(baseId) != null;
	}

	static int Get(string side, string baseId)
	{
		if (HasOwnStock(side, baseId))
		{
			int stock;
			s_mStock.Find(Key(side, baseId), stock);
			return stock;
		}

		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war)
			return 0;

		return war.GetSupplies(side);
	}

	// Adds (or with a negative amount takes, floor 0) to the stock of the base.
	static void Add(string side, string baseId, int amount)
	{
		if (amount == 0)
			return;

		if (HasOwnStock(side, baseId))
		{
			int stock;
			s_mStock.Find(Key(side, baseId), stock);
			stock = Math.Max(0, stock + amount);
			s_mStock.Set(Key(side, baseId), stock);
			Print(string.Format("CLW: logistics stock %1 %2: %3 -> %4", side, baseId, amount, stock), LogLevel.NORMAL);
			return;
		}

		CLW_WarState war = CLW_GameMode.GetWarState();
		if (war)
			war.AddSupplies(side, amount);
	}

	// Charge the base. False (nothing charged) when it cannot pay.
	static bool TrySpend(string side, string baseId, int amount)
	{
		if (amount < 0)
			return false;

		if (HasOwnStock(side, baseId))
		{
			int stock;
			s_mStock.Find(Key(side, baseId), stock);
			if (stock < amount)
			{
				Print(string.Format("CLW: logistics stock %1 %2: spend %3 refused (stock %4)", side, baseId, amount, stock), LogLevel.NORMAL);
				return false;
			}

			s_mStock.Set(Key(side, baseId), stock - amount);
			Print(string.Format("CLW: logistics stock %1 %2: spent %3 -> %4", side, baseId, amount, stock - amount), LogLevel.NORMAL);
			return true;
		}

		CLW_WarState war = CLW_GameMode.GetWarState();
		return war && war.TrySpend(side, amount);
	}

	static void Refund(string side, string baseId, int amount)
	{
		Add(side, baseId, amount);
	}

	// A lost FOB takes its stock with it. Returns the stock that was lost.
	static int Clear(string side, string baseId)
	{
		int stock;
		string key = Key(side, baseId);
		if (!s_mStock.Find(key, stock))
			return 0;

		s_mStock.Remove(key);
		if (stock > 0)
			Print(string.Format("CLW: logistics stock %1 %2: %3 supplies lost with the base", side, baseId, stock), LogLevel.NORMAL);

		return stock;
	}

	// Side total: HQ ledger plus all FOB stocks (logistics off: just the ledger).
	static int GetTotal(string side)
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		int total;
		if (war)
			total = war.GetSupplies(side);

		if (!CLW_Capabilities.Has("logistics"))
			return total;

		array<CLW_Fob> fobs = {};
		CLW_FobManager.GetFobs(side, fobs, true);
		foreach (CLW_Fob fob : fobs)
		{
			total += Get(side, fob.m_sId);
		}

		return total;
	}
}
