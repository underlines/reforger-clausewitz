// One capturable point (runtime state). Progress is signed: +100 = fully held by the scenario's first faction,
// -100 = second faction, 0 = neutral. Use CLW_ControlPoints.ProgressFor for a faction's point of view.
class CLW_ControlPoint
{
	string m_sId;
	string m_sName;
	vector m_vPos;
	float m_fRadius;
	int m_iValue;
	string m_sOwner;		// faction key, "" = neutral
	float m_fProgress;		// -100..100, see above
	bool m_bContested;
	int m_iCountFirst;		// living characters of the first / second faction inside, last tick
	int m_iCountSecond;
	ref SCR_MapMarkerBase m_Marker;
	string m_sMarkerOwner = "?";	// owner the current marker was built for ("?" = none inserted)
}

// Own capture logic (spec section 1), no trigger prefab. Server only, 2 s tick.
class CLW_ControlPoints
{
	protected ref array<ref CLW_ControlPoint> m_aPoints = {};
	protected ref array<ref SCR_MapMarkerBase> m_aBaseMarkers = {};
	protected ref array<string> m_aBaseMarkerKeys = {};
	protected ref ScriptInvoker m_OnOwnerChanged = new ScriptInvoker();	// (string id, string oldOwner, string newOwner)
	protected ref ScriptInvoker m_OnContested = new ScriptInvoker();	// (string id)
	static const float OBJECTIVE_SITE_RADIUS_MIN_M = 150;	// layout: search radius around a scaled objective position
	protected string m_sFirst;
	protected string m_sSecond;
	protected bool m_bRunning;
	// scratch state of the sphere query callback
	protected int m_iQueryFirst;
	protected int m_iQuerySecond;

	ScriptInvoker GetOnOwnerChanged()
	{
		return m_OnOwnerChanged;
	}

	ScriptInvoker GetOnContested()
	{
		return m_OnContested;
	}

	void Init(notnull CLW_ScenarioConfig scenario)
	{
		if (!Replication.IsServer())
			return;

		m_sFirst = scenario.GetFactionKey(0);
		m_sSecond = scenario.GetFactionKey(1);
		BaseWorld world = GetGame().GetWorld();
		foreach (CLW_ObjectiveSetup setup : scenario.m_aObjectives)
		{
			CLW_ControlPoint point = new CLW_ControlPoint();
			point.m_sId = setup.m_sId;
			point.m_sName = setup.m_sName;
			point.m_vPos = setup.m_vPos;
			point.m_vPos[1] = world.GetSurfaceY(setup.m_vPos[0], setup.m_vPos[2]);
			point.m_fRadius = setup.m_fRadius;
			point.m_iValue = setup.m_iValue;
			point.m_sOwner = setup.m_sStartOwner;
			if (point.m_sOwner == m_sFirst)
				point.m_fProgress = 100;
			else if (point.m_sOwner == m_sSecond)
				point.m_fProgress = -100;
			m_aPoints.Insert(point);
			Print(string.Format("CLW: points %1 '%2' at %3 r=%4 value=%5 owner='%6'", point.m_sId, point.m_sName, point.m_vPos, point.m_fRadius, point.m_iValue, point.m_sOwner), LogLevel.NORMAL);
		}

		m_bRunning = true;
		GetGame().GetCallqueue().CallLater(Tick, CLW_WarConfig.CAPTURE_TICK_MS, true);
		Print(string.Format("CLW: points started: %1 points, %2 vs %3", m_aPoints.Count(), m_sFirst, m_sSecond), LogLevel.NORMAL);
	}

