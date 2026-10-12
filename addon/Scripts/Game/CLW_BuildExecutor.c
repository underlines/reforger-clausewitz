// Build executor (M3 phase F, design section 4): turns a FOB in state BUILDING into a standing FOB.
//   instant   the FOB stands the moment the builder is on site
//   timed     progress 0..100 % over the mode parameter fobBuildSec while the builder is within CLW_FobManager.ARRIVE_M of the site;
//             paused (and logged) while it is away; a log line every 25 %
// "Standing" = spawn point of the side (respawn at ticket cost like the base: the kills already cost tickets) + services tier 1
// (command post, supply cache, arsenal) through CLW_Services.SpawnServices. Tier 2 (vehicle depot, medical) is Upgrade(). Remove() undoes
// all of it when the FOB is lost. The compositions come from the faction data (CLW_FactionProfile.m_aServices, kin fallback), the spawn
// point prefab from the scenario faction setup: nothing is named here. Server only. Log prefix: "CLW: fob".
class CLW_BuildExecutor
{
	static const float STEP_PCT = 25;

	// One build step of a BUILDING fob. dt = seconds since the last step, present = the builder is on site (abstract builds: always).
	static void Step(CLW_Fob fob, float dt, bool present)
	{
		if (fob.m_eState != CLW_EFobState.BUILDING)
			return;

		if (CLW_FobManager.IsInstant())
		{
			Print(string.Format("CLW: fob %1 build 100 pct (instant)", fob.m_sId), LogLevel.NORMAL);
			Complete(fob);
			return;
		}

		float now = CLW_Orders.Now();
		if (!present)
		{
			if (now - fob.m_fLastPauseLogAt >= CLW_FobManager.PAUSE_LOG_SEC)
			{
				fob.m_fLastPauseLogAt = now;
				Print(string.Format("CLW: fob %1 build paused at %2 pct: the builder is away from the site", fob.m_sId, Math.Round(fob.m_fBuild)), LogLevel.NORMAL);
			}

			return;
		}

		fob.m_fBuild = Math.Min(100, fob.m_fBuild + dt / CLW_FobManager.GetBuildSec() * 100);
		int step = Math.Round(Math.Floor(fob.m_fBuild / STEP_PCT) * STEP_PCT);
		if (step > fob.m_iLoggedStep && step < 100)
		{
			fob.m_iLoggedStep = step;
			Print(string.Format("CLW: fob %1 build %2 pct", fob.m_sId, step), LogLevel.NORMAL);
		}

		if (fob.m_fBuild >= 100)
		{
			Print(string.Format("CLW: fob %1 build 100 pct", fob.m_sId), LogLevel.NORMAL);
			Complete(fob);
		}
	}

	// The FOB stands: spawn point + tier 1 services, state ACTIVE, announcement, built event.
	static void Complete(CLW_Fob fob)
	{
		fob.m_fBuild = 100;
		fob.m_iLoggedStep = 100;

		CLW_Bases bases = CLW_GameMode.GetBases();
		if (bases)
			fob.m_SpawnPoint = bases.SpawnFobPoint(fob.m_sSide, fob.m_vPos);

		if (!fob.m_SpawnPoint)
			Print(string.Format("CLW: fob %1 WARNING no spawn point (the side has no spawn point prefab?): players cannot respawn here", fob.m_sId), LogLevel.WARNING);

		int placed;
		if (CLW_Capabilities.Has("services"))
		{
			array<string> kinds = {};
			kinds.Insert(CLW_ServiceKind.COMMAND_POST);
			kinds.Insert(CLW_ServiceKind.SUPPLY);
			kinds.Insert(CLW_ServiceKind.ARSENAL);
			placed = CLW_Services.SpawnServices(fob.m_sSide, fob.m_sId, fob.m_vPos, kinds);
		}

		fob.m_iTier = 1;
		fob.m_bHadPost = CLW_Services.FindAt(fob.m_sSide, fob.m_sId, CLW_ServiceKind.COMMAND_POST) != null;
		CLW_FobManager.SetState(fob, CLW_EFobState.ACTIVE, string.Format("established: spawn point=%1, %2 services (tier 1), command post=%3", fob.m_SpawnPoint != null, placed, fob.m_bHadPost));
		CLW_Debug.Announce(string.Format("%1 established FOB %2 near %3 (grid %4)", fob.m_sSide, fob.m_sId, fob.m_sName, CLW_Places.Grid(fob.m_vPos)));
		CLW_FobManager.NotifyBuilt(fob);
	}

	// Tier 2: vehicle depot and medical. The cost is paid by the caller.
	static void Upgrade(CLW_Fob fob)
	{
		int placed;
		if (CLW_Capabilities.Has("services"))
		{
			array<string> kinds = {};
			kinds.Insert(CLW_ServiceKind.VEHICLE_DEPOT);
			kinds.Insert(CLW_ServiceKind.MEDICAL);
			placed = CLW_Services.SpawnServices(fob.m_sSide, fob.m_sId, fob.m_vPos, kinds);
		}

		fob.m_iTier = 2;
		Print(string.Format("CLW: fob %1 upgraded to tier 2: %2 more services (vehicle depot, medical)", fob.m_sId, placed), LogLevel.NORMAL);
		CLW_Debug.Announce(string.Format("%1 upgraded FOB %2 (depot, medical)", fob.m_sSide, fob.m_sId));
	}

	// The command post of the FOB is gone (deleted) or its main building is destroyed. [verify] the first damageable entity of the
	// composition is its main building.
	static bool PostDestroyed(CLW_Fob fob)
	{
		IEntity post = CLW_Services.FindAt(fob.m_sSide, fob.m_sId, CLW_ServiceKind.COMMAND_POST);
		if (!post)
			return true;

		array<IEntity> tree = {};
		CLW_Services.CollectHierarchy(post, tree);
		foreach (IEntity part : tree)
		{
			DamageManagerComponent damage = DamageManagerComponent.Cast(part.FindComponent(DamageManagerComponent));
			if (damage)
				return damage.GetState() == EDamageState.DESTROYED;
		}

		return false;
	}

	// The FOB is lost: spawn point and services (with their supply link) removed.
	static void Remove(CLW_Fob fob)
	{
		CLW_Bases bases = CLW_GameMode.GetBases();
		if (bases && fob.m_SpawnPoint)
			bases.RemoveFobPoint(fob.m_SpawnPoint);

		fob.m_SpawnPoint = null;
		int removed = CLW_Services.RemoveBase(fob.m_sSide, fob.m_sId);
		Print(string.Format("CLW: fob %1 removed: spawn point gone, %2 services deleted", fob.m_sId, removed), LogLevel.NORMAL);
	}
}
