// Clearing of SMALL obstacles inside a service footprint (WP11 urban placement). Villages are full of bushes, fences, benches and crates; with
// them counted as blockers every spot in a village failed and the bases ended up in open fields. The CLASSIFICATION is data
// (Configs/CLW/Placement/ClearRules.conf, CLW_ClearRules, documented in dev/notes/overrides.md): an object may be deleted only when
//   - a rule matches it (prefab path substring, entity class name, or "any tree"): an allow-list, anything unknown is never touched,
//   - no never-rule matches (path substrings / class names), it is not a building, character, vehicle, terrain, nor one of our services,
//   - its bounding radius is within the rule's (else the global) limit and it is not taller than the global height limit,
//   - it is REPLICATED (has an RplComponent) when m_bRequireReplicated: deleting a world object on the server only reaches the clients when
//     the entity is replicated; an object without RplComponent would stay (collision, visuals) on every client, so it keeps blocking the spot.
// CLW_FreeSpot.CheckPose treats a clearable object as free and collects it; the services placement deletes the collected objects of the
// FINAL spot only (CLW_PlaceClear.ClearAll). Server only. Log prefix: "CLW: place clear".

[BaseContainerProps()]
class CLW_ClearRule
{
	[Attribute("", UIWidgets.EditBox, "Kind name for the log (vegetation, fence, clutter, prop ...)")]
	string m_sKind;

	[Attribute("1", UIWidgets.CheckBox, "Rule active")]
	bool m_bEnabled;

	[Attribute("0", UIWidgets.CheckBox, "Matches every tree entity (BaseTree / TreeEntity), whatever its prefab path")]
	bool m_bAnyTree;

	[Attribute(desc: "Prefab path substrings (case-insensitive) this rule matches")]
	ref array<string> m_aPathContains;

	[Attribute(desc: "Entity class names (case-insensitive, exact) this rule matches")]
	ref array<string> m_aClassNames;

	[Attribute("0", UIWidgets.EditBox, "Max bounding radius (m) for this rule, 0 = the global limit")]
	float m_fMaxRadiusM;
}

[BaseContainerProps(configRoot: true)]
class CLW_ClearRules
{
	[Attribute("1", UIWidgets.CheckBox, "Master switch: 0 = nothing is ever cleared, only free spots are used")]
	bool m_bEnabled;

	[Attribute("1", UIWidgets.CheckBox, "Clear only replicated entities (RplComponent): the deletion then reaches the clients. Leave on unless static objects were verified to be removed on clients too")]
	bool m_bRequireReplicated;

	[Attribute("3", UIWidgets.EditBox, "Global max bounding radius (m) of a clearable object")]
	float m_fMaxRadiusM;

	[Attribute("8", UIWidgets.EditBox, "Max height (m) of a clearable object")]
	float m_fMaxHeightM;

	[Attribute("60", UIWidgets.EditBox, "Max objects removed for one service footprint (more than that = the spot is not taken as clearable)")]
	int m_iMaxPerSpot;

	[Attribute(desc: "Prefab path substrings (case-insensitive) that are NEVER cleared, whatever a rule says")]
	ref array<string> m_aNeverPathContains;

	[Attribute(desc: "Entity class names (case-insensitive, exact) that are NEVER cleared")]
	ref array<string> m_aNeverClassNames;

	[Attribute(desc: "Allow-list rules, the first matching one decides kind and radius")]
	ref array<ref CLW_ClearRule> m_aRules;
}

class CLW_PlaceClear
{
	static const ResourceName RULES_PATH = "Configs/CLW/Placement/ClearRules.conf";

	protected static ref CLW_ClearRules s_Rules;
	protected static bool s_bLoaded;
	protected static ref array<IEntity> s_aProtected = {};

	static void Reset()
	{
		s_aProtected.Clear();
		s_bLoaded = false;
		s_Rules = null;
	}

	// Our own placed entities (services, kit pieces): never cleared by a later placement.
	static void Protect(IEntity entity)
	{
		if (entity && !s_aProtected.Contains(entity))
			s_aProtected.Insert(entity);
	}

	protected static void LowerAll(array<string> list)
	{
		if (!list)
			return;

		for (int i = 0; i < list.Count(); i++)
		{
			string text = list[i];
			text.ToLower();
			list.Set(i, text);
		}
	}

	// The rules (null when the file is missing or the master switch is off: nothing is cleared then). Loaded once per session.
	static CLW_ClearRules Get()
	{
		if (s_bLoaded)
			return s_Rules;

		s_bLoaded = true;
		CLW_ClearRules loaded = SCR_ConfigHelperT<CLW_ClearRules>.GetConfigObject(RULES_PATH);
		if (!loaded)
		{
			Print("CLW: place clear rules missing (" + RULES_PATH + "): nothing is cleared, only free spots are used", LogLevel.WARNING);
			return null;
		}

		if (!loaded.m_bEnabled)
		{
			Print("CLW: place clear is switched off in " + RULES_PATH + ": only free spots are used", LogLevel.NORMAL);
			return null;
		}

		LowerAll(loaded.m_aNeverPathContains);
		LowerAll(loaded.m_aNeverClassNames);
		int active;
		if (loaded.m_aRules)
		{
			foreach (CLW_ClearRule rule : loaded.m_aRules)
			{
				if (!rule)
					continue;

				LowerAll(rule.m_aPathContains);
				LowerAll(rule.m_aClassNames);
				if (rule.m_bEnabled)
					active++;
			}
		}

		s_Rules = loaded;
		Print(string.Format("CLW: place clear rules loaded: %1 active rules, max radius %2 m, max height %3 m, replicated only=%4, max %5 per spot", active, loaded.m_fMaxRadiusM, loaded.m_fMaxHeightM, loaded.m_bRequireReplicated, loaded.m_iMaxPerSpot), LogLevel.NORMAL);
		return s_Rules;
	}

