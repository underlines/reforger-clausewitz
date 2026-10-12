enum CLW_EVehicleState
{
	MOUNTED_IDLE,
	DRIVING,
	DISMOUNTING,
	DISMOUNTED,
	REMOUNTING
}

// What the crew does when the drive waypoint is reached.
enum CLW_EArrival
{
	IDLE,			// just stop
	HOLD,			// Defend where it stopped (RECON, FALL_BACK)
	DISMOUNT		// begin dismounting the cargo group (ATTACK, DEFEND with cargo)
}

// One commandable vehicle unit (spec section 5): vehicle + crew group [+ embarked cargo group], addressed by the crew
// group's registry id. The state machine runs from CLW_VehicleManager.Tick (every 2 s). Server only.
// Public API for the commanders: Order() and the getters below.
class CLW_VehicleUnit
{
	protected string m_sId;
	protected string m_sFaction;
	protected string m_sCargoId;
	protected ref CLW_ForceEntry m_Entry;
	protected IEntity m_Vehicle;
	protected SCR_AIGroup m_Crew;
	protected SCR_AIGroup m_Cargo;
	protected CLW_VehicleManager m_Mgr;				// weak
	protected CLW_EVehicleState m_eState = CLW_EVehicleState.MOUNTED_IDLE;
	protected CLW_EArrival m_eArrival = CLW_EArrival.IDLE;
	protected bool m_bHasOrder;
	protected CLW_ECommand m_eCommand;
	protected vector m_vTarget;
	protected string m_sReason;
	protected vector m_vApproach;						// unit direction (xz) of the approach to the target
	protected vector m_vDriveTarget;					// where the crew is currently driving
	protected bool m_bDriveForced;
	protected float m_fStateTime;
	protected float m_fSinceIssue;						// seconds since the last waypoint was issued
	protected float m_fSinceReissue;
	protected float m_fSampleAcc;
	protected float m_fNoProgress;
	protected ref array<vector> m_aSamples = {};
	protected bool m_bGiveUp;							// stuck: cargo continues on foot
	protected bool m_bForcedOut;
	protected bool m_bDissolved;
	protected bool m_bPending;							// order received while remounting
	protected CLW_ECommand m_ePendCommand;
	protected vector m_vPendTarget;
	protected string m_sPendReason;
	protected float m_fHealthAtLink;
	protected int m_iTeleports;
	protected float m_fSinceTeleportTry = 100;

	void CLW_VehicleUnit(CLW_VehicleManager manager, string id, string factionKey, CLW_ForceEntry entry, IEntity vehicle, SCR_AIGroup crew, SCR_AIGroup cargo, string cargoId)
	{
		m_Mgr = manager;
		m_sId = id;
		m_sFaction = factionKey;
		m_Entry = entry;
		m_Vehicle = vehicle;
		m_Crew = crew;
		m_Cargo = cargo;
		m_sCargoId = cargoId;
		m_fHealthAtLink = GetVehicleHealth();
	}

	// ---- helpers (static, shared with the spawner) ----

	static bool IsAlive(IEntity entity)
	{
		if (!entity)
			return false;

		CharacterControllerComponent controller = CharacterControllerComponent.Cast(entity.FindComponent(CharacterControllerComponent));
		if (controller && controller.GetLifeState() == ECharacterLifeState.DEAD)
			return false;

		return true;
	}

	// True when the character sits in a compartment of this vehicle.
	static bool IsInVehicle(IEntity person, IEntity vehicle)
	{
		if (!person || !vehicle)
			return false;

		SCR_CompartmentAccessComponent access = SCR_CompartmentAccessComponent.Cast(person.FindComponent(SCR_CompartmentAccessComponent));
		if (!access || !access.IsInCompartment())
			return false;

		IEntity inside = access.GetVehicle();
		if (!inside)
			return false;

		return inside == vehicle || inside.GetRootParent() == vehicle;
	}

	// Living members of the group that sit in the vehicle.
	static int CountInside(SCR_AIGroup group, IEntity vehicle)
	{
		if (!group || !vehicle)
			return 0;

		int inside;
		array<IEntity> members = CLW_SquadInfo.Members(group);
		foreach (IEntity member : members)
		{
			if (IsAlive(member) && IsInVehicle(member, vehicle))
				inside++;
		}

		return inside;
	}

	// ---- getters (public API) ----

	string GetId()
	{
		return m_sId;
	}

	string GetFaction()
	{
		return m_sFaction;
	}

	string GetKind()
	{
		return m_Entry.m_sKind;
	}

	string GetDisplayType()
	{
		return m_Entry.m_sName;
	}

	// Vehicle name from the force entry (data), e.g. "LAV-25".
	string GetVehicleTypeName()
	{
		return m_Entry.GetVehicleName();
	}

	string GetTypeId()
	{
		return m_Entry.m_sType;
	}

	IEntity GetVehicle()
	{
		return m_Vehicle;
	}

	SCR_AIGroup GetCrewGroup()
	{
		return m_Crew;
	}

	SCR_AIGroup GetCargoGroup()
	{
		return m_Cargo;
	}

	string GetCargoId()
	{
		return m_sCargoId;
	}

	bool HasCargoGroup()
	{
		return m_Cargo != null;
	}

	bool IsDissolved()
	{
		return m_bDissolved;
	}

	// 0..1, 0 when the vehicle is gone.
	float GetVehicleHealth()
	{
		if (!m_Vehicle)
			return 0;

		DamageManagerComponent damage = DamageManagerComponent.Cast(m_Vehicle.FindComponent(DamageManagerComponent));
		if (!damage)
			return 1;

		return damage.GetHealthScaled();
	}

	// Everybody alive (crew and cargo) sits in the vehicle.
	bool IsMounted()
	{
		if (!m_Vehicle || !m_Crew)
			return false;

		int crewAlive = CLW_SquadInfo.AliveCount(m_Crew);
		if (crewAlive <= 0 || CountInside(m_Crew, m_Vehicle) < crewAlive)
			return false;

		if (m_Cargo)
		{
			int cargoAlive = CLW_SquadInfo.AliveCount(m_Cargo);
			if (cargoAlive > 0 && CountInside(m_Cargo, m_Vehicle) < cargoAlive)
				return false;
		}

		return true;
	}

	// Driving, dismounting or remounting: the unit is going somewhere / doing something (activity management).
	bool IsActiveState()
	{
		return m_eState == CLW_EVehicleState.DRIVING || m_eState == CLW_EVehicleState.DISMOUNTING || m_eState == CLW_EVehicleState.REMOUNTING;
	}

