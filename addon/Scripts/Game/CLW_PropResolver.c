// Prop resolver: the ONE choke point that turns a ROLE ("service.arsenal", "fob.supply_box", "fort.sandbag_long", ...) of a side into a
// prefab. The role carries the placement data (CLW_ServiceEntry: offset, yaw, radius, footprint, marker); the prefab is only the current
// candidate for it. Both the FOB kit (CLW_Services.SpawnFobKit) and the base services (CLW_Services.SpawnOne) resolve through here.
// Chain (M4 WP3, VISION 10a), first hit with a prefab that LOADS wins:
//   1. admin override  overrides.json "props": per side ("*" = every side) and role a prefab path, "vanilla" or a style set name;
//                      a side "style" applies a set to every role without an explicit entry (CLW_AdminOverrides)
//   2. shipped preference sets  Configs/CLW/Props/PropSets.conf: ordered style sets (role -> candidate prefabs) that name the side
//                      in m_aFactions (pack sets); vanilla sets are not tried here
//   3. the faction data: the side's profile -> the profile named by m_sServiceFallbackFaction -> the profile of a friendly (kin) faction
//                      from the scenario profile list
//   4. the vanilla set (m_bVanilla) that fits the side's kin, as the last resort for a side the chain knows nothing about
//   5. none
// Services of a side with no service data at all are synthesized from steps 1, 2 and 4 (Synthesize), so a pack faction without a
// profile still gets a base. Server only. Log prefix: "CLW: props", one line per side + role with the source.

// One role of a style set: ordered candidate prefabs, the first one that loads is used.
[BaseContainerProps()]
class CLW_PropRoleCandidates
{
	[Attribute("", UIWidgets.EditBox, "Role id: service.command_post, service.supply, service.arsenal, service.vehicle_depot, service.repair, fob.arsenal_box, fob.supply_box, fort.sandbag_long, ...")]
	string m_sRole;

	[Attribute("", UIWidgets.ResourceNamePicker, "Candidate prefabs in order of preference; the first one that loads is used", "et")]
	ref array<ResourceName> m_aPrefabs;
}

// A style set: roles -> candidate prefabs for one look (a pack's props, vanilla US, ...).
[BaseContainerProps()]
class CLW_PropSet
{
	[Attribute("", UIWidgets.EditBox, "Set name (admin override value in overrides.json props, log)")]
	string m_sName;

	[Attribute("0", UIWidgets.CheckBox, "Vanilla set: never tried in the preference step; used by the 'vanilla' override and as the last resort when the faction data has nothing for the role")]
	bool m_bVanilla;

	[Attribute(desc: "Faction keys this set is used for automatically (a pack set names the pack factions). For a vanilla set: the factions it looks like, so a kin side gets the matching one. Empty = only by name")]
	ref array<string> m_aFactions;

	[Attribute()]
	ref array<ref CLW_PropRoleCandidates> m_aRoles;
}

[BaseContainerProps(configRoot: true)]
class CLW_PropSets
{
	[Attribute()]
	ref array<ref CLW_PropSet> m_aSets;
}

class CLW_PropResolver
{
	static const string SERVICE_PREFIX = "service.";
	static const string SETS_PATH = "Configs/CLW/Props/PropSets.conf";
	static const string SYNTH_SOURCE = "propset";
	static const string VANILLA = "vanilla";

	protected static ref array<string> s_aLogged = {};
	protected static ref CLW_PropSets s_Sets;
	protected static bool s_bSetsLoaded;
	protected static ref map<string, bool> s_mLoads = new map<string, bool>();

	// Game start: forget the logged roles and the admin file (statics survive a Play restart).
	static void Reset()
	{
		s_aLogged.Clear();
		s_mLoads.Clear();
		s_bSetsLoaded = false;
		s_Sets = null;
		CLW_AdminOverrides.Reset();
	}

	// Prefab of a role for a side; empty when nothing in the chain has the role. source: "override ...", "set <name>", "profile US",
	// "fallback US", "kin US", "vanilla set <name>" or "none".
	static ResourceName Resolve(string sideKey, string role, out string source)
	{
		string sourceKey;
		CLW_ServiceEntry entry = ResolveEntry(sideKey, role, source, sourceKey);
		if (!entry)
			return "";

		return entry.m_Prefab;
	}

