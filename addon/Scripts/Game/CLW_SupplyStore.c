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

	// One-line stock overview for the logs ("HQ 420, FOB1 60"), so a convoy decision can be read without the stock lines.
	static string Describe(string side)
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		string text = "HQ ";
		if (war)
			text += "" + war.GetSupplies(side);
		else
			text += "?";

		if (!CLW_Capabilities.Has("logistics"))
			return text + " (one pool)";

		array<CLW_Fob> fobs = {};
		CLW_FobManager.GetFobs(side, fobs, true);
		foreach (CLW_Fob fob : fobs)
		{
			text += ", " + fob.m_sId + " " + Get(side, fob.m_sId);
		}

		return text;
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

// Economy guard (M4 round 3 WP6, owner problems P9 / P10): the HQ reserve floor and the AI-cap pre-check, in one place for the
// rule commander, the LLM buy gate (CLW_Action_Buy) and the convoys (CLW_Convoys). Mode params (CLW_WarMode_ShortConflict):
// hqReserve (default 100) and convoyMinLoad (default 60). AI purchases and convoys never take the HQ stock below hqReserve; player
// requests (/request, HQ menu, arsenal kits) are NOT checked here and may (first come). Server only.
// Log prefix: "CLW: economy", the rule commander / convoys log their own lines.
class CLW_EconomyGuard
{
	static const int DEFAULT_HQ_RESERVE = 100;
	static const int DEFAULT_CONVOY_MIN_LOAD = 60;

	protected static ref map<string, string> s_mLogState = new map<string, string>();	// "<key>|<side>" -> last logged state

	static void Reset()
	{
		s_mLogState.Clear();
	}

	// Supplies the HQ keeps back from AI purchases and convoys (mode param hqReserve, 0 = no floor).
	static int HqReserve()
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war || !war.GetMode())
			return DEFAULT_HQ_RESERVE;

		return Math.Clamp(war.GetMode().GetIntParam("hqReserve", DEFAULT_HQ_RESERVE), 0, 2000);
	}

	// Smallest convoy load worth a trip (mode param convoyMinLoad).
	static int ConvoyMinLoad()
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war || !war.GetMode())
			return DEFAULT_CONVOY_MIN_LOAD;

		return Math.Max(1, war.GetMode().GetIntParam("convoyMinLoad", DEFAULT_CONVOY_MIN_LOAD));
	}

	// What the AI (purchases, convoys) may take from the base's stock: HQ stock minus the reserve; a FOB stock has no reserve.
	static int Loadable(string side, string baseId)
	{
		int stock = CLW_SupplyStore.Get(side, baseId);
		if (CLW_SupplyStore.HasOwnStock(side, baseId))
			return stock;

		return Math.Max(0, stock - HqReserve());
	}

	// AI purchase check: true when paying 'cost' leaves the HQ stock at or above the reserve. Otherwise 'why' is the text the LLM sees.
	static bool CanAiSpend(string side, int cost, out string why)
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war)
			return true;

		int stock = war.GetSupplies(side);
		int reserve = HqReserve();
		if (stock - cost >= reserve)
			return true;

		why = string.Format("HQ reserve: stock %1, reserve %2", stock, reserve);
		return false;
	}

	// True when the side is at or over its AI cap (live + pending AI >= cap): any purchase would be refused. Cap 0 = unlimited.
	static bool AiCapFull(string side, out int live, out int cap)
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		CLW_ForceSpawner spawner = CLW_GameMode.GetForceSpawner();
		if (!war || !spawner)
			return false;

		cap = war.GetAiCap(side);
		live = spawner.GetLiveAI(side) + spawner.GetPendingAI(side);
		return cap > 0 && live >= cap;
	}

	// True when 'state' differs from the last state logged under key + side (then it is remembered): log once per change.
	static bool LogChanged(string key, string side, string state)
	{
		string mapKey = key + "|" + side;
		string last;
		if (s_mLogState.Find(mapKey, last) && last == state)
			return false;

		s_mLogState.Set(mapKey, state);
		return true;
	}
}