	string GetStateName()
	{
		return StateName(m_eState);
	}

	static string StateName(CLW_EVehicleState state)
	{
		switch (state)
		{
			case CLW_EVehicleState.MOUNTED_IDLE: return "MOUNTED_IDLE";
			case CLW_EVehicleState.DRIVING: return "DRIVING";
			case CLW_EVehicleState.DISMOUNTING: return "DISMOUNTING";
			case CLW_EVehicleState.DISMOUNTED: return "DISMOUNTED";
			case CLW_EVehicleState.REMOUNTING: return "REMOUNTING";
		}

		return "?";
	}

	int GetCrewAlive()
	{
		if (!m_Crew)
			return 0;

		return CLW_SquadInfo.AliveCount(m_Crew);
	}

	int GetCargoAlive()
	{
		if (!m_Cargo)
			return 0;

		return CLW_SquadInfo.AliveCount(m_Cargo);
	}

	vector GetPosition()
	{
		if (m_Vehicle)
			return m_Vehicle.GetOrigin();

		if (m_Crew)
		{
			IEntity leader = CLW_SquadInfo.LeaderEntity(m_Crew);
			if (leader)
				return leader.GetOrigin();
		}

		return vector.Zero;
	}

	bool HasOrder()
	{
		return m_bHasOrder;
	}

	string GetCommandName()
	{
		if (!m_bHasOrder)
			return "";

		return CLW_Command.ToName(m_eCommand);
	}

	vector GetOrderTarget()
	{
		return m_vTarget;
	}

	string GetOrderReason()
	{
		return m_sReason;
	}

	// ---- orders ----

	// Command this unit. Returns false when the unit is dissolved / unusable. Vehicle semantics: spec sections 5 and 6.
	bool Order(CLW_ECommand command, vector target, string reason)
	{
		if (m_bDissolved || !m_Vehicle || !m_Crew)
			return false;

		target[1] = GetGame().GetWorld().GetSurfaceY(target[0], target[2]);
		Print(string.Format("CLW: vehicle %1 order %2 -> %3 (%4) in state %5", m_sId, CLW_Command.ToName(command), target, reason, GetStateName()), LogLevel.NORMAL);

		if (m_eState == CLW_EVehicleState.REMOUNTING)
		{
			m_bPending = true;
			m_ePendCommand = command;
			m_vPendTarget = target;
			m_sPendReason = reason;
			return true;
		}

		SetOrder(command, target, reason);
		if (HasCargoAlive() && (m_eState == CLW_EVehicleState.DISMOUNTED || m_eState == CLW_EVehicleState.DISMOUNTING))
		{
			IEntity cargoLeader = CLW_SquadInfo.LeaderEntity(m_Cargo);
			vector cargoPos = GetPosition();
			if (cargoLeader)
				cargoPos = cargoLeader.GetOrigin();

			if (vector.DistanceXZ(cargoPos, target) > CLW_WarConfig.REMOUNT_MIN_DIST_M)
			{
				StartRemount(command, target, reason);
				return true;
			}

			if (m_eState == CLW_EVehicleState.DISMOUNTING)
				return true;	// Deploy() uses the new order

			OrderDismounted();
			return true;
		}

		PlanMounted();
		return true;
	}

	protected void SetOrder(CLW_ECommand command, vector target, string reason)
	{
		m_bHasOrder = true;
		m_eCommand = command;
		m_vTarget = target;
		m_sReason = reason;
		m_bGiveUp = false;
		vector from = GetPosition();
		vector flat = target - from;
		flat[1] = 0;
		if (flat.Length() > 1)
			m_vApproach = flat.Normalized();
		else
			m_vApproach = Vector(0, 0, 1);
	}

	protected bool HasCargoAlive()
	{
		return m_Cargo && CLW_SquadInfo.AliveCount(m_Cargo) > 0;
	}

	protected bool IsTransport()
	{
		return m_Entry.m_sKind == CLW_ForceKind.MOTORIZED;
	}

	protected void Enter(CLW_EVehicleState next, string why)
	{
		Print(string.Format("CLW: vehicle %1 %2 -> %3 (%4)", m_sId, StateName(m_eState), StateName(next), why), LogLevel.NORMAL);
		m_eState = next;
		m_fStateTime = 0;
		if (next == CLW_EVehicleState.DISMOUNTING)
			m_bForcedOut = false;
	}

	protected void ResetWatchdog()
	{
		m_aSamples.Clear();
		m_fSampleAcc = 0;
		m_fNoProgress = 0;
		m_fSinceReissue = 0;
		m_fSinceIssue = 0;
	}

	// Drive the crew to pos (Move, or ForcedMove). The cargo group is left alone.
	protected void IssueDrive(vector pos, bool forced)
	{
		m_vDriveTarget = pos;
		m_bDriveForced = forced;
		CLW_Waypoints.Clear(m_Crew);
		if (forced)
			CLW_Waypoints.AddForcedMove(m_Crew, pos, CLW_WarConfig.ARRIVE_RADIUS_M / 2);
		else
			CLW_Waypoints.AddMove(m_Crew, pos, CLW_WarConfig.ARRIVE_RADIUS_M / 2);
		ResetWatchdog();
	}