	// The entry (prefab + placement data) of a role; null when none. sourceKey = faction key of the profile the prefab came from,
	// "*" when it came from an override or a set (the prefab's own faction is unknown: CLW_Services.AdoptFaction takes every other one).
	static CLW_ServiceEntry ResolveEntry(string sideKey, string role, out string source, out string sourceKey)
	{
		source = "none";
		sourceKey = "";

		// the placement data and today's candidate: the faction data chain
		string chainSource;
		string chainKey;
		CLW_ServiceEntry chain = ResolveChain(sideKey, role, chainSource, chainKey);

		// 1. admin override
		string how;
		ResourceName wished = FromAdmin(sideKey, role, how);
		if (!wished.IsEmpty())
		{
			CLW_ServiceEntry made = WithPrefab(chain, role, wished);
			source = how;
			sourceKey = "*";
			LogOnce(sideKey, role, made, source);
			return made;
		}

		// 2. shipped preference sets of the side
		CLW_PropSet pset;
		ResourceName preferred = FromSets(sideKey, role, pset);
		if (!preferred.IsEmpty())
		{
			CLW_ServiceEntry fromSet = WithPrefab(chain, role, preferred);
			source = "set " + pset.m_sName;
			sourceKey = "*";
			LogOnce(sideKey, role, fromSet, source);
			return fromSet;
		}

		// 3. the faction data
		if (chain)
		{
			source = chainSource;
			sourceKey = chainKey;
			LogOnce(sideKey, role, chain, source);
			return chain;
		}

		// 4. the vanilla set as the last resort
		CLW_PropSet vanillaSet;
		ResourceName vanillaPrefab = FromVanilla(sideKey, role, vanillaSet);
		if (!vanillaPrefab.IsEmpty())
		{
			CLW_ServiceEntry fromVanilla = WithPrefab(null, role, vanillaPrefab);
			source = "vanilla set " + vanillaSet.m_sName;
			sourceKey = "*";
			LogOnce(sideKey, role, fromVanilla, source);
			return fromVanilla;
		}

		LogOnce(sideKey, role, null, source);
		return null;
	}

	// Service entries for a side whose faction data has none (CLW_Services.ResolveEntries calls this last): one entry per base service
	// kind that overrides, sets or the vanilla set can supply. True when any.
	static bool Synthesize(string sideKey, notnull array<ref CLW_ServiceEntry> entries)
	{
		array<string> kinds = {};
		kinds.Insert(CLW_ServiceKind.COMMAND_POST);
		kinds.Insert(CLW_ServiceKind.SUPPLY);
		kinds.Insert(CLW_ServiceKind.ARSENAL);
		kinds.Insert(CLW_ServiceKind.VEHICLE_DEPOT);
		kinds.Insert(CLW_ServiceKind.REPAIR);
		int found;
		foreach (string kind : kinds)
		{
			string source;
			string sourceKey;
			CLW_ServiceEntry entry = ResolveEntry(sideKey, SERVICE_PREFIX + kind, source, sourceKey);
			if (entry)
			{
				entries.Insert(entry);
				found++;
			}
		}

		Print(string.Format("CLW: props %1: no service data in the faction profiles, %2 base services synthesized from overrides / sets", sideKey, found), LogLevel.NORMAL);
		return found > 0;
	}

	// ---- steps ----

	// Step 1: the admin wish for a role. Empty = none or unusable (a warning is logged once).
	protected static ResourceName FromAdmin(string sideKey, string role, out string how)
	{
		how = "";
		string wish = CLW_AdminOverrides.GetProp(sideKey, role);
		if (!wish.IsEmpty())
			return ResolveWish(sideKey, role, wish, "override", how);

		string style = CLW_AdminOverrides.GetStyle(sideKey);
		if (!style.IsEmpty())
			return ResolveWish(sideKey, role, style, "override style", how);

		return "";
	}