	// The scenario sides were assigned other factions (CLW_Sides.Apply, before the war runs): adopt the new keys, reset every
	// point to its start ownership (the objective's start owner names a side slot's default key -> mapped to the faction now
	// on that slot) and force the map markers to be rebuilt for the new identities.
	void Rekey(notnull CLW_ScenarioConfig scenario)
	{
		if (!Replication.IsServer())
			return;

		m_sFirst = scenario.GetFactionKey(0);
		m_sSecond = scenario.GetFactionKey(1);
		foreach (CLW_ControlPoint point : m_aPoints)
		{
			CLW_ObjectiveSetup setup = scenario.FindObjective(point.m_sId);
			string owner;
			if (setup)
				owner = scenario.MapDefaultKey(setup.m_sStartOwner);
			point.m_sOwner = owner;
			point.m_bContested = false;
			point.m_fProgress = 0;
			if (owner == m_sFirst)
				point.m_fProgress = 100;
			else if (owner == m_sSecond)
				point.m_fProgress = -100;
			point.m_sMarkerOwner = "?";
			Print(string.Format("CLW: points %1 re-keyed owner='%2'", point.m_sId, owner), LogLevel.NORMAL);
		}

		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		if (manager)
		{
			foreach (SCR_MapMarkerBase baseMarker : m_aBaseMarkers)
			{
				manager.RemoveStaticMarker(baseMarker);
			}
		}

		m_aBaseMarkers.Clear();
		m_aBaseMarkerKeys.Clear();
		Print(string.Format("CLW: points re-keyed: %1 vs %2", m_sFirst, m_sSecond), LogLevel.NORMAL);
	}

	// Battle layout (called once when the war starts, after Rekey): drops the objectives the chosen battle size does not use
	// (CLW_ObjectiveSetup.m_iMinBattleSize), and when the HQs were moved (factor != 1) scales the remaining objectives about the
	// authored HQ midpoint by the same factor, then snaps each to a usable site that keeps hqClear metres from both HQs.
	// Objectives keep their id, name, value, radius and start owner. Scenario and runtime points stay consistent.
	void ApplyLayout(notnull CLW_ScenarioConfig scenario, int sizeIndex, string sizeName, vector mid, float factor, array<vector> hqs, float hqClear)
	{
		if (!Replication.IsServer())
			return;

		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();

		// 1. which objectives does this battle size use? (never drop all of them)
		array<string> drop = {};
		if (sizeIndex >= 0)
		{
			foreach (CLW_ObjectiveSetup setup : scenario.m_aObjectives)
			{
				if (setup.m_iMinBattleSize > sizeIndex)
					drop.Insert(setup.m_sId);
			}
		}

		if (drop.Count() >= m_aPoints.Count())
			drop.Clear();

		foreach (string dropId : drop)
		{
			CLW_ControlPoint gone = Find(dropId);
			if (gone)
			{
				if (gone.m_Marker && manager)
					manager.RemoveStaticMarker(gone.m_Marker);
				m_aPoints.RemoveItem(gone);
			}

			CLW_ObjectiveSetup goneSetup = scenario.FindObjective(dropId);
			if (goneSetup)
				scenario.m_aObjectives.RemoveItem(goneSetup);
			Print(string.Format("CLW: points layout battleSize=%1: objective %2 dropped (needs a bigger battle)", sizeName, dropId), LogLevel.NORMAL);
		}

		// 2. scale + snap the rest
		float minGap = 1000000000;
		if (Math.AbsFloat(factor - 1) >= 0.0001)
		{
			array<vector> placed = {};
			foreach (CLW_ControlPoint point : m_aPoints)
			{
				vector wish = mid + (point.m_vPos - mid) * factor;
				float hqDist = 0;
				foreach (vector hq : hqs)
				{
					float d = vector.DistanceXZ(wish, hq);
					if (hqDist == 0 || d < hqDist)
						hqDist = d;
				}

				float searchRadius = Math.Max(OBJECTIVE_SITE_RADIUS_MIN_M, point.m_fRadius * 2);
				array<vector> avoid = {};
				avoid.Copy(hqs);
				vector site;
				float roadDist;
				float score;
				bool ok = CLW_Placement.FindSite(wish, searchRadius, avoid, hqClear + point.m_fRadius, site, roadDist, score);
				if (!ok)
				{
					// nothing acceptable in reach: widen once, and keep the clearance
					ok = CLW_Placement.FindSite(wish, searchRadius * 3, avoid, hqClear + point.m_fRadius, site, roadDist, score);
					if (!ok)
						Print(string.Format("CLW: points WARNING objective %1 has no good site near %2: raw scaled position", point.m_sId, wish), LogLevel.WARNING);
				}

				Print(string.Format("CLW: points layout %1 %2 -> %3 (scaled %4, moved %5 m, nearest HQ %6 m, nearestRoad=%7 m)", point.m_sId, point.m_vPos, site, wish, Math.Round(vector.DistanceXZ(site, wish)), Math.Round(hqDist), roadDist), LogLevel.NORMAL);
				point.m_vPos = site;
				CLW_ObjectiveSetup setup2 = scenario.FindObjective(point.m_sId);
				if (setup2)
					setup2.m_vPos = site;

				point.m_sMarkerOwner = "?";
				if (point.m_Marker && manager)
				{
					manager.RemoveStaticMarker(point.m_Marker);
					point.m_Marker = null;
				}

				foreach (vector other : placed)
				{
					float gap = vector.DistanceXZ(site, other);
					if (gap < minGap)
						minGap = gap;
				}

				placed.Insert(site);
			}
		}

		// 3. HQ markers follow the HQs (rebuilt by the next tick)
		if (manager)
		{
			foreach (SCR_MapMarkerBase baseMarker : m_aBaseMarkers)
			{
				manager.RemoveStaticMarker(baseMarker);
			}
		}

		m_aBaseMarkers.Clear();
		m_aBaseMarkerKeys.Clear();

		int total;
		foreach (CLW_ControlPoint kept : m_aPoints)
		{
			total += kept.m_iValue;
		}

		Print(string.Format("CLW: points layout battleSize=%1: %2 objectives (total value %3), scale=%4, closest pair %5 m, HQ clearance %6 m", sizeName, m_aPoints.Count(), total, factor, Math.Round(minGap), Math.Round(hqClear)), LogLevel.NORMAL);
	}