	// Order for a unit whose cargo (if any) is mounted: plan by command.
	protected void PlanMounted()
	{
		vector pos = GetPosition();
		float dist = vector.DistanceXZ(pos, m_vTarget);

		if (!HasCargoAlive())
		{
			// light / armor / armored recon, or a mech that lost its cargo: the crew executes the command itself
			CLW_Waypoints.Clear(m_Crew);
			m_vDriveTarget = m_vTarget;
			ResetWatchdog();
			if (m_eCommand == CLW_ECommand.ATTACK)
				CLW_Waypoints.AddAttack(m_Crew, m_vTarget);
			else if (m_eCommand == CLW_ECommand.DEFEND)
				CLW_Waypoints.AddDefend(m_Crew, m_vTarget);
			else if (m_eCommand == CLW_ECommand.FALL_BACK)
			{
				CLW_Waypoints.AddForcedMove(m_Crew, m_vTarget);
				CLW_Waypoints.AddDefend(m_Crew, m_vTarget);
			}
			else if (m_eCommand == CLW_ECommand.RECON)
			{
				CLW_Waypoints.AddMove(m_Crew, m_vTarget);
				CLW_Waypoints.AddDefend(m_Crew, m_vTarget);
			}
			else
				CLW_Waypoints.AddMove(m_Crew, m_vTarget);

			m_eArrival = CLW_EArrival.IDLE;
			Enter(CLW_EVehicleState.DRIVING, CLW_Command.ToName(m_eCommand) + " by the crew alone");
			return;
		}

		switch (m_eCommand)
		{
			case CLW_ECommand.MOVE:
			{
				IssueDrive(m_vTarget, false);
				m_eArrival = CLW_EArrival.IDLE;
				Enter(CLW_EVehicleState.DRIVING, "MOVE, everybody mounted");
				return;
			}
			case CLW_ECommand.RECON:
			{
				IssueDrive(m_vTarget, false);
				m_eArrival = CLW_EArrival.HOLD;
				Enter(CLW_EVehicleState.DRIVING, "RECON, mounted");
				return;
			}
			case CLW_ECommand.FALL_BACK:
			{
				IssueDrive(m_vTarget, true);
				m_eArrival = CLW_EArrival.HOLD;
				Enter(CLW_EVehicleState.DRIVING, "FALL_BACK, forced move");
				return;
			}
		}

		// ATTACK / DEFEND: drive to the dismount point short of the target, then dismount
		float dismountDist = CLW_WarConfig.DISMOUNT_DIST_M;
		if (IsTransport())
			dismountDist = CLW_WarConfig.DISMOUNT_DIST_TRUCK_M;

		m_eArrival = CLW_EArrival.DISMOUNT;
		if (dist <= dismountDist + CLW_WarConfig.ARRIVE_RADIUS_M)
		{
			m_vDriveTarget = pos;
			ResetWatchdog();
			BeginDismount("already within " + Math.Round(dismountDist) + " m of the target");
			return;
		}

		vector dismountPoint = m_vTarget - m_vApproach * dismountDist;
		IssueDrive(dismountPoint, false);
		Enter(CLW_EVehicleState.DRIVING, string.Format("%1, driving to the dismount point %2 m short of the target", CLW_Command.ToName(m_eCommand), Math.Round(dismountDist)));
	}

	// ---- dismount / deploy ----

	protected void BeginDismount(string why)
	{
		Enter(CLW_EVehicleState.DISMOUNTING, why);
		CLW_Waypoints.Clear(m_Crew);	// the vehicle stops here
		CLW_Waypoints.Clear(m_Cargo);
		CLW_Waypoints.AddGetOut(m_Cargo, GetPosition(), false, false, true);
	}

	protected int CargoMounted()
	{
		return CountInside(m_Cargo, m_Vehicle);
	}

	// Kick the cargo that did not get out by itself (teleport out).
	protected void ForceOutCargo()
	{
		m_bForcedOut = true;
		int forced;
		array<IEntity> members = CLW_SquadInfo.Members(m_Cargo);
		foreach (IEntity member : members)
		{
			if (!IsAlive(member) || !IsInVehicle(member, m_Vehicle))
				continue;

			SCR_CompartmentAccessComponent access = SCR_CompartmentAccessComponent.Cast(member.FindComponent(SCR_CompartmentAccessComponent));
			if (access && access.GetOutVehicle(EGetOutType.TELEPORT, -1, ECloseDoorAfterActions.INVALID, false))
				forced++;
		}

		Print(string.Format("CLW: vehicle %1 forced %2 cargo members out (teleport)", m_sId, forced), LogLevel.WARNING);
	}

	// Cargo is out: infantry gets the order, the vehicle takes its supporting position (or the truck leaves).
	protected void Deploy()
	{
		Enter(CLW_EVehicleState.DISMOUNTED, "cargo is out, infantry deploys");
		DeployInfantry();
		DeployVehicle();
		// a few seconds later (engine finished the get-out): free cargo still seated or standing on the vehicle
		GetGame().GetCallqueue().CallLater(UnstuckCargo, 4000, false);
	}

	protected void UnstuckCargo()
	{
		if (!m_Cargo || m_bDissolved || m_eState != CLW_EVehicleState.DISMOUNTED)
			return;

		CLW_Unstuck.FreeMembers(m_Cargo, "after dismount of " + m_sId, false);
	}

	protected void DeployInfantry()
	{
		if (!HasCargoAlive() || !m_bHasOrder)
			return;

		CLW_Waypoints.Clear(m_Cargo);
		CLW_Waypoints.AddForCommand(m_Cargo, m_eCommand, m_vTarget);
		Print(string.Format("CLW: vehicle %1 cargo %2 order %3 on foot at %4", m_sId, m_sCargoId, CLW_Command.ToName(m_eCommand), m_vTarget), LogLevel.NORMAL);
	}

	protected void DeployVehicle()
	{
		if (IsTransport())
		{
			Dissolve("transport drops the infantry and returns to base", true);
			return;
		}

		CLW_Waypoints.Clear(m_Crew);
		vector hold = GetPosition();
		if (m_bGiveUp || !m_bHasOrder)
		{
			CLW_Waypoints.AddDefend(m_Crew, hold);
			m_vDriveTarget = hold;
			return;
		}

		if (m_eCommand == CLW_ECommand.ATTACK)
			hold = m_vTarget - m_vApproach * CLW_WarConfig.OVERWATCH_DIST_M;
		else if (m_eCommand == CLW_ECommand.DEFEND)
			hold = m_vTarget - m_vApproach * CLW_WarConfig.DEFEND_BEHIND_M;
		else
			hold = m_vTarget;

		m_vDriveTarget = hold;
		CLW_Waypoints.AddMove(m_Crew, hold, CLW_WarConfig.ARRIVE_RADIUS_M / 2);
		CLW_Waypoints.AddDefend(m_Crew, hold);
		m_fSinceIssue = 0;
		Print(string.Format("CLW: vehicle %1 vehicle supports from %2", m_sId, hold), LogLevel.NORMAL);
	}

	// A near order while the infantry is already out: re-order infantry and vehicle without remounting.
	protected void OrderDismounted()
	{
		DeployInfantry();
		DeployVehicle();
	}

	// ---- remount ----

	protected void StartRemount(CLW_ECommand command, vector target, string reason)
	{
		m_bPending = true;
		m_ePendCommand = command;
		m_vPendTarget = target;
		m_sPendReason = reason;
		Enter(CLW_EVehicleState.REMOUNTING, "order farther than " + Math.Round(CLW_WarConfig.REMOUNT_MIN_DIST_M) + " m: infantry boards again");
		CLW_Waypoints.Clear(m_Crew);	// the vehicle waits
		CLW_Waypoints.Clear(m_Cargo);
		CLW_Waypoints.AddUsableVehicle(m_Cargo, m_Vehicle);
		CLW_Waypoints.AddGetIn(m_Cargo, m_Vehicle, false, false, true);
	}

