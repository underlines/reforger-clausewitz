// Interface between the commanders (P3) and the mission board for human-led squads (P4, spec section 7).
// The commanders produce mission OFFERS (the LLM reply "missions", or the rule commander's own plan via
// CLW_RuleCommander.BuildOffers); this hook receives them. The default implementation only logs and stores them.
// P4 subclasses CLW_MissionHook (e.g. CLW_MissionBoard), overrides the methods and calls CLW_MissionHook.Set(board).

// One offered mission. Exactly one of m_sTarget (objective id) or m_bHasPos/m_vPos is set.
class CLW_MissionOffer
{
	string m_sKind;			// CAPTURE | DEFEND | RECON | DESTROY
	string m_sTarget;		// objective id, "" when the offer is a position
	vector m_vPos;			// target position when m_bHasPos
	bool m_bHasPos;
	string m_sBrief;		// prose brief shown to the player (<= 240 chars), may be empty: the board writes its own
	string m_sSource;		// "rules" | "llm"
}

class CLW_MissionHook
{
	protected static ref CLW_MissionHook s_Instance;
	protected ref map<string, ref array<ref CLW_MissionOffer>> m_mOffers = new map<string, ref array<ref CLW_MissionOffer>>();

	// The active hook (the default implementation until P4 installs its own).
	static CLW_MissionHook Get()
	{
		if (!s_Instance)
			s_Instance = new CLW_MissionHook();

		return s_Instance;
	}

	static void Set(CLW_MissionHook hook)
	{
		s_Instance = hook;
	}

	// True when the hook owns the missions of human-led squads: the rule commander then skips its legacy
	// "one vanilla task per faction" path entirely (P4 returns true).
	bool OwnsHumanSquads()
	{
		return false;
	}

	// Offers for a human-led squad from the LLM or the rules. offers is non-empty, validated (kinds, targets, <= 3, brief <= 240).
	// The board decides whether to publish them (refresh limits, accepted mission present, ...).
	void OnOffers(string factionKey, string squadId, notnull array<ref CLW_MissionOffer> offers)
	{
		array<ref CLW_MissionOffer> stored = new array<ref CLW_MissionOffer>();
		foreach (CLW_MissionOffer offer : offers)
		{
			stored.Insert(offer);
			string target = offer.m_sTarget;
			if (offer.m_bHasPos)
				target = string.Format("%1/%2", Math.Round(offer.m_vPos[0]), Math.Round(offer.m_vPos[2]));
			Print(string.Format("CLW: missions %1 offer for %2: %3 %4 (%5) '%6'", factionKey, squadId, offer.m_sKind, target, offer.m_sSource, offer.m_sBrief), LogLevel.NORMAL);
		}

		m_mOffers.Set(factionKey + "/" + squadId, stored);
	}

	// Offers stored by the default implementation (empty for a board that publishes directly).
	void GetStoredOffers(string factionKey, string squadId, notnull array<ref CLW_MissionOffer> outOffers)
	{
		array<ref CLW_MissionOffer> stored;
		if (m_mOffers.Find(factionKey + "/" + squadId, stored) && stored)
		{
			foreach (CLW_MissionOffer one : stored)
				outOffers.Insert(one);
		}
	}

	// JSON object for the squad's "mission" field in the situation: {"state":"offered|accepted|none","kind":"CAPTURE","target":"C"}.
	// "" = no mission info (written as null).
	string GetMissionJson(string factionKey, string squadId)
	{
		return "";
	}

	// The war switched SETUP -> RUNNING (factions are final now): forget stored offers.
	void OnWarStarted()
	{
		m_mOffers.Clear();
	}
}