	void Stop()
	{
		m_bRunning = false;
		GetGame().GetCallqueue().Remove(Tick);
	}

	// ---- getters (situation builder, dashboard, war state) ----

	int Count()
	{
		return m_aPoints.Count();
	}

	CLW_ControlPoint Get(int index)
	{
		if (index < 0 || index >= m_aPoints.Count())
			return null;

		return m_aPoints[index];
	}

	CLW_ControlPoint Find(string id)
	{
		foreach (CLW_ControlPoint point : m_aPoints)
		{
			if (point.m_sId == id)
				return point;
		}

		return null;
	}

	// Progress from a faction's view: +100 = fully its own, -100 = fully the enemy's.
	float ProgressFor(CLW_ControlPoint point, string factionKey)
	{
		if (!point)
			return 0;

		if (factionKey == m_sSecond)
			return -point.m_fProgress;

		return point.m_fProgress;
	}

	int HeldCount(string factionKey)
	{
		int n;
		foreach (CLW_ControlPoint point : m_aPoints)
		{
			if (point.m_sOwner == factionKey)
				n++;
		}

		return n;
	}

	// Sum of the values of the points a faction owns.
	int HeldValue(string factionKey)
	{
		int total;
		foreach (CLW_ControlPoint point : m_aPoints)
		{
			if (point.m_sOwner == factionKey)
				total += point.m_iValue;
		}

		return total;
	}

	// True when the faction owns every point (and there is at least one).
	bool HoldsAll(string factionKey)
	{
		return !m_aPoints.IsEmpty() && HeldCount(factionKey) == m_aPoints.Count();
	}

	// ---- capture logic ----

	protected void Tick()
	{
		if (!m_bRunning)
			return;

		float dt = CLW_WarConfig.CAPTURE_TICK_MS / 1000.0;
		foreach (CLW_ControlPoint point : m_aPoints)
		{
			CountPresent(point);
			UpdatePoint(point, dt);
		}

		SyncMarkers();
	}