	protected void TickRemount()
	{
		if (!HasCargoAlive())
			return;

		int alive = CLW_SquadInfo.AliveCount(m_Cargo);
		if (CargoMounted() >= alive)
		{
			Enter(CLW_EVehicleState.MOUNTED_IDLE, "infantry remounted");
			ExecutePending();
			return;
		}

		if (m_fStateTime >= CLW_WarConfig.REMOUNT_TIMEOUT_SEC)
			Split();
	}

	protected void ExecutePending()
	{
		if (!m_bPending)
			return;

		m_bPending = false;
		SetOrder(m_ePendCommand, m_vPendTarget, m_sPendReason);
		PlanMounted();
	}

	// Remount timed out: the infantry walks (becomes its own unit), the vehicle continues alone.
	protected void Split()
	{
		Print(string.Format("CLW: vehicle %1 remount timed out after %2 s: unit splits, %3 walks", m_sId, Math.Round(m_fStateTime), m_sCargoId), LogLevel.WARNING);
		CLW_SquadRegistry registry = m_Mgr.GetRegistry();
		if (registry && !m_sCargoId.IsEmpty())
			registry.SetHidden(m_sCargoId, false);

		CLW_Waypoints.Clear(m_Cargo);
		if (m_bPending)
			CLW_Waypoints.AddForCommand(m_Cargo, m_ePendCommand, m_vPendTarget);

		m_Cargo = null;
		m_sCargoId = "";
		Enter(CLW_EVehicleState.MOUNTED_IDLE, "split from the infantry");
		ExecutePending();
	}

	// ---- watchdog ----

	protected void Sample()
	{
		m_aSamples.Insert(GetPosition());
		while (m_aSamples.Count() > CLW_WarConfig.STUCK_SAMPLES)
		{
			m_aSamples.RemoveOrdered(0);
		}

		if (m_aSamples.Count() < CLW_WarConfig.STUCK_SAMPLES)
			return;

		float progress = vector.DistanceXZ(m_aSamples[0], m_aSamples[m_aSamples.Count() - 1]);
		if (progress >= CLW_WarConfig.STUCK_MIN_PROGRESS_M)
		{
			m_fNoProgress = 0;
			return;
		}

		bool first = m_fNoProgress <= 0;
		if (first)
			m_fNoProgress = (CLW_WarConfig.STUCK_SAMPLES - 1) * CLW_WarConfig.STUCK_SAMPLE_SEC;
		else
			m_fNoProgress += CLW_WarConfig.STUCK_SAMPLE_SEC;

		Print(string.Format("CLW: vehicle %1 STUCK: %2 m in %3 s (total %4 s without progress) at %5", m_sId, Math.Round(progress), (CLW_WarConfig.STUCK_SAMPLES - 1) * CLW_WarConfig.STUCK_SAMPLE_SEC, Math.Round(m_fNoProgress), GetPosition()), LogLevel.WARNING);
		// escalation: (a) re-issue at 40 s, (b) teleport toward the target at 80 s, (c) despawn / dismount at 160 s
		if (m_fNoProgress >= CLW_WarConfig.STUCK_REMOVE_SEC)
		{
			RemoveStuck();
			return;
		}

		if (m_fNoProgress >= CLW_WarConfig.STUCK_TELEPORT_SEC && m_iTeleports < CLW_WarConfig.STUCK_TELEPORT_MAX && m_fSinceTeleportTry >= 30)
		{
			m_fSinceTeleportTry = 0;
			if (TryTeleport())
				return;
		}

		if (first || m_fSinceReissue >= CLW_WarConfig.STUCK_REISSUE_SEC)
		{
			m_fSinceReissue = 0;
			Print(string.Format("CLW: vehicle %1 re-issuing the drive waypoint to %2", m_sId, m_vDriveTarget), LogLevel.WARNING);
			CLW_Waypoints.Clear(m_Crew);
			if (m_bDriveForced)
				CLW_Waypoints.AddForcedMove(m_Crew, m_vDriveTarget, CLW_WarConfig.ARRIVE_RADIUS_M / 2);
			else
				CLW_Waypoints.AddMove(m_Crew, m_vDriveTarget, CLW_WarConfig.ARRIVE_RADIUS_M / 2);
			m_fSinceIssue = 0;
		}
	}

	// (b) Stuck for STUCK_TELEPORT_SEC: with no human nearby, move the vehicle (crew and cargo ride along) to a free road / ground spot
	// within STUCK_TELEPORT_MAX_M that lies toward its target, then drive on. False when not done.
	protected bool TryTeleport()
	{
		vector pos = GetPosition();
		float human = CLW_Placement.NearestPlayerDistance(pos);
		if (human < CLW_WarConfig.STUCK_TELEPORT_HUMAN_M)
		{
			Print(string.Format("CLW: vehicle %1 stuck, teleport skipped: a human is %2 m away", m_sId, Math.Round(human)), LogLevel.NORMAL);
			return false;
		}

		float health = GetVehicleHealth();
		if (m_fHealthAtLink > 0.5 && health < CLW_WarConfig.STUCK_TELEPORT_MIN_HEALTH)
		{
			Print(string.Format("CLW: vehicle %1 stuck, teleport skipped: too damaged (health %2)", m_sId, health), LogLevel.NORMAL);
			return false;
		}

		vector spot;
		float yaw;
		string via;
		if (!CLW_Placement.FindSpotToward(pos, m_vDriveTarget, CLW_WarConfig.STUCK_TELEPORT_MIN_M, CLW_WarConfig.STUCK_TELEPORT_MAX_M, CLW_WarConfig.SPAWN_VEHICLE_CYLINDER_R, CLW_WarConfig.SPAWN_VEHICLE_CYLINDER_H, spot, yaw, via))
		{
			Print(string.Format("CLW: vehicle %1 stuck at %2, no free spot toward %3 within %4 m", m_sId, pos, m_vDriveTarget, CLW_WarConfig.STUCK_TELEPORT_MAX_M), LogLevel.WARNING);
			return false;
		}

		vector mat[4];
		Math3D.AnglesToMatrix(Vector(yaw, 0, 0), mat);	// [verify] angle order yaw, pitch, roll
		mat[3] = spot + Vector(0, 0.6, 0);
		m_Vehicle.SetTransform(mat);
		m_Vehicle.Update();
		Physics physics = m_Vehicle.GetPhysics();
		if (physics)
		{
			physics.SetVelocity(vector.Zero);
			physics.SetAngularVelocity(vector.Zero);
		}

		m_iTeleports++;
		Print(string.Format("CLW: vehicle %1 TELEPORTED #%2 from %3 to %4 (%5, yaw %6, %7 m toward %8), human %9 m away", m_sId, m_iTeleports, pos, spot, via, Math.Round(yaw), Math.Round(vector.DistanceXZ(pos, spot)), m_vDriveTarget, Math.Round(Math.Min(human, 99999))), LogLevel.WARNING);
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (war)
			war.PushEvent(m_sFaction, m_sId + " was stuck and has been repositioned toward its target");

		IssueDrive(m_vDriveTarget, m_bDriveForced);
		return true;
	}

