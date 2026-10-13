// Squad roles for AI groups (loadouts-arsenal-plan R0.1 / R0.2 / R4). Server only. Two hooks in CLW_ForceSpawner (and one log hook in CLW_PlayerSquad):
//   Prepare(group, ...)        right after the group entity was spawned and BEFORE its members exist: logs the slot prefabs with their ROLE_* labels
//                              (R0.1) and, when the group needs roles, swaps slot prefabs for the R2 role prefabs (CLW_RolePicker). SCR_AIGroup creates
//                              member i from m_aUnitPrefabSlots[i] when the engine's spawn queue dispatches (ExpandOneMember), which is after this call,
//                              so the official character with its own AI setup spawns; no member is deleted or re-created.
//   OnMembersReady(group)      the members exist: per member the role -> kit (CLW_LoadoutLibrary.PickKitForRole), applied about a second later with
//                              CLW_Kits.Apply, plus the R0.2 log lines. Switch: overrides.json  {"squadKits":"off"}  (default on).
// Roles: members keep their group slot's role (pack groups keep their designed composition). When the slots carry no role or are all the same, or the
// dashboard set a composition for the kind (CLW_LoadoutLibrary.CompositionOverride), the composition of the kind applies (CLW_SquadCompositions,
// Configs/CLW/SquadCompositions.conf). Recruits of a player squad (reinforcements bought by a request) fill the roles the squad still misses.
// Every failure keeps today's behaviour: the slot prefab stays, a member without a kit keeps the gear of its prefab. Log prefix "CLW: squad roles".

// Shape of the optional flag in overrides.json (other keys of the file are ignored).
class CLW_SquadKitFlags
{
	string squadKits;
}

// What Prepare decided for one group; consumed by OnMembersReady.
class CLW_SquadPlan
{
	SCR_AIGroup m_Group;					// weak
	string m_sUnitId;
	string m_sFaction;
	string m_sKind;
	ref array<string> m_aRoles = {};		// role per slot after the swap ("" = unknown)
	int m_iFallback;						// slots that wanted another role but the faction has no prefab for it (slot prefab kept)
}

// One kit to apply to one member, carried through CallLater.
class CLW_KitJob
{
	IEntity m_Character;					// weak
	string m_sKit;
	string m_sUnitId;
	int m_iIndex;
	string m_sRole;
}

class CLW_SquadRoles
{
	protected static const int KIT_DELAY_MS = 1000;			// after the member exists: AI init has run
	protected static const int KITCHECK_DELAY_MS = 10000;

	protected static ref array<ref CLW_SquadPlan> s_aPlans = {};
	protected static bool s_bKitsRead;
	protected static bool s_bKits = true;

	// ---- switch ----

	// overrides.json  {"squadKits":"off"}  disables the kits (the role prefabs stay). Read once.
	static bool KitsEnabled()
	{
		if (s_bKitsRead)
			return s_bKits;

		s_bKitsRead = true;
		if (!FileIO.FileExists(CLW_CatalogOverrides.FILE_PATH))
			return s_bKits;

		string text = SCR_FileIOHelper.GetFileStringContent(CLW_CatalogOverrides.FILE_PATH, false);
		if (text.IsEmpty())
			return s_bKits;

		SCR_JsonLoadContext ctx = new SCR_JsonLoadContext();
		CLW_SquadKitFlags flags = new CLW_SquadKitFlags();
		if (ctx.ImportFromString(text) && ctx.ReadValue("", flags))
		{
			string mode = flags.squadKits;
			mode.ToLower();
			s_bKits = mode != "off";
		}

		Print("CLW: squad roles kits " + s_bKits.ToString() + " (overrides.json squadKits)", LogLevel.NORMAL);
		return s_bKits;
	}

	// Forget plans (war restart).
	static void Reset()
	{
		s_aPlans.Clear();
		s_bKitsRead = false;
	}

	// ---- composition helpers ----

