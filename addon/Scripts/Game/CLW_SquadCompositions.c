// Squad compositions (loadouts-arsenal-plan R4): per CLW_ForceKind an ordered list of ROLE_* labels. Used by CLW_SquadRoles for groups whose slots carry
// no role or are all identical (and as the fallback when the dashboard sets no composition of its own). Shipped defaults: Configs/CLW/SquadCompositions.conf.
// Longer than the group: the first N roles (the leader stays first). Shorter: the last role repeats (see CLW_SquadRoles.Fit).

[BaseContainerProps()]
class CLW_SquadComposition
{
	[Attribute("", UIWidgets.EditBox, "CLW_ForceKind string (infantry, recon, mech, ...)")]
	string m_sKind;

	[Attribute(desc: "Ordered roles, ROLE_* label names (ROLE_LEADER, ROLE_RIFLEMAN, ...). Slot 0 is the squad leader")]
	ref array<string> m_aRoles;
}

[BaseContainerProps(configRoot: true)]
class CLW_SquadCompositions
{
	[Attribute()]
	ref array<ref CLW_SquadComposition> m_aCompositions;
}

class CLW_SquadCompositionStore
{
	static const ResourceName PATH = "Configs/CLW/SquadCompositions.conf";

	protected static ref CLW_SquadCompositions s_Data;
	protected static bool s_bLoaded;

	static CLW_SquadCompositions GetData()
	{
		if (s_bLoaded)
			return s_Data;

		s_bLoaded = true;
		s_Data = SCR_ConfigHelperT<CLW_SquadCompositions>.GetConfigObject(PATH);
		if (!s_Data)
		{
			Print("CLW: squad roles compositions missing (" + PATH + "), compositions off", LogLevel.WARNING);
			return null;
		}

		int count;
		if (s_Data.m_aCompositions)
			count = s_Data.m_aCompositions.Count();

		Print(string.Format("CLW: squad roles compositions loaded: %1 kinds", count), LogLevel.NORMAL);
		return s_Data;
	}

	// Shipped default roles of a kind. False when the file has none for it (roles stay empty).
	static bool Default(string kind, notnull array<string> roles)
	{
		CLW_SquadCompositions data = GetData();
		if (!data || !data.m_aCompositions)
			return false;

		foreach (CLW_SquadComposition composition : data.m_aCompositions)
		{
			if (!composition || composition.m_sKind != kind || !composition.m_aRoles || composition.m_aRoles.IsEmpty())
				continue;

			foreach (string role : composition.m_aRoles)
			{
				roles.Insert(role);
			}

			return true;
		}

		return false;
	}
}