	// Vehicle share of the purchase price (what a despawn refunds half of).
	protected int VehicleShare()
	{
		if (m_Entry.m_iVehicleCost > 0)
			return Math.Min(m_Entry.m_iVehicleCost, m_Entry.m_iCost);

		int share = m_Entry.m_iCost - m_Entry.m_iCargoMembers * CLW_WarConfig.DISCOVER_SOLDIER_COST;
		return Math.Max(share, Math.Round(m_Entry.m_iCost * 0.4));
	}

	// (c) Still stuck after STUCK_REMOVE_SEC. No human near: despawn the vehicle with the crew (and cargo still inside) and refund
	// half of the vehicle's price. Humans near: everybody gets out, the infantry continues on foot with the order, and the empty
	// vehicle is deleted later once nobody is close to it.
	protected void RemoveStuck()
	{
		vector pos = GetPosition();
		float human = CLW_Placement.NearestPlayerDistance(pos);
		CLW_WarState war = CLW_GameMode.GetWarState();
		CLW_SquadRegistry registry = m_Mgr.GetRegistry();
		if (human > CLW_WarConfig.STUCK_REMOVE_HUMAN_M)
		{
			int refund = Math.Round(VehicleShare() * CLW_WarConfig.STUCK_REMOVE_REFUND);
			Print(string.Format("CLW: vehicle %1 STUCK for %2 s, no human within %3 m: despawn with crew, refund %4", m_sId, Math.Round(m_fNoProgress), CLW_WarConfig.STUCK_REMOVE_HUMAN_M, refund), LogLevel.WARNING);
			m_bDissolved = true;
			// cargo still inside goes with the vehicle; cargo that already walks on stays alive and is released to the commander
			if (m_Cargo)
			{
				array<IEntity> cargoMembers = CLW_SquadInfo.Members(m_Cargo);
				foreach (IEntity member : cargoMembers)
				{
					if (member && IsInVehicle(member, m_Vehicle))
						SCR_EntityHelper.DeleteEntityAndChildren(member);
				}

				if (CLW_SquadInfo.AliveCount(m_Cargo) > 0)
				{
					if (registry && !m_sCargoId.IsEmpty())
						registry.SetHidden(m_sCargoId, false);
					CLW_Waypoints.Clear(m_Cargo);
				}
				else
				{
					CLW_Waypoints.Clear(m_Cargo);
					CLW_ForceSpawner.DeleteGroupAndMembers(m_Cargo);
				}
			}

			CLW_Waypoints.Clear(m_Crew);
			CLW_ForceSpawner.DeleteGroupAndMembers(m_Crew);
			SCR_EntityHelper.DeleteEntityAndChildren(m_Vehicle);
			if (war)
			{
				war.AddSupplies(m_sFaction, refund);
				war.PushEvent(m_sFaction, m_sId + " stuck for good and removed (+" + refund + " supplies)");
			}

			return;
		}

		Print(string.Format("CLW: vehicle %1 STUCK for %2 s, human %3 m away: everybody dismounts, vehicle will be removed when unobserved", m_sId, Math.Round(m_fNoProgress), Math.Round(human)), LogLevel.WARNING);
		EjectAll(m_Crew);
		EjectAll(m_Cargo);
		if (m_bHasOrder)
		{
			// cargo continues on foot; the crew becomes a small infantry group with the same order
			if (HasCargoAlive())
				DeployInfantry();
			if (CLW_SquadInfo.AliveCount(m_Crew) > 0)
			{
				CLW_Waypoints.Clear(m_Crew);
				CLW_Waypoints.AddForCommand(m_Crew, m_eCommand, m_vTarget);
			}
		}
		else
		{
			CLW_Waypoints.Clear(m_Crew);
		}

		IEntity vehicle = m_Vehicle;
		Dissolve("stuck for " + Math.Round(m_fNoProgress) + " s, abandoned (humans near)", false);
		m_Mgr.StartAbandon(m_sId, vehicle);
	}

	protected void GiveUp()
	{
		if (HasCargoAlive() && CargoMounted() > 0)
		{
			m_bGiveUp = true;
			BeginDismount("stuck for " + Math.Round(m_fNoProgress) + " s: infantry continues on foot");
			return;
		}

		CLW_Waypoints.Clear(m_Crew);
		CLW_Waypoints.AddDefend(m_Crew, GetPosition());
		Enter(CLW_EVehicleState.MOUNTED_IDLE, "stuck for " + Math.Round(m_fNoProgress) + " s, no cargo to dismount: holding position");
	}

	// ---- tick ----

	void Tick(float dt)
	{
		if (m_bDissolved)
			return;

		m_fStateTime += dt;
		m_fSinceIssue += dt;
		m_fSinceTeleportTry += dt;
		m_fSinceReissue += dt;

		// unlink rules (spec section 5)
		if (!m_Vehicle)
		{
			Dissolve("vehicle destroyed or deleted", false);
			return;
		}

		DamageManagerComponent damage = DamageManagerComponent.Cast(m_Vehicle.FindComponent(DamageManagerComponent));
		if (damage)
		{
			if (damage.GetState() == EDamageState.DESTROYED)
			{
				Dissolve("vehicle destroyed", false);
				return;
			}

			if (IsImmobileHealth(damage.GetHealthScaled()))
			{
				Dissolve("vehicle immobile (health " + damage.GetHealthScaled() + ")", false);
				return;
			}
		}

		if (!m_Crew || CLW_SquadInfo.AliveCount(m_Crew) <= 0)
		{
			CLW_Waypoints.Clear(m_Crew);	// an empty group with a move waypoint makes vanilla SCR_AIProcessFailedMovementResult throw on its null leader
			Dissolve("crew lost, vehicle abandoned", false);
			return;
		}

		if (m_Cargo && CLW_SquadInfo.AliveCount(m_Cargo) <= 0)
		{
			Print(string.Format("CLW: vehicle %1 cargo group %2 wiped out, vehicle continues alone", m_sId, m_sCargoId), LogLevel.WARNING);
			CLW_Waypoints.Clear(m_Cargo);
			m_Cargo = null;
			m_sCargoId = "";
			m_bPending = false;
			CLW_EVehicleState previous = m_eState;
			bool wasDismountPlan = previous == CLW_EVehicleState.DRIVING && m_eArrival == CLW_EArrival.DISMOUNT;
			if (previous == CLW_EVehicleState.DISMOUNTING || previous == CLW_EVehicleState.DISMOUNTED || previous == CLW_EVehicleState.REMOUNTING || wasDismountPlan)
			{
				Enter(CLW_EVehicleState.MOUNTED_IDLE, "cargo wiped out");
				if (m_bHasOrder)
					PlanMounted();
			}
		}

		switch (m_eState)
		{
			case CLW_EVehicleState.DRIVING: TickDriving(dt); break;
			case CLW_EVehicleState.DISMOUNTING: TickDismounting(); break;
			case CLW_EVehicleState.REMOUNTING: TickRemount(); break;
			case CLW_EVehicleState.MOUNTED_IDLE: TickMountedIdle(); break;
		}
	}