	// Composition cut or stretched to n slots: longer = the first n roles (the leader stays first), shorter = the last role repeats.
	static void Fit(notnull array<string> composition, int n, notnull array<string> result)
	{
		if (composition.IsEmpty())
			return;

		for (int i = 0; i < n; i++)
		{
			if (i < composition.Count())
				result.Insert(composition[i]);
			else
				result.Insert(composition[composition.Count() - 1]);
		}
	}

	// The roles of the living AI soldiers of a squad (players skipped), from their prefabs' ROLE_* labels.
	static void RolesOf(notnull SCR_AIGroup squad, notnull array<string> roles)
	{
		array<IEntity> members = CLW_SquadInfo.Members(squad);
		foreach (IEntity member : members)
		{
			if (!member || GetGame().GetPlayerManager().GetPlayerIdFromControlledEntity(member) > 0)
				continue;

			CharacterControllerComponent controller = CharacterControllerComponent.Cast(member.FindComponent(CharacterControllerComponent));
			if (controller && controller.GetLifeState() == ECharacterLifeState.DEAD)
				continue;

			roles.Insert(RoleOfCharacter(member));
		}
	}

	protected static string RoleOfCharacter(IEntity character)
	{
		EntityPrefabData data = character.GetPrefabData();
		if (!data)
			return "";

		return CLW_RolePicker.RoleOfPrefab(data.GetPrefabName());
	}

	// What a recruit group should be filled with: the composition minus the roles the squad already has (the human leads, so no leader).
	// When the squad is complete the composition (without the leader) starts over.
	protected static void MissingRoles(notnull array<string> composition, notnull SCR_AIGroup squad, notnull array<string> missing)
	{
		array<string> pool = {};
		foreach (string role : composition)
		{
			if (role != "ROLE_LEADER")
				pool.Insert(role);
		}

		array<string> have = {};
		RolesOf(squad, have);
		foreach (string present : have)
		{
			int at = pool.Find(present);
			if (at >= 0)
				pool.Remove(at);
		}

		if (pool.IsEmpty())
		{
			foreach (string again : composition)
			{
				if (again != "ROLE_LEADER")
					missing.Insert(again);
			}

			return;
		}

		foreach (string left : pool)
		{
			missing.Insert(left);
		}
	}

	// ---- hook 1: before the members exist ----