	static string PrefabPathOf(IEntity entity)
	{
		EntityPrefabData data = entity.GetPrefabData();
		if (!data)
			return "";

		ResourceName prefab = data.GetPrefabName();
		return prefab.GetPath();
	}

	// True when 'entity' (or its root, which is what would be deleted) may be cleared. target = the entity to delete, kind = rule kind,
	// skip = why not ("protected", "never", "nomatch", "big", "norpl") when false.
	static bool Classify(notnull CLW_ClearRules rules, notnull IEntity entity, out IEntity target, out string kind, out string skip)
	{
		IEntity root = entity.GetRootParent();
		if (!root)
			root = entity;

		target = root;
		if (s_aProtected.Contains(root))
		{
			skip = "protected";
			return false;
		}

		if (ChimeraCharacter.Cast(root) || Vehicle.Cast(root) || Building.Cast(root) || GenericTerrainEntity.Cast(root))
		{
			skip = "never";
			return false;
		}

		string path = PrefabPathOf(root);
		path.ToLower();
		string cls = root.ClassName();
		cls.ToLower();
		if (rules.m_aNeverPathContains && !path.IsEmpty())
		{
			foreach (string neverPart : rules.m_aNeverPathContains)
			{
				if (!neverPart.IsEmpty() && path.Contains(neverPart))
				{
					skip = "never";
					return false;
				}
			}
		}

		if (rules.m_aNeverClassNames)
		{
			foreach (string neverClass : rules.m_aNeverClassNames)
			{
				if (neverClass == cls)
				{
					skip = "never";
					return false;
				}
			}
		}

		bool isTree = BaseTree.Cast(root) || TreeEntity.Cast(root);
		CLW_ClearRule matched;
		if (rules.m_aRules)
		{
			foreach (CLW_ClearRule rule : rules.m_aRules)
			{
				if (!rule || !rule.m_bEnabled)
					continue;

				bool hit = false;
				if (rule.m_bAnyTree && isTree)
					hit = true;

				if (!hit && rule.m_aPathContains && !path.IsEmpty())
				{
					foreach (string part : rule.m_aPathContains)
					{
						if (!part.IsEmpty() && path.Contains(part))
						{
							hit = true;
							break;
						}
					}
				}

				if (!hit && rule.m_aClassNames)
				{
					foreach (string className : rule.m_aClassNames)
					{
						if (className == cls)
						{
							hit = true;
							break;
						}
					}
				}

				if (hit)
				{
					matched = rule;
					break;
				}
			}
		}

		if (!matched)
		{
			skip = "nomatch";
			return false;
		}

		vector mins, maxs;
		root.GetWorldBounds(mins, maxs);
		float radius = Math.Max(maxs[0] - mins[0], maxs[2] - mins[2]) * 0.5;
		float height = maxs[1] - mins[1];
		float limit = rules.m_fMaxRadiusM;
		if (matched.m_fMaxRadiusM > 0)
			limit = matched.m_fMaxRadiusM;

		if (radius > limit || height > rules.m_fMaxHeightM)
		{
			skip = "big";
			return false;
		}

		if (rules.m_bRequireReplicated && !RplComponent.Cast(root.FindComponent(RplComponent)))
		{
			skip = "norpl";
			return false;
		}

		kind = matched.m_sKind;
		return true;
	}

	// Deletes the collected objects of a chosen spot (server only), one log line per kind. Returns the number removed.
	static int ClearAll(string label, notnull array<IEntity> list)
	{
		if (list.IsEmpty() || !Replication.IsServer())
			return 0;

		CLW_ClearRules rules = Get();
		if (!rules)
			return 0;

		array<string> kinds = {};
		array<int> counts = {};
		array<string> texts = {};
		int removed;
		foreach (IEntity entity : list)
		{
			if (!entity || entity.IsDeleted())
				continue;

			IEntity target;
			string kind;
			string skip;
			if (!Classify(rules, entity, target, kind, skip) || !target || target.IsDeleted())
				continue;

			string name = PrefabPathOf(target);
			int slot = kinds.Find(kind);
			if (slot < 0)
			{
				kinds.Insert(kind);
				counts.Insert(0);
				texts.Insert("");
				slot = kinds.Count() - 1;
			}

			counts[slot] = counts[slot] + 1;
			if (counts[slot] <= 8)
			{
				if (!texts[slot].IsEmpty())
					texts[slot] = texts[slot] + ", ";

				texts[slot] = texts[slot] + name;
			}
			else if (counts[slot] == 9)
				texts[slot] = texts[slot] + ", ...";

			SCR_EntityHelper.DeleteEntityAndChildren(target);
			removed++;
		}

		foreach (int k, string kindName : kinds)
		{
			Print(string.Format("CLW: place clear %1 removed %2 objects: %3 (%4)", kindName, counts[k], texts[k], label), LogLevel.NORMAL);
		}

		return removed;
	}
}