	// Vanilla AI dismounts passengers at contact (trucks always, armed vehicles when the threat is close): the cargo is on foot
	// although we did not order it. Treat that as DISMOUNTED and give the infantry the attack order directly.
	protected bool CheckUnexpectedDismount()
	{
		if (!HasCargoAlive() || m_fStateTime < 10 || m_fSinceIssue < CLW_WarConfig.ORDER_GRACE_SEC)
			return false;

		if (CargoMounted() > 0)
			return false;

		Enter(CLW_EVehicleState.DISMOUNTED, "cargo left the vehicle by itself (dismount at contact)");
		DeployInfantry();
		DeployVehicle();
		return true;
	}

	protected void TickMountedIdle()
	{
		CheckUnexpectedDismount();
	}

	protected void TickDriving(float dt)
	{
		if (CheckUnexpectedDismount())
			return;

		// arrival
		bool arrived;
		if (m_fSinceIssue >= CLW_WarConfig.ORDER_GRACE_SEC)
		{
			if (vector.DistanceXZ(GetPosition(), m_vDriveTarget) <= CLW_WarConfig.ARRIVE_RADIUS_M)
				arrived = true;
			else if (!m_Crew.GetCurrentWaypoint())
				arrived = true;
		}

		if (arrived)
		{
			m_aSamples.Clear();
			if (m_eArrival == CLW_EArrival.DISMOUNT && HasCargoAlive())
			{
				BeginDismount("arrived at the dismount point");
				return;
			}

			if (m_eArrival == CLW_EArrival.HOLD)
			{
				CLW_Waypoints.Clear(m_Crew);
				CLW_Waypoints.AddDefend(m_Crew, GetPosition());
				Enter(CLW_EVehicleState.MOUNTED_IDLE, "arrived, holding");
				return;
			}

			Enter(CLW_EVehicleState.MOUNTED_IDLE, "arrived");
			return;
		}

		m_fSampleAcc += dt;
		if (m_fSampleAcc >= CLW_WarConfig.STUCK_SAMPLE_SEC)
		{
			m_fSampleAcc = 0;
			Sample();
		}
	}

	protected void TickDismounting()
	{
		int mounted = CargoMounted();
		if (mounted <= 0)
		{
			Deploy();
			return;
		}

		if (m_fStateTime >= CLW_WarConfig.DISMOUNT_FORCE_SEC && !m_bForcedOut)
			ForceOutCargo();

		if (m_fStateTime >= CLW_WarConfig.DISMOUNT_FORCE_SEC + 10)
		{
			Print(string.Format("CLW: vehicle %1 dismount incomplete (%2 still inside), deploying anyway", m_sId, mounted), LogLevel.WARNING);
			Deploy();
		}
	}

	// Low health counts as immobile only for vehicles that reported healthy values at link time (a vehicle whose damage
	// manager never reports health must not be dissolved by a bogus 0).
	protected bool IsImmobileHealth(float health)
	{
		return m_fHealthAtLink > 0.5 && health <= CLW_WarConfig.VEHICLE_IMMOBILE_HEALTH;
	}

	// ---- dissolving ----

	// End this unit: the survivors become plain units. returning = an unarmed transport that drives back to base to be removed.
	void Dissolve(string why, bool returning = false)
	{
		if (m_bDissolved)
			return;

		m_bDissolved = true;
		Print(string.Format("CLW: vehicle %1 UNLINK (%2) state=%3 crew=%4 cargo=%5", m_sId, why, GetStateName(), GetCrewAlive(), GetCargoAlive()), LogLevel.WARNING);
		CLW_SquadRegistry registry = m_Mgr.GetRegistry();

		// a vehicle that cannot be used any more: people get out
		bool vehicleOk = m_Vehicle && !returning;
		if (m_Vehicle && !returning)
		{
			DamageManagerComponent damage = DamageManagerComponent.Cast(m_Vehicle.FindComponent(DamageManagerComponent));
			if (damage && (damage.GetState() == EDamageState.DESTROYED || IsImmobileHealth(damage.GetHealthScaled())))
				vehicleOk = false;
			else if (GetCrewAlive() <= 0)
				vehicleOk = false;
		}

		if (m_Vehicle && !returning && !vehicleOk)
		{
			EjectAll(m_Crew);
			EjectAll(m_Cargo);
		}

		if (m_Cargo && registry && !m_sCargoId.IsEmpty())
			registry.SetHidden(m_sCargoId, false);

		if (returning)
		{
			if (registry)
				registry.SetHidden(m_sId, true);
			m_Mgr.StartReturn(m_sId, m_sFaction, m_Vehicle, m_Crew);
		}
		else if (m_Crew && CLW_SquadInfo.AliveCount(m_Crew) > 0 && registry)
		{
			registry.SetKind(m_sId, CLW_ForceKind.INFANTRY, "Crew of " + m_Entry.GetVehicleName());
		}

		CLW_WarState war = CLW_GameMode.GetWarState();
		if (war && !returning)
			war.PushEvent(m_sFaction, m_sId + " unit dissolved: " + why);
	}