	// group: freshly spawned (members not created yet). kind = CLW_ForceKind of the unit (the vehicle's kind for an embarked group).
	// recruitSquad: the player squad that receives this group's AI (request), null otherwise.
	static void Prepare(SCR_AIGroup group, string unitId, string factionKey, string kind, SCR_AIGroup recruitSquad)
	{
		if (!Replication.IsServer() || !group || !group.m_aUnitPrefabSlots || group.m_aUnitPrefabSlots.IsEmpty())
			return;

		for (int q = s_aPlans.Count() - 1; q >= 0; q--)
		{
			if (!s_aPlans[q].m_Group)
				s_aPlans.Remove(q);	// group vanished before its members were ready
		}

		array<ResourceName> slots = {};
		foreach (ResourceName slot : group.m_aUnitPrefabSlots)
		{
			slots.Insert(slot);
		}

		int count = slots.Count();
		array<string> slotRoles = {};
		string slotLine;
		bool sameRoles = true;
		for (int i = 0; i < count; i++)
		{
			string slotRole = CLW_RolePicker.RoleOfPrefab(slots[i]);
			slotRoles.Insert(slotRole);
			if (slotRole != slotRoles[0])
				sameRoles = false;

			string shown = slotRole;
			if (shown.IsEmpty())
				shown = "none";

			if (i > 0)
				slotLine += ", ";

			slotLine += CLW_RolePicker.ShortName(slots[i]) + ":" + shown;
		}

		string groupPrefab = "?";
		EntityPrefabData groupData = group.GetPrefabData();
		if (groupData)
			groupPrefab = CLW_RolePicker.ShortName(groupData.GetPrefabName());

		Print("CLW: squad roles slots " + unitId + " " + groupPrefab + " [" + slotLine + "]", LogLevel.NORMAL);

		// which composition (if any) applies
		array<string> composition = {};
		string source = "slots";
		array<string> adminRoles = {};
		if (CLW_LoadoutLibrary.CompositionOverride(kind, adminRoles) && !adminRoles.IsEmpty())
		{
			composition = adminRoles;
			source = "dashboard";
		}
		else if (count >= 2 && sameRoles)
		{
			if (CLW_SquadCompositionStore.Default(kind, composition))
				source = "default";
		}

		CLW_SquadPlan plan = new CLW_SquadPlan();
		plan.m_Group = group;
		plan.m_sUnitId = unitId;
		plan.m_sFaction = factionKey;
		plan.m_sKind = kind;
		foreach (string known : slotRoles)
		{
			plan.m_aRoles.Insert(known);
		}

		if (!composition.IsEmpty())
		{
			array<string> wanted = {};
			if (recruitSquad)
				MissingRoles(composition, recruitSquad, wanted);
			else
				wanted = composition;

			array<string> fitted = {};
			Fit(wanted, count, fitted);
			array<ResourceName> fresh = {};
			int swapped;
			for (int j = 0; j < count; j++)
			{
				fresh.Insert(slots[j]);
				string target = fitted[j];
				if (target.IsEmpty() || target == slotRoles[j])
					continue;

				ResourceName rolePrefab = CLW_RolePicker.RolePrefab(factionKey, target);
				if (rolePrefab.IsEmpty())
				{
					plan.m_iFallback++;
					continue;
				}

				fresh.Set(j, rolePrefab);
				plan.m_aRoles.Set(j, target);
				swapped++;
			}

			if (swapped > 0)
				group.m_aUnitPrefabSlots = fresh;	// a new array: never write into a list a prefab may share

			Print(string.Format("CLW: squad roles plan %1 %2 source=%3 swapped=%4 fallback=%5", unitId, kind, source, swapped, plan.m_iFallback), LogLevel.NORMAL);
		}

		s_aPlans.Insert(plan);
	}

	// ---- hook 2: the members exist ----

	static void OnMembersReady(SCR_AIGroup group)
	{
		if (!Replication.IsServer() || !group)
			return;

		CLW_SquadPlan plan;
		for (int p = s_aPlans.Count() - 1; p >= 0; p--)
		{
			if (s_aPlans[p].m_Group == group)
			{
				plan = s_aPlans[p];
				s_aPlans.Remove(p);
				break;
			}
		}

		if (!plan)
			return;

		bool kitsOn = KitsEnabled();
		array<AIAgent> agents = {};
		group.GetAgents(agents);
		string rolesLine;
		string kitsLine;
		int idx;
		foreach (AIAgent agent : agents)
		{
			if (!agent)
				continue;

			IEntity character = agent.GetControlledEntity();
			if (!character)
				continue;

			string role = "";
			if (idx < plan.m_aRoles.Count())
				role = plan.m_aRoles[idx];

			string kitNote = "-";
			if (!role.IsEmpty() && kitsOn)
			{
				string kit = CLW_LoadoutLibrary.PickKitForRole(plan.m_sFaction, plan.m_sKind, role, idx);
				if (!kit.IsEmpty())
				{
					CLW_KitJob job = new CLW_KitJob();
					job.m_Character = character;
					job.m_sKit = kit;
					job.m_sUnitId = plan.m_sUnitId;
					job.m_iIndex = idx;
					job.m_sRole = role;
					GetGame().GetCallqueue().CallLater(ApplyKit, KIT_DELAY_MS, false, job);
					kitNote = "kit";
				}
			}

			if (idx > 0)
			{
				rolesLine += ",";
				kitsLine += ",";
			}

			string shownRole = role;
			if (shownRole.IsEmpty())
				shownRole = "none";

			rolesLine += shownRole;
			kitsLine += kitNote;
			idx++;
		}

		Print("CLW: squad roles " + plan.m_sUnitId + " " + plan.m_sKind + " roles=[" + rolesLine + "] kits=[" + kitsLine + "] fallback=" + plan.m_iFallback.ToString(), LogLevel.NORMAL);
	}