	protected static ResourceName ResolveWish(string sideKey, string role, string wish, string label, out string how)
	{
		how = "";
		string lower = wish;
		lower.ToLower();
		ResourceName result;
		bool styleSetKnown = false;
		if (lower == VANILLA)
		{
			CLW_PropSet vanillaSet;
			result = FromVanilla(sideKey, role, vanillaSet);
			if (!result.IsEmpty())
				how = label + " vanilla (set " + vanillaSet.m_sName + ")";
		}
		else if (IsPath(wish))
		{
			if (PrefabLoads(wish))
			{
				result = wish;
				how = label + " prefab";
			}
		}
		else
		{
			CLW_PropSet named = FindSet(wish);
			styleSetKnown = named != null;
			if (named)
			{
				result = FromSet(named, role);
				if (!result.IsEmpty())
					how = label + " set " + named.m_sName;
			}
		}

		if (result.IsEmpty() && (label == "override" || !styleSetKnown))
			WarnOnce(sideKey, role, wish, label);

		return result;
	}

	// Step 2: the first non-vanilla set that names the side and has a loading candidate for the role.
	protected static ResourceName FromSets(string sideKey, string role, out CLW_PropSet found)
	{
		CLW_PropSets sets = GetSets();
		if (!sets || !sets.m_aSets)
			return "";

		foreach (CLW_PropSet pset : sets.m_aSets)
		{
			if (!pset || pset.m_bVanilla || !pset.m_aFactions || !pset.m_aFactions.Contains(sideKey))
				continue;

			ResourceName prefab = FromSet(pset, role);
			if (!prefab.IsEmpty())
			{
				found = pset;
				return prefab;
			}
		}

		return "";
	}

	// Step 4: a vanilla set with a loading candidate: the one that looks like the side's own or kin faction first, else the first.
	protected static ResourceName FromVanilla(string sideKey, string role, out CLW_PropSet found)
	{
		CLW_PropSets sets = GetSets();
		if (!sets || !sets.m_aSets)
			return "";

		CLW_PropSet firstSet;
		ResourceName firstPrefab;
		foreach (CLW_PropSet pset : sets.m_aSets)
		{
			if (!pset || !pset.m_bVanilla)
				continue;

			ResourceName prefab = FromSet(pset, role);
			if (prefab.IsEmpty())
				continue;

			if (pset.m_aFactions)
			{
				foreach (string key : pset.m_aFactions)
				{
					if (key == sideKey || CLW_Services.AreKin(sideKey, key))
					{
						found = pset;
						return prefab;
					}
				}
			}

			if (!firstSet)
			{
				firstSet = pset;
				firstPrefab = prefab;
			}
		}

		found = firstSet;
		return firstPrefab;
	}

	protected static ResourceName FromSet(CLW_PropSet pset, string role)
	{
		if (!pset.m_aRoles)
			return "";

		foreach (CLW_PropRoleCandidates candidates : pset.m_aRoles)
		{
			if (!candidates || candidates.m_sRole != role || !candidates.m_aPrefabs)
				continue;

			foreach (ResourceName prefab : candidates.m_aPrefabs)
			{
				if (!prefab.IsEmpty() && PrefabLoads(prefab))
					return prefab;
			}
		}

		return "";
	}

	protected static CLW_PropSet FindSet(string name)
	{
		CLW_PropSets sets = GetSets();
		if (!sets || !sets.m_aSets)
			return null;

		string wanted = name;
		wanted.ToLower();
		foreach (CLW_PropSet pset : sets.m_aSets)
		{
			if (!pset)
				continue;

			string candidate = pset.m_sName;
			candidate.ToLower();
			if (candidate == wanted)
				return pset;
		}

		return null;
	}