	protected void EjectAll(SCR_AIGroup group)
	{
		if (!group || !m_Vehicle)
			return;

		array<IEntity> members = CLW_SquadInfo.Members(group);
		foreach (IEntity member : members)
		{
			if (!IsAlive(member) || !IsInVehicle(member, m_Vehicle))
				continue;

			SCR_CompartmentAccessComponent access = SCR_CompartmentAccessComponent.Cast(member.FindComponent(SCR_CompartmentAccessComponent));
			if (access)
				access.GetOutVehicle(EGetOutType.TELEPORT, -1, ECloseDoorAfterActions.INVALID, false);
		}
	}
}

// A transport that dropped its infantry and drives back to base; deleted on arrival (partial refund).
class CLW_ReturnJob
{
	string m_sId;
	string m_sFaction;
	IEntity m_Vehicle;
	SCR_AIGroup m_Crew;
	vector m_vBase;
	float m_fAge;
	float m_fSinceIssue;
	bool m_bDone;
}

// An empty stuck vehicle waiting to be deleted once no human is close to it.
class CLW_AbandonJob
{
	string m_sId;
	IEntity m_Vehicle;
	float m_fAge;
	float m_fSinceCheck;
	bool m_bDone;
}

// All vehicle units of the war. Ticked every CLW_WarConfig.VEHICLE_TICK_MS.
class CLW_VehicleManager
{
	protected ref array<ref CLW_VehicleUnit> m_aUnits = {};
	protected ref array<ref CLW_ReturnJob> m_aReturns = {};
	protected ref array<ref CLW_AbandonJob> m_aAbandoned = {};
	protected CLW_SquadRegistry m_Registry;	// weak: owned by the game mode

	void Init(CLW_SquadRegistry registry)
	{
		m_Registry = registry;
		if (!Replication.IsServer())
			return;

		GetGame().GetCallqueue().CallLater(Tick, CLW_WarConfig.VEHICLE_TICK_MS, true);
		Print("CLW: vehicle manager ready", LogLevel.NORMAL);
	}

	CLW_SquadRegistry GetRegistry()
	{
		return m_Registry;
	}

	// Called by the spawner when a crewed vehicle (with or without embarked infantry) is complete.
	CLW_VehicleUnit Link(string id, string factionKey, CLW_ForceEntry entry, IEntity vehicle, SCR_AIGroup crew, SCR_AIGroup cargo, string cargoId)
	{
		CLW_VehicleUnit unit = new CLW_VehicleUnit(this, id, factionKey, entry, vehicle, crew, cargo, cargoId);
		m_aUnits.Insert(unit);
		if (m_Registry && cargo && !cargoId.IsEmpty())
			m_Registry.SetHidden(cargoId, true);

		Print(string.Format("CLW: vehicle %1 LINKED faction=%2 type=%3 kind=%4 vehicle='%5' crew=%6 cargo=%7 (%8)", id, factionKey, entry.m_sType, entry.m_sKind, entry.GetVehicleName(), CLW_SquadInfo.AliveCount(crew), cargoId, unit.GetStateName()), LogLevel.NORMAL);
		return unit;
	}

	CLW_VehicleUnit Find(string id)
	{
		foreach (CLW_VehicleUnit unit : m_aUnits)
		{
			if (unit.GetId() == id && !unit.IsDissolved())
				return unit;
		}

		return null;
	}

	// True while the group is the hidden cargo of a live vehicle unit.
	bool IsLinkedCargo(string groupId)
	{
		if (groupId.IsEmpty())
			return false;

		foreach (CLW_VehicleUnit unit : m_aUnits)
		{
			if (!unit.IsDissolved() && unit.HasCargoGroup() && unit.GetCargoId() == groupId)
				return true;
		}

		return false;
	}

	// The vehicle unit that carries this group (cargo id), else null.
	CLW_VehicleUnit FindByCargo(string groupId)
	{
		foreach (CLW_VehicleUnit unit : m_aUnits)
		{
			if (!unit.IsDissolved() && unit.HasCargoGroup() && unit.GetCargoId() == groupId)
				return unit;
		}

		return null;
	}

	void GetUnits(string factionKey, notnull array<CLW_VehicleUnit> units)
	{
		foreach (CLW_VehicleUnit unit : m_aUnits)
		{
			if (!unit.IsDissolved() && unit.GetFaction() == factionKey)
				units.Insert(unit);
		}
	}

	// Vehicles of the faction: live vehicle units plus transports still driving home.
	int CountVehicles(string factionKey)
	{
		int total;
		foreach (CLW_VehicleUnit unit : m_aUnits)
		{
			if (!unit.IsDissolved() && unit.GetFaction() == factionKey)
				total++;
		}

		foreach (CLW_ReturnJob job : m_aReturns)
		{
			if (job.m_sFaction == factionKey && !job.m_bDone)
				total++;
		}

		return total;
	}

	void Tick()
	{
		float dt = CLW_WarConfig.VEHICLE_TICK_MS / 1000.0;
		for (int i = m_aUnits.Count() - 1; i >= 0; i--)
		{
			CLW_VehicleUnit unit = m_aUnits[i];
			unit.Tick(dt);
			if (unit.IsDissolved())
				m_aUnits.Remove(i);
		}

		for (int j = m_aReturns.Count() - 1; j >= 0; j--)
		{
			TickReturn(m_aReturns[j], dt);
			if (m_aReturns[j].m_bDone)
				m_aReturns.Remove(j);
		}

		for (int k = m_aAbandoned.Count() - 1; k >= 0; k--)
		{
			TickAbandoned(m_aAbandoned[k], dt);
			if (m_aAbandoned[k].m_bDone)
				m_aAbandoned.Remove(k);
		}
	}

	// ---- abandoned (stuck, emptied) vehicles ----

	void StartAbandon(string id, IEntity vehicle)
	{
		if (!vehicle)
			return;

		CLW_AbandonJob job = new CLW_AbandonJob();
		job.m_sId = id;
		job.m_Vehicle = vehicle;
		m_aAbandoned.Insert(job);
		Print(string.Format("CLW: vehicle %1 abandoned at %2, deleted once no human is within %3 m (gives up after %4 s)", id, vehicle.GetOrigin(), CLW_WarConfig.ABANDON_DELETE_HUMAN_M, CLW_WarConfig.ABANDON_GIVE_UP_SEC), LogLevel.NORMAL);
	}

	protected bool HasOccupant(IEntity vehicle)
	{
		BaseCompartmentManagerComponent compartments = BaseCompartmentManagerComponent.Cast(vehicle.FindComponent(BaseCompartmentManagerComponent));
		if (!compartments)
			return false;

		array<BaseCompartmentSlot> slots = {};
		compartments.GetCompartments(slots);
		foreach (BaseCompartmentSlot slot : slots)
		{
			IEntity occupant;
			if (slot)
				occupant = slot.GetOccupant();
			if (occupant && CLW_VehicleUnit.IsAlive(occupant))
				return true;
		}

		return false;
	}