	protected void CountPresent(CLW_ControlPoint point)
	{
		m_iQueryFirst = 0;
		m_iQuerySecond = 0;
		GetGame().GetWorld().QueryEntitiesBySphere(point.m_vPos, point.m_fRadius, QueryCharacter, null, EQueryEntitiesFlags.DYNAMIC);
		point.m_iCountFirst = m_iQueryFirst;
		point.m_iCountSecond = m_iQuerySecond;
	}

	// QueryEntitiesBySphere callback: count living characters per faction. Return true to continue.
	protected bool QueryCharacter(IEntity entity)
	{
		SCR_ChimeraCharacter character = SCR_ChimeraCharacter.Cast(entity);
		if (!character)
			return true;

		CharacterControllerComponent controller = character.GetCharacterController();
		if (!controller || controller.GetLifeState() == ECharacterLifeState.DEAD)
			return true;

		string key = character.GetFactionKey();
		if (key == m_sFirst)
			m_iQueryFirst++;
		else if (key == m_sSecond)
			m_iQuerySecond++;

		return true;
	}

	protected void UpdatePoint(CLW_ControlPoint point, float dt)
	{
		bool contested = point.m_iCountFirst > 0 && point.m_iCountSecond > 0;
		if (contested != point.m_bContested)
		{
			point.m_bContested = contested;
			Print(string.Format("CLW: points %1 contested=%2 (%3=%4 %5=%6) progress=%7", point.m_sId, contested, m_sFirst, point.m_iCountFirst, m_sSecond, point.m_iCountSecond, Math.Round(point.m_fProgress)), LogLevel.NORMAL);
			if (contested)
				m_OnContested.Invoke(point.m_sId);
		}

		if (contested)
			return;	// frozen

		float before = point.m_fProgress;
		if (point.m_iCountFirst > 0 || point.m_iCountSecond > 0)
		{
			int n = point.m_iCountFirst;
			if (point.m_iCountSecond > n)
				n = point.m_iCountSecond;
			if (n > CLW_WarConfig.CAPTURE_MAX_COUNTED)
				n = CLW_WarConfig.CAPTURE_MAX_COUNTED;
			float rate = CLW_WarConfig.CAPTURE_RATE_PCT_PER_SEC * n / 2;
			if (point.m_iCountFirst > 0)
				point.m_fProgress = Math.Clamp(point.m_fProgress + rate * dt, -100, 100);
			else
				point.m_fProgress = Math.Clamp(point.m_fProgress - rate * dt, -100, 100);
		}
		else
		{
			// nobody present: decay toward the current owner (0 when neutral)
			float target = 0;
			if (point.m_sOwner == m_sFirst)
				target = 100;
			else if (point.m_sOwner == m_sSecond)
				target = -100;

			float step = CLW_WarConfig.CAPTURE_DECAY_PCT_PER_SEC * dt;
			if (point.m_fProgress < target)
				point.m_fProgress = Math.Min(point.m_fProgress + step, target);
			else if (point.m_fProgress > target)
				point.m_fProgress = Math.Max(point.m_fProgress - step, target);
		}

		// owner transitions: capture at +-100, neutralise when the progress crosses 0
		string newOwner = point.m_sOwner;
		if (point.m_fProgress >= 100)
			newOwner = m_sFirst;
		else if (point.m_fProgress <= -100)
			newOwner = m_sSecond;
		else if (point.m_sOwner == m_sFirst && point.m_fProgress <= 0)
			newOwner = "";
		else if (point.m_sOwner == m_sSecond && point.m_fProgress >= 0)
			newOwner = "";

		int shown = Math.Round(point.m_fProgress);
		int shownBefore = Math.Round(before);
		if (shown != shownBefore && (shown % 25 == 0))
			Print(string.Format("CLW: points %1 progress=%2 owner='%3' (%4=%5 %6=%7)", point.m_sId, shown, point.m_sOwner, m_sFirst, point.m_iCountFirst, m_sSecond, point.m_iCountSecond), LogLevel.NORMAL);

		if (newOwner != point.m_sOwner)
		{
			string oldOwner = point.m_sOwner;
			point.m_sOwner = newOwner;
			string verb = "captured by " + newOwner;
			if (newOwner.IsEmpty())
				verb = "neutralised (was " + oldOwner + ")";
			CLW_Debug.Say(string.Format("point %1 %2 %3", point.m_sId, point.m_sName, verb));
			m_OnOwnerChanged.Invoke(point.m_sId, oldOwner, newOwner);
		}
	}

