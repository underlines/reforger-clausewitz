// Spawn on assignment (owner playtest 2026-10-12): a purchase made for a concrete need carries its first order. The order waits here under
// the purchase tag (CLW_ForceSpawner.BuyTagged) and is applied to the new unit the moment the spawner reports it ready, so a bought unit
// never idles at the base waiting for the next HQ turn. Used by the rule commander (buy for an unmet task) and by BUY entries of the
// LLM reply (command / target / pos). Pending orders also count as coverage of their task, so the commander does not buy twice for it.
// Server only. Log prefix: "CLW: pending order".

class CLW_PendingOrder
{
	string m_sTag;
	string m_sFaction;
	string m_sCommand;		// MOVE | ATTACK | DEFEND | RECON
	string m_sTarget;		// objective id, "" = use m_vPos
	vector m_vPos;
	string m_sReason;
	string m_sSource;		// CLW_Orders.SOURCE_*
	float m_fCreatedAt;
	string m_sUnitId;		// set when the spawner reported the unit; the order is then applied (retried until the registry knows the unit)
	float m_fReadyAt;
	int m_iTries;
}

class CLW_PendingOrders
{
	static const float RETRY_MS = 2000;
	static const int MAX_TRIES = 8;				// after the unit is ready (registry adoption takes up to one sweep)
	static const float EXPIRE_SEC = 300;		// a purchase that never reports back

	protected static ref array<ref CLW_PendingOrder> s_aPending = {};
	protected static int s_iNext;
	protected static bool s_bTicking;

	// Queue an order for the next purchase. Returns the tag to hand to BuyTagged; Remove(tag) when the purchase was refused.
	static string Add(string faction, string command, string target, vector pos, string reason, string source)
	{
		EnsureHook();
		s_iNext++;
		CLW_PendingOrder pending = new CLW_PendingOrder();
		pending.m_sTag = "ord_" + faction + "_" + s_iNext;
		pending.m_sFaction = faction;
		pending.m_sCommand = command;
		pending.m_sTarget = target;
		pending.m_vPos = pos;
		pending.m_sReason = reason;
		pending.m_sSource = source;
		pending.m_fCreatedAt = CLW_Orders.Now();
		s_aPending.Insert(pending);
		return pending.m_sTag;
	}

	static void Remove(string tag)
	{
		for (int i = s_aPending.Count() - 1; i >= 0; i--)
		{
			if (s_aPending[i].m_sTag == tag)
				s_aPending.Remove(i);
		}
	}

	// Orders still waiting for their unit that cover this task (same faction, command and objective).
	static int CountFor(string faction, string command, string target)
	{
		int count;
		foreach (CLW_PendingOrder pending : s_aPending)
		{
			if (pending.m_sFaction == faction && pending.m_sCommand == command && pending.m_sTarget == target)
				count++;
		}

		return count;
	}

	// True for a unit whose first order has not been applied yet (the idle watch and the planner leave it alone).
	static bool IsPendingUnit(string unitId)
	{
		if (unitId.IsEmpty())
			return false;

		foreach (CLW_PendingOrder pending : s_aPending)
		{
			if (pending.m_sUnitId == unitId)
				return true;
		}

		return false;
	}

	static int Count(string faction)
	{
		int count;
		foreach (CLW_PendingOrder pending : s_aPending)
		{
			if (pending.m_sFaction == faction)
				count++;
		}

		return count;
	}

	static void Clear(string faction)
	{
		for (int i = s_aPending.Count() - 1; i >= 0; i--)
		{
			if (s_aPending[i].m_sFaction == faction)
				s_aPending.Remove(i);
		}
	}

	protected static void EnsureHook()
	{
		CLW_ForceSpawner spawner = CLW_GameMode.GetForceSpawner();
		if (spawner)
		{
			spawner.GetOnTagged().Remove(OnTagged);
			spawner.GetOnTagged().Insert(OnTagged);
		}
	}