	protected void TickAbandoned(CLW_AbandonJob job, float dt)
	{
		job.m_fAge += dt;
		job.m_fSinceCheck += dt;
		if (!job.m_Vehicle)
		{
			job.m_bDone = true;
			return;
		}

		if (job.m_fSinceCheck < CLW_WarConfig.ABANDON_CHECK_SEC)
			return;

		job.m_fSinceCheck = 0;
		if (job.m_fAge >= CLW_WarConfig.ABANDON_GIVE_UP_SEC)
		{
			Print(string.Format("CLW: vehicle %1 abandoned vehicle left in place after %2 s (observed)", job.m_sId, Math.Round(job.m_fAge)), LogLevel.NORMAL);
			job.m_bDone = true;
			return;
		}

		if (CLW_Placement.NearestPlayerDistance(job.m_Vehicle.GetOrigin()) <= CLW_WarConfig.ABANDON_DELETE_HUMAN_M || HasOccupant(job.m_Vehicle))
			return;

		Print(string.Format("CLW: vehicle %1 abandoned vehicle deleted (unobserved, empty)", job.m_sId), LogLevel.NORMAL);
		SCR_EntityHelper.DeleteEntityAndChildren(job.m_Vehicle);
		job.m_bDone = true;
	}

	// ---- returning transports ----

	void StartReturn(string id, string factionKey, IEntity vehicle, SCR_AIGroup crew)
	{
		CLW_Bases bases = CLW_GameMode.GetBases();
		vector basePos;
		if (bases)
			basePos = bases.GetBasePos(factionKey);

		if (basePos == vector.Zero || !vehicle || !crew)
		{
			Print(string.Format("CLW: vehicle %1 cannot return to base (base=%2), the transport stays where it is", id, basePos), LogLevel.WARNING);
			return;
		}

		CLW_ReturnJob job = new CLW_ReturnJob();
		job.m_sId = id;
		job.m_sFaction = factionKey;
		job.m_Vehicle = vehicle;
		job.m_Crew = crew;
		job.m_vBase = basePos;
		m_aReturns.Insert(job);
		CLW_Waypoints.Clear(crew);
		CLW_Waypoints.AddMove(crew, basePos, CLW_WarConfig.RTB_DELETE_RADIUS_M / 2);
		Print(string.Format("CLW: vehicle %1 RTB: transport drives to base %2 and will be removed (refund %3)", id, basePos, CLW_WarConfig.TRUCK_REFUND), LogLevel.NORMAL);
	}

	protected void TickReturn(CLW_ReturnJob job, float dt)
	{
		job.m_fAge += dt;
		job.m_fSinceIssue += dt;
		if (!job.m_Vehicle || !job.m_Crew || CLW_SquadInfo.AliveCount(job.m_Crew) <= 0)
		{
			if (job.m_Crew)
				CLW_Waypoints.Clear(job.m_Crew);
			Print(string.Format("CLW: vehicle %1 RTB aborted (vehicle or crew lost)", job.m_sId), LogLevel.NORMAL);
			job.m_bDone = true;
			return;
		}

		bool arrived = vector.DistanceXZ(job.m_Vehicle.GetOrigin(), job.m_vBase) <= CLW_WarConfig.RTB_DELETE_RADIUS_M;
		bool timeout = job.m_fAge >= CLW_WarConfig.RTB_TIMEOUT_SEC;
		if (!arrived && !timeout)
		{
			if (job.m_fSinceIssue >= 30 && !job.m_Crew.GetCurrentWaypoint())
			{
				job.m_fSinceIssue = 0;
				Print(string.Format("CLW: vehicle %1 RTB: waypoint lost, re-issuing", job.m_sId), LogLevel.NORMAL);
				CLW_Waypoints.AddMove(job.m_Crew, job.m_vBase, CLW_WarConfig.RTB_DELETE_RADIUS_M / 2);
			}

			return;
		}

		job.m_bDone = true;
		CLW_ForceSpawner.DeleteGroupAndMembers(job.m_Crew);
		SCR_EntityHelper.DeleteEntityAndChildren(job.m_Vehicle);
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (arrived)
		{
			Print(string.Format("CLW: vehicle %1 RTB arrived, transport removed, refund %2", job.m_sId, CLW_WarConfig.TRUCK_REFUND), LogLevel.NORMAL);
			if (war)
			{
				war.AddSupplies(job.m_sFaction, CLW_WarConfig.TRUCK_REFUND);
				war.PushEvent(job.m_sFaction, job.m_sId + " transport returned to base (+" + CLW_WarConfig.TRUCK_REFUND + " supplies)");
			}
		}
		else
		{
			Print(string.Format("CLW: vehicle %1 RTB timeout after %2 s, transport removed without refund", job.m_sId, Math.Round(job.m_fAge)), LogLevel.WARNING);
		}
	}

	// Rematch (CLW_Rematch.c): stop the tick, delete every vehicle unit (cargo, crew, vehicle), returning transports and abandoned vehicles.
	// Returns the number of vehicles deleted.
	int RematchReset()
	{
		GetGame().GetCallqueue().Remove(Tick);
		int deleted;
		foreach (CLW_VehicleUnit unit : m_aUnits)
		{
			CLW_ForceSpawner.DeleteGroupAndMembers(unit.GetCargoGroup());
			CLW_ForceSpawner.DeleteGroupAndMembers(unit.GetCrewGroup());
			IEntity unitVehicle = unit.GetVehicle();
			if (unitVehicle)
			{
				SCR_EntityHelper.DeleteEntityAndChildren(unitVehicle);
				deleted++;
			}
		}

		foreach (CLW_ReturnJob returnJob : m_aReturns)
		{
			CLW_ForceSpawner.DeleteGroupAndMembers(returnJob.m_Crew);
			if (returnJob.m_Vehicle)
			{
				SCR_EntityHelper.DeleteEntityAndChildren(returnJob.m_Vehicle);
				deleted++;
			}
		}

		foreach (CLW_AbandonJob abandonJob : m_aAbandoned)
		{
			if (abandonJob.m_Vehicle)
			{
				SCR_EntityHelper.DeleteEntityAndChildren(abandonJob.m_Vehicle);
				deleted++;
			}
		}

		m_aUnits.Clear();
		m_aReturns.Clear();
		m_aAbandoned.Clear();
		return deleted;
	}
}