	// ---- map markers (public: all factions see them) ----

	protected void SyncMarkers()
	{
		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		if (!manager)
			return;

		int flags = AllFactionFlags();
		foreach (CLW_ControlPoint point : m_aPoints)
		{
			if (point.m_Marker && point.m_sMarkerOwner == point.m_sOwner)
				continue;

			if (point.m_Marker)
				manager.RemoveStaticMarker(point.m_Marker);

			point.m_Marker = BuildMarker(manager, flags, point.m_sOwner, point.m_vPos, point.m_sId + " " + point.m_sName, EMilitarySymbolIcon.RELAY);
			if (point.m_Marker)
			{
				point.m_sMarkerOwner = point.m_sOwner;
				Print(string.Format("CLW: points marker %1 inserted owner='%2'", point.m_sId, point.m_sOwner), LogLevel.NORMAL);
			}
		}

		CLW_Bases bases = CLW_GameMode.GetBases();
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (!bases || !scenario)
			return;

		foreach (CLW_FactionSetup setup : scenario.m_aFactions)
		{
			if (m_aBaseMarkerKeys.Contains(setup.m_sKey))
				continue;

			vector basePos = bases.GetBasePos(setup.m_sKey);
			if (basePos == vector.Zero)
				continue;

			string label = setup.m_sBaseLabel;
			if (label.IsEmpty())
				label = setup.m_sKey + " HQ";
			SCR_MapMarkerBase marker = BuildMarker(manager, flags, setup.m_sKey, basePos, label, EMilitarySymbolIcon.MOBILEHQ);
			if (marker)
			{
				m_aBaseMarkerKeys.Insert(setup.m_sKey);
				m_aBaseMarkers.Insert(marker);
				Print(string.Format("CLW: points base marker %1 '%2' at %3", setup.m_sKey, label, basePos), LogLevel.NORMAL);
			}
		}
	}

	protected SCR_MapMarkerBase BuildMarker(SCR_MapMarkerManagerComponent manager, int factionFlags, string ownerKey, vector pos, string text, EMilitarySymbolIcon icon)
	{
		EMilitarySymbolIdentity identity = CLW_ScenarioConfig.GetIdentity(ownerKey);	// neutral -> UNKNOWN
		SCR_MapMarkerBase marker = manager.PrepareMilitaryMarker(identity, EMilitarySymbolDimension.INSTALLATION, icon);
		if (!marker)
			return null;

		marker.SetWorldPos(pos[0], pos[2]);
		marker.SetMarkerFactionFlags(factionFlags);
		marker.SetCustomText(text);
		manager.InsertStaticMarker(marker, false, true);
		return marker;
	}

	// Bit mask with both scenario factions: visible to everyone playing (public markers). [verify] 0 may already mean "all"
	protected int AllFactionFlags()
	{
		FactionManager factionManager = GetGame().GetFactionManager();
		if (!factionManager)
			return 0;

		int flags;
		Faction first = factionManager.GetFactionByKey(m_sFirst);
		Faction second = factionManager.GetFactionByKey(m_sSecond);
		if (first)
			flags |= 1 << factionManager.GetFactionIndex(first);
		if (second)
			flags |= 1 << factionManager.GetFactionIndex(second);
		return flags;
	}
}
