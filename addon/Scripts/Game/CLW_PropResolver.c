// Prop resolver: the ONE choke point that turns a ROLE ("service.arsenal", "fob.supply_box", "fort.sandbag_long", ...) of a side into a
// prefab. The role carries the placement data (CLW_ServiceEntry: offset, yaw, radius, footprint, marker); the prefab is only the current
// candidate for it. Today the chain is: the side's faction profile -> the profile named by m_sServiceFallbackFaction -> the profile of a
// friendly (kin) faction from the scenario profile list -> none. A later change (admin override > preferred packs > vanilla, style sets;
// VISION 10a) only has to change this class. Both the FOB kit (CLW_Services.SpawnFobKit) and the base services (CLW_Services.SpawnOne)
// resolve through here. Server only. Log prefix: "CLW: props", one line per side + role.
class CLW_PropResolver
{
	static const string SERVICE_PREFIX = "service.";

	protected static ref array<string> s_aLogged = {};

	// Game start: forget the logged roles (statics survive a Play restart).
	static void Reset()
	{
		s_aLogged.Clear();
	}

	// Prefab of a role for a side; empty when no profile in the chain has the role. source: "profile US", "fallback US", "kin US" or "none".
	static ResourceName Resolve(string sideKey, string role, out string source)
	{
		string sourceKey;
		CLW_ServiceEntry entry = ResolveEntry(sideKey, role, source, sourceKey);
		if (!entry)
			return "";

		return entry.m_Prefab;
	}

	// The entry (prefab + placement data) of a role; null when none. sourceKey = faction key of the profile it came from.
	static CLW_ServiceEntry ResolveEntry(string sideKey, string role, out string source, out string sourceKey)
	{
		source = "none";
		sourceKey = "";
		CLW_ServiceEntry found;
		CLW_ForceCatalog catalog = CLW_GameMode.GetForceCatalog();
		CLW_FactionProfile own;
		if (catalog)
			own = catalog.FindProfile(sideKey);

		found = FindIn(own, role);
		if (found)
		{
			source = "profile " + sideKey;
			sourceKey = sideKey;
			LogOnce(sideKey, role, found, source);
			return found;
		}

		if (own && !own.m_sServiceFallbackFaction.IsEmpty() && catalog)
		{
			found = FindIn(catalog.FindProfile(own.m_sServiceFallbackFaction), role);
			if (found)
			{
				source = "fallback " + own.m_sServiceFallbackFaction;
				sourceKey = own.m_sServiceFallbackFaction;
				LogOnce(sideKey, role, found, source);
				return found;
			}
		}

		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (scenario && scenario.m_aProfileConfigs)
		{
			foreach (ResourceName path : scenario.m_aProfileConfigs)
			{
				CLW_FactionProfile candidate = CLW_FactionProfile.Load(path);
				if (!candidate || candidate.m_sFactionKey == sideKey)
					continue;

				found = FindIn(candidate, role);
				if (!found || !CLW_Services.AreKin(sideKey, candidate.m_sFactionKey))
					continue;

				source = "kin " + candidate.m_sFactionKey;
				sourceKey = candidate.m_sFactionKey;
				LogOnce(sideKey, role, found, source);
				return found;
			}
		}

		LogOnce(sideKey, role, null, source);
		return null;
	}

	// The FOB kit of a side: the first profile in the chain (own, named fallback, kin) with an enabled kit. Returns the faction key it
	// came from, "" when no kit exists anywhere (the caller then falls back to the tier-1 services).
	static string CollectKit(string sideKey, notnull array<ref CLW_ServiceEntry> entries)
	{
		CLW_ForceCatalog catalog = CLW_GameMode.GetForceCatalog();
		CLW_FactionProfile own;
		if (catalog)
			own = catalog.FindProfile(sideKey);

		if (AppendKit(own, entries))
			return sideKey;

		if (own && !own.m_sServiceFallbackFaction.IsEmpty() && catalog)
		{
			if (AppendKit(catalog.FindProfile(own.m_sServiceFallbackFaction), entries))
				return own.m_sServiceFallbackFaction;
		}

		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (scenario && scenario.m_aProfileConfigs)
		{
			foreach (ResourceName path : scenario.m_aProfileConfigs)
			{
				CLW_FactionProfile candidate = CLW_FactionProfile.Load(path);
				if (!candidate || candidate.m_sFactionKey == sideKey || !CLW_Services.AreKin(sideKey, candidate.m_sFactionKey))
					continue;

				if (AppendKit(candidate, entries))
					return candidate.m_sFactionKey;
			}
		}

		return "";
	}

	protected static bool AppendKit(CLW_FactionProfile profile, notnull array<ref CLW_ServiceEntry> entries)
	{
		if (!profile || !profile.m_aFobKit)
			return false;

		bool any;
		foreach (CLW_ServiceEntry entry : profile.m_aFobKit)
		{
			if (entry && entry.m_bEnabled && !entry.m_Prefab.IsEmpty() && CLW_ServiceKind.IsKnown(entry.m_sKind))
			{
				entries.Insert(entry);
				any = true;
			}
		}

		return any;
	}

	// First enabled entry of a profile for a role: "service.<kind>" looks in m_aServices, any other role in m_aFobKit (by m_sRole).
	protected static CLW_ServiceEntry FindIn(CLW_FactionProfile profile, string role)
	{
		if (!profile)
			return null;

		array<ref CLW_ServiceEntry> list;
		if (role.IndexOf(SERVICE_PREFIX) == 0)
			list = profile.m_aServices;
		else
			list = profile.m_aFobKit;

		if (!list)
			return null;

		foreach (CLW_ServiceEntry entry : list)
		{
			if (entry && entry.m_bEnabled && !entry.m_Prefab.IsEmpty() && CLW_ServiceKind.IsKnown(entry.m_sKind) && entry.GetRole() == role)
				return entry;
		}

		return null;
	}

	protected static void LogOnce(string sideKey, string role, CLW_ServiceEntry entry, string source)
	{
		string id = sideKey + "|" + role;
		if (s_aLogged.Contains(id))
			return;

		s_aLogged.Insert(id);
		string prefab = "none";
		if (entry)
			prefab = entry.m_Prefab.GetPath();

		Print(string.Format("CLW: props %1 %2 -> %3 (source %4)", sideKey, role, prefab, source), LogLevel.NORMAL);
	}
}