	// Recruits joined a player squad (CLW_PlayerSquad.AdoptAI): log the squad's roles now.
	static void OnRecruitsAdopted(SCR_AIGroup squad, int playerId, int moved)
	{
		if (!squad)
			return;

		array<string> roles = {};
		RolesOf(squad, roles);
		string line;
		foreach (string role : roles)
		{
			if (!line.IsEmpty())
				line += ",";

			string shown = role;
			if (shown.IsEmpty())
				shown = "none";

			line += shown;
		}

		Print(string.Format("CLW: squad roles recruits player=%1 squad=%2 moved=%3 now=[%4]", playerId, squad, moved, line), LogLevel.NORMAL);
	}

	// ---- kits (R0.2) ----

	protected static void ApplyKit(CLW_KitJob job)
	{
		if (!job || !job.m_Character)
			return;

		array<string> missing = {};
		string tag = "ai " + job.m_sUnitId + " slot " + job.m_iIndex.ToString();
		bool done = CLW_Kits.Apply(job.m_Character, job.m_sKit, tag, missing);
		int applied;
		if (done)
			applied = 1;

		int ammo;
		int mags = CountMagazines(job.m_Character, ammo);
		Print(string.Format("CLW: squad roles kit %1 m%2 %3 applied=%4 missing=%5 primary=%6 mags=%7", job.m_sUnitId, job.m_iIndex, job.m_sRole, applied, missing.Count(), WeaponOf(job.m_Character), mags), LogLevel.NORMAL);
		GetGame().GetCallqueue().CallLater(KitCheck, KITCHECK_DELAY_MS, false, job);
	}

	protected static void KitCheck(CLW_KitJob job)
	{
		if (!job || !job.m_Character)
			return;

		int ammo;
		CountMagazines(job.m_Character, ammo);
		Print(string.Format("CLW: squad roles kitcheck %1 m%2 weapon=%3 ammo=%4", job.m_sUnitId, job.m_iIndex, WeaponOf(job.m_Character), ammo), LogLevel.NORMAL);
	}

	// Prefab file name of the weapon in hand, "none" without one.
	protected static string WeaponOf(IEntity character)
	{
		BaseWeaponManagerComponent manager = BaseWeaponManagerComponent.Cast(character.FindComponent(BaseWeaponManagerComponent));
		if (!manager)
			return "none";

		BaseWeaponComponent weapon = manager.GetCurrentWeapon();
		if (!weapon)
			return "none";

		IEntity weaponEntity = weapon.GetOwner();
		if (!weaponEntity)
			return "none";

		EntityPrefabData data = weaponEntity.GetPrefabData();
		if (!data)
			return "none";

		return CLW_RolePicker.ShortName(data.GetPrefabName());
	}

	// Magazines in the character's storages; ammo = rounds in them.
	protected static int CountMagazines(IEntity character, out int ammo)
	{
		ammo = 0;
		InventoryStorageManagerComponent inventory = InventoryStorageManagerComponent.Cast(character.FindComponent(InventoryStorageManagerComponent));
		if (!inventory)
			return 0;

		array<IEntity> items = {};
		inventory.GetItems(items);
		int mags;
		foreach (IEntity item : items)
		{
			if (!item)
				continue;

			BaseMagazineComponent magazine = BaseMagazineComponent.Cast(item.FindComponent(BaseMagazineComponent));
			if (!magazine)
				continue;

			mags++;
			ammo += magazine.GetAmmoCount();
		}

		return mags;
	}
}