	// ForceSpawner callback: (tag, unit id) when a tagged purchase is READY, (tag, "") when it failed.
	static void OnTagged(string tag, string unitId)
	{
		CLW_PendingOrder pending;
		foreach (CLW_PendingOrder candidate : s_aPending)
		{
			if (candidate.m_sTag == tag)
			{
				pending = candidate;
				break;
			}
		}

		if (!pending)
			return;

		if (unitId.IsEmpty())
		{
			Print(string.Format("CLW: pending order %1 dropped: the purchase failed (%2 %3 for %4)", tag, pending.m_sCommand, pending.m_sTarget, pending.m_sFaction), LogLevel.NORMAL);
			Remove(tag);
			return;
		}

		pending.m_sUnitId = unitId;
		pending.m_fReadyAt = CLW_Orders.Now();
		if (!TryApply(pending))
			StartTick();
	}

	protected static void StartTick()
	{
		if (s_bTicking)
			return;

		s_bTicking = true;
		GetGame().GetCallqueue().CallLater(Tick, RETRY_MS, true);
	}

	protected static void Tick()
	{
		float now = CLW_Orders.Now();
		for (int i = s_aPending.Count() - 1; i >= 0; i--)
		{
			CLW_PendingOrder pending = s_aPending[i];
			if (pending.m_sUnitId.IsEmpty())
			{
				if (now - pending.m_fCreatedAt > EXPIRE_SEC)
				{
					Print(string.Format("CLW: pending order %1 expired: the purchase never reported ready (%2 s)", pending.m_sTag, Math.Round(now - pending.m_fCreatedAt)), LogLevel.WARNING);
					s_aPending.Remove(i);
				}

				continue;
			}

			if (TryApply(pending))
				continue;

			pending.m_iTries++;
			if (pending.m_iTries >= MAX_TRIES && i < s_aPending.Count() && s_aPending[i] == pending)
			{
				Print(string.Format("CLW: pending order %1 given up for %2 after %3 tries", pending.m_sTag, pending.m_sUnitId, pending.m_iTries), LogLevel.WARNING);
				s_aPending.Remove(i);
			}
		}

		if (s_aPending.IsEmpty())
		{
			GetGame().GetCallqueue().Remove(Tick);
			s_bTicking = false;
		}
	}

	// True when the order was applied (and the entry removed) or finally refused; false = try again later (unit not adopted yet).
	protected static bool TryApply(CLW_PendingOrder pending)
	{
		CLW_RemoteCommander commander = CLW_GameMode.GetCommander(pending.m_sFaction);
		if (!commander || !commander.GetRules())
		{
			Print(string.Format("CLW: pending order %1 dropped: no commander for %2", pending.m_sTag, pending.m_sFaction), LogLevel.WARNING);
			Remove(pending.m_sTag);
			return true;
		}

		string why;
		if (!commander.GetRules().ApplyOrderAs(pending.m_sUnitId, pending.m_sCommand, pending.m_sTarget, pending.m_vPos, pending.m_sReason, pending.m_sSource, why))
		{
			if (why.IndexOf("unknown or not commandable") == 0)
				return false;	// the registry has not adopted the new group yet: next tick

			Print(string.Format("CLW: pending order %1 refused for %2: %3", pending.m_sTag, pending.m_sUnitId, why), LogLevel.WARNING);
			Remove(pending.m_sTag);
			return true;
		}

		float waited = CLW_Orders.Now() - pending.m_fCreatedAt;
		Print(string.Format("CLW: pending order %1 applied: %2 %3 -> %4 %5 (%6, %7 s after the buy)", pending.m_sTag, pending.m_sFaction, pending.m_sUnitId, pending.m_sCommand, pending.m_sTarget, pending.m_sReason, Math.Round(waited)), LogLevel.NORMAL);
		Remove(pending.m_sTag);
		return true;
	}

	// Rematch (CLW_Rematch.c): drop the orders waiting for purchases of the finished war.
	static void RematchReset()
	{
		GetGame().GetCallqueue().Remove(Tick);
		s_bTicking = false;
		s_aPending.Clear();
		s_iNext = 0;
	}
}