	// Step 3 (today's chain): the side's profile -> its named fallback faction -> a kin faction's profile from the scenario list.
	protected static CLW_ServiceEntry ResolveChain(string sideKey, string role, out string source, out string sourceKey)
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
			return found;
		}

		if (own && !own.m_sServiceFallbackFaction.IsEmpty() && catalog)
		{
			found = FindIn(catalog.FindProfile(own.m_sServiceFallbackFaction), role);
			if (found)
			{
				source = "fallback " + own.m_sServiceFallbackFaction;
				sourceKey = own.m_sServiceFallbackFaction;
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
				return found;
			}
		}

		return null;
	}

	// ---- FOB kit ----

	// The FOB kit of a side: the first profile in the chain (own, named fallback, kin) with an enabled kit. Returns the faction key it
	// came from, "" when no kit exists anywhere (the caller then falls back to the tier-1 services). The kit entries are the placement
	// data; the prefab of every piece comes from ResolveEntry (overrides and sets apply per role).
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

	// First enabled entry of a profile for a role whose prefab loads: "service.<kind>" looks in m_aServices, any other role in m_aFobKit
	// (by m_sRole).
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
			{
				if (PrefabLoads(entry.m_Prefab))
					return entry;

				WarnPrefab(profile.m_sFactionKey, role, entry.m_Prefab);
			}
		}

		return null;
	}

	// ---- helpers ----

	protected static CLW_PropSets GetSets()
	{
		if (s_bSetsLoaded)
			return s_Sets;

		s_bSetsLoaded = true;
		s_Sets = SCR_ConfigHelperT<CLW_PropSets>.GetConfigObject(SETS_PATH);
		if (!s_Sets)
		{
			Print("CLW: props style sets missing or invalid (" + SETS_PATH + "): no preference or vanilla sets", LogLevel.WARNING);
			return null;
		}

		int count;
		if (s_Sets.m_aSets)
			count = s_Sets.m_aSets.Count();

		Print(string.Format("CLW: props style sets loaded: %1 from %2", count, SETS_PATH), LogLevel.NORMAL);
		return s_Sets;
	}

	// A wish is a prefab path when it looks like a resource name; anything else is a set name.
	protected static bool IsPath(string wish)
	{
		return wish.Contains("/") || wish.Contains("{") || wish.Contains(".et");
	}

	protected static bool PrefabLoads(ResourceName prefab)
	{
		if (prefab.IsEmpty())
			return false;

		string key = prefab;
		bool known;
		if (s_mLoads.Find(key, known))
			return known;

		Resource resource = Resource.Load(prefab);
		bool ok = resource && resource.IsValid();
		s_mLoads.Set(key, ok);
		return ok;
	}

	// A copy of the placement entry (or defaults when there is none) with another prefab.
	protected static CLW_ServiceEntry WithPrefab(CLW_ServiceEntry placement, string role, ResourceName prefab)
	{
		CLW_ServiceEntry made = new CLW_ServiceEntry();
		made.m_Prefab = prefab;
		made.m_sRole = role;
		made.m_bEnabled = true;
		if (placement)
		{
			made.m_sKind = placement.m_sKind;
			made.m_vOffset = placement.m_vOffset;
			made.m_fYaw = placement.m_fYaw;
			made.m_fRadius = placement.m_fRadius;
			made.m_vFootprint = placement.m_vFootprint;
			made.m_iMapMarker = placement.m_iMapMarker;
			made.m_sRole = placement.m_sRole;
			made.m_fGapM = placement.m_fGapM;
			made.m_fRingMinM = placement.m_fRingMinM;
			made.m_fRingMaxM = placement.m_fRingMaxM;
			return made;
		}

		made.m_sKind = KindOfRole(role);
		made.m_fYaw = -1;
		made.m_iMapMarker = -1;
		made.m_fGapM = -1;	// script-made entries get no attribute defaults: -1 = default of the kind
		return made;
	}

	// Service kind of a role without placement data: "service.<kind>" is the kind; kit roles are guessed from their name.
	protected static string KindOfRole(string role)
	{
		if (role.IndexOf(SERVICE_PREFIX) == 0)
			return role.Substring(SERVICE_PREFIX.Length(), role.Length() - SERVICE_PREFIX.Length());

		if (role.Contains("arsenal"))
			return CLW_ServiceKind.ARSENAL;
		if (role.Contains("supply"))
			return CLW_ServiceKind.SUPPLY;

		return CLW_ServiceKind.FORTIFICATION;
	}

	protected static void WarnOnce(string sideKey, string role, string wish, string label)
	{
		string id = "warn|" + sideKey + "|" + role + "|" + wish;
		if (s_aLogged.Contains(id))
			return;

		s_aLogged.Insert(id);
		Print(string.Format("CLW: props %1 %2: %3 '%4' is not usable (prefab does not load, unknown set, or the set has no such role): falling through to the next source", sideKey, role, label, wish), LogLevel.WARNING);
	}

	protected static void WarnPrefab(string profileKey, string role, ResourceName prefab)
	{
		string id = "bad|" + profileKey + "|" + role + "|" + prefab;
		if (s_aLogged.Contains(id))
			return;

		s_aLogged.Insert(id);
		Print(string.Format("CLW: props profile %1 role %2: prefab does not load (%3), skipped", profileKey, role, prefab), LogLevel.ERROR);
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
