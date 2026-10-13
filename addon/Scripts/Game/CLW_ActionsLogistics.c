// Logistics intents on the action registry (M3 phase L): RESUPPLY <fob> (HQ -> FOB) and CONVOY <origin> <dest> (any two bases). Layer intent,
// authority hq, capability `logistics`; registered from script (CLW_ActionsLogistics.Register, called by CLW_GameMode right after the
// registry reset). They share the reply channel "fob" with ESTABLISH_FOB / UPGRADE_FOB (the LLM files them under "fobs"). The executors
// start a convoy (CLW_Convoys.Start); the truck, loading and delivery are in CLW_Logistics.c. Executors hold no per-call state.

class CLW_Action_Resupply : CLW_ActionExecutor
{
	// Drops out of the manifest without FOBs to feed or without a supply truck in the faction's catalog (data).
	override bool IsAvailable(string factionKey)
	{
		return CLW_FobManager.GetMax() > 0 && CLW_Convoys.HasTruckData(factionKey);
	}

	override bool Validate(CLW_ActionCall call, out string error)
	{
		string fobId = call.GetString("fob");
		CLW_Fob fob = CLW_FobManager.Find(fobId);
		if (!fob || fob.m_sSide != call.m_sFaction)
		{
			error = "unknown FOB " + fobId;
			return false;
		}

		if (!fob.IsActive())
		{
			error = fobId + " is not active (" + fob.StateName() + ")";
			return false;
		}

		return CLW_Convoys.CanStart(call.m_sFaction, CLW_Services.BASE_HQ, fobId, error);
	}

	override bool Execute(CLW_ActionCall call, out string result)
	{
		string fobId = call.GetString("fob");
		string why;
		if (!CLW_Convoys.Start(call.m_sFaction, CLW_Services.BASE_HQ, fobId, call.m_sSource, call.m_sReason, why))
		{
			result = why;
			return false;
		}

		call.m_iApplied = 1;
		result = "RESUPPLY " + fobId;
		call.m_sResultWire = CLW_Loc.W("#CLW-Act_Resupply", fobId);
		return true;
	}
}

class CLW_Action_Convoy : CLW_ActionExecutor
{
	override bool IsAvailable(string factionKey)
	{
		return CLW_FobManager.GetMax() > 0 && CLW_Convoys.HasTruckData(factionKey);
	}

	override bool Validate(CLW_ActionCall call, out string error)
	{
		return CLW_Convoys.CanStart(call.m_sFaction, call.GetString("origin"), call.GetString("dest"), error);
	}

	override bool Execute(CLW_ActionCall call, out string result)
	{
		string why;
		if (!CLW_Convoys.Start(call.m_sFaction, call.GetString("origin"), call.GetString("dest"), call.m_sSource, call.m_sReason, why))
		{
			result = why;
			return false;
		}

		call.m_iApplied = 1;
		result = "CONVOY " + call.GetString("origin") + " -> " + call.GetString("dest");
		call.m_sResultWire = CLW_Loc.W("#CLW-Act_Convoy", call.GetString("origin"), call.GetString("dest"));
		return true;
	}
}

class CLW_ActionsLogistics
{
	// Script registration. Call right after CLW_ActionRegistry.Reset() (the registry forgets script entries).
	static void Register()
	{
		CLW_ActionRegistry registry = CLW_ActionRegistry.Get();
		CLW_ActionEntry resupply = CLW_ActionEntry.Create("RESUPPLY", CLW_ActionEntry.LAYER_INTENT, "Send a supply convoy from the HQ to a FOB: a supply truck loads min(truck capacity, HQ stock) at the HQ (deducted at departure), drives to the FOB and adds it to the FOB stock on arrival. The truck can be destroyed on the way (cargo lost). One convoy at a time per FOB, max logisticsRules.convoyMax in flight. Use it when a FOB's stock is low (fobs[].stock, logisticsRules.lowStock) and the HQ can spare it.", new CLW_Action_Resupply());
		Prepare(resupply);
		resupply.AddParam("fob", "base", true, "id of an active FOB from fobs in the situation");
		resupply.AddParam("reason", "string", false, "short text");
		registry.Register(resupply);

		CLW_ActionEntry convoy = CLW_ActionEntry.Create("CONVOY", CLW_ActionEntry.LAYER_INTENT, "Send a supply convoy between two bases (HQ or active FOB ids): the load is taken from the origin stock at departure and added to the destination stock on arrival. Use it to move stock between FOBs or back to the HQ; RESUPPLY covers HQ -> FOB.", new CLW_Action_Convoy());
		Prepare(convoy);
		convoy.AddParam("origin", "base", true, "base id the load is taken from: HQ or an active FOB id");
		convoy.AddParam("dest", "base", true, "base id the load is delivered to: HQ or an active FOB id");
		convoy.AddParam("reason", "string", false, "short text");
		registry.Register(convoy);
		Print("CLW: logistics entries registered (RESUPPLY, CONVOY)", LogLevel.NORMAL);
	}

	protected static void Prepare(CLW_ActionEntry entry)
	{
		entry.m_sChannel = "fob";
		entry.m_sAuthority = CLW_ActionEntry.AUTH_HQ;
		entry.m_aRequires.Insert("logistics");
		entry.m_aTags.Insert("logistics");
		entry.m_aTags.Insert("economy");
	}
}
