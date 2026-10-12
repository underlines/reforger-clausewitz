// Player requests (M3 phase Q, design section 6): the fast path that needs no LLM. A request (chat /request, request terminal action,
// later the LLM agent turn) becomes a CLW_ActionCall with authority "player" and runs through the action registry (REQUEST_VEHICLE,
// REQUEST_SQUAD, REQUEST_RESUPPLY, REQUEST_REINFORCE, REQUEST_RECON, see CLW_ActionsRequests.c). The executors hold the grant rules:
// role (squad leader vs soldier), per-player cooldown, supplies ledger, AI / vehicle caps, engine AI headroom (via CLW_ForceSpawner).
// This class: parsing, bookkeeping (history for the situation JSON and the dashboard), answers to the requester, log lines
// "CLW: request ...". Server only. All tunables are in CLW_RequestConfig.

class CLW_RequestConfig
{
	static const int HISTORY_MAX = 24;					// requests kept for the situation / dashboard
	static const int SITUATION_MAX = 10;				// newest requests of the faction sent in the situation
	static const int RESUPPLY_COST = 20;				// supplies for a resupply box
	static const float COOLDOWN_VEHICLE_SEC = 180;		// per player and request kind, counted from the last GRANTED request
	static const float COOLDOWN_SQUAD_SEC = 240;
	static const float COOLDOWN_RESUPPLY_SEC = 90;
	static const float COOLDOWN_ORDER_SEC = 120;		// reinforce / recon
	static const float RESUPPLY_NEAR_M = 80;			// "near a friendly base": a service of the side or the base within this
	static const int RESUPPLY_LIFETIME_MS = 600000;	// the box is removed after this
	static const int MAX_ISSUED_VEHICLES = 4;			// requested empty vehicles alive per side
	static const float ORDER_BUSY_PENALTY_M = 1500;		// a group with an active move/defend order counts this much further away
	static const float REPORT_COOLDOWN_SEC = 8;			// per player between two /report
	static const float REPORT_CONFIDENCE = 0.7;			// contacts made by /report
	static const float MARKER_CONFIDENCE = 0.5;			// contacts parsed from player map markers
	static const int MARKER_POLL_MS = 5000;
	static const int MARKER_DEFAULT_COUNT = 4;			// size assumed for a marker without a number in its text
	static const int REPORT_MAX_COUNT = 60;
	static const float REPORT_MAX_DIST_M = 3000;
	static const float REPORT_MIN_DIST_M = 10;
	static const int COSTS_DELAY_MS = 4000;				// after the war start: wait for the force catalogs
}

class CLW_RequestKind
{
	static const string VEHICLE = "vehicle";
	static const string SQUAD = "squad";
	static const string RESUPPLY = "resupply";
	static const string REINFORCE = "reinforce";
	static const string RECON = "recon";

	static bool IsKnown(string kind)
	{
		return kind == VEHICLE || kind == SQUAD || kind == RESUPPLY || kind == REINFORCE || kind == RECON;
	}

	static string ActionOf(string kind)
	{
		if (kind == VEHICLE)
			return "REQUEST_VEHICLE";
		if (kind == SQUAD)
			return "REQUEST_SQUAD";
		if (kind == RESUPPLY)
			return "REQUEST_RESUPPLY";
		if (kind == REINFORCE)
			return "REQUEST_REINFORCE";
		if (kind == RECON)
			return "REQUEST_RECON";

		return "";
	}

	// Squads and orders to HQ groups are for squad leaders; soldiers may ask for resupply and transport (a vehicle).
	static bool NeedsLeader(string kind)
	{
		return kind == SQUAD || kind == REINFORCE || kind == RECON;
	}

	static float CooldownSec(string kind)
	{
		if (kind == VEHICLE)
			return CLW_RequestConfig.COOLDOWN_VEHICLE_SEC;
		if (kind == SQUAD)
			return CLW_RequestConfig.COOLDOWN_SQUAD_SEC;
		if (kind == RESUPPLY)
			return CLW_RequestConfig.COOLDOWN_RESUPPLY_SEC;

		return CLW_RequestConfig.COOLDOWN_ORDER_SEC;
	}
}

class CLW_RequestRecord
{
	int m_iId;
	int m_iPlayer;
	string m_sName;
	string m_sFaction;
	string m_sRole;			// leader | soldier
	string m_sKind;
	string m_sArg;
	bool m_bGranted;
	string m_sReason;		// result text of the executor / the refusal reason
	float m_fTime;			// world time (s)
}

class CLW_RequestService
{
	protected static ref array<ref CLW_RequestRecord> s_aHistory = {};
	protected static ref map<string, float> s_mLastGrant = new map<string, float>();	// "<player>/<kind>" -> world time of the last grant
	protected static ref array<IEntity> s_aIssuedVehicles = {};
	protected static ref array<string> s_aIssuedFactions = {};
	protected static ref array<IEntity> s_aBoxes = {};
	protected static ref array<int> s_aBoxPlayers = {};
	protected static int s_iSeq;

	static float Now()
	{
		return GetGame().GetWorld().GetWorldTime() * 0.001;
	}

	// ---- lifecycle (CLW_GameMode) ----

	// Game start (server): statics survive a Play restart.
	static void OnGameStart()
	{
		s_aHistory.Clear();
		s_mLastGrant.Clear();
		s_aIssuedVehicles.Clear();
		s_aIssuedFactions.Clear();
		s_aBoxes.Clear();
		s_aBoxPlayers.Clear();
		s_iSeq = 0;
		CLW_VehicleDelivery.Reset();	// M4 WP4
		CLW_Reports.Reset();
		CLW_HqChat.Reset();	// M3 phase A: pending /hq messages
	}

	// War SETUP -> RUNNING: start the marker parser, tell the players the request costs once the catalogs exist.
	static void OnWarStart()
	{
		if (!Replication.IsServer())
			return;

		CLW_Reports.Start();
		CLW_HqChat.Start();	// M3 phase A: timeout tick of the /hq messages
		GetGame().GetCallqueue().CallLater(SendCostsToAll, CLW_RequestConfig.COSTS_DELAY_MS, false);
	}

	// ---- player facts ----

	static string PlayerName(int playerId)
	{
		PlayerManager players = GetGame().GetPlayerManager();
		if (!players)
			return "player" + playerId;

		string name = players.GetPlayerName(playerId);
		if (name.IsEmpty())
			return "player" + playerId;

		return name;
	}

	// Faction key of the player when it is one of the scenario factions, else "".
	static string FactionKeyOf(int playerId)
	{
		Faction faction = SCR_FactionManager.SGetPlayerFaction(playerId);
		if (!faction)
			return "";

		string key = faction.GetFactionKey();
		if (!CLW_ScenarioConfig.IsClwFaction(key))
			return "";

		return key;
	}

	static SCR_AIGroup GroupOf(int playerId)
	{
		SCR_GroupsManagerComponent groups = SCR_GroupsManagerComponent.GetInstance();
		if (!groups)
			return null;

		return groups.GetPlayerGroup(playerId);
	}

	static bool IsSquadLeader(int playerId)
	{
		SCR_AIGroup group = GroupOf(playerId);
		return group && group.IsPlayerLeader(playerId);
	}

	static string RoleOf(int playerId)
	{
		if (IsSquadLeader(playerId))
			return "leader";

		return "soldier";
	}

	static bool PlayerPos(int playerId, out vector pos)
	{
		IEntity entity = GetGame().GetPlayerManager().GetPlayerControlledEntity(playerId);
		if (!entity)
			return false;

		pos = entity.GetOrigin();
		return true;
	}

	// ---- cooldown ----

	protected static string CooldownKey(int playerId, string kind)
	{
		return playerId.ToString() + "/" + kind;
	}

	// Seconds left until the player may request this kind again (0 = now).
	static float CooldownLeft(int playerId, string kind)
	{
		float last;
		if (!s_mLastGrant.Find(CooldownKey(playerId, kind), last))
			return 0;

		float left = last + CLW_RequestKind.CooldownSec(kind) - Now();
		if (left < 0)
			return 0;

		return left;
	}

	static void MarkGranted(int playerId, string kind)
	{
		s_mLastGrant.Set(CooldownKey(playerId, kind), Now());
	}

	// ---- catalog picks (data: the faction's CLW_ForceCatalog, so RHS / WCS / GRS packs work) ----

	protected static bool Matches(CLW_ForceEntry entry, string wanted)
	{
		if (entry.m_sType == wanted || entry.m_sKind == wanted)
			return true;

		string name = entry.m_sName;
		name.ToLower();
		string typeText = entry.m_sType;
		typeText.ToLower();
		return name.Contains(wanted) || typeText.Contains(wanted);
	}

	// The vehicle to issue: arg = type id, kind or part of the name; empty = the cheapest transport (motorized, else light, else any).
	static CLW_ForceEntry PickVehicle(string factionKey, string arg)
	{
		CLW_ForceCatalog catalog = CLW_GameMode.GetForceCatalog();
		if (!catalog)
			return null;

		array<CLW_ForceEntry> entries = {};
		catalog.GetEntries(factionKey, entries, true);	// players drive: crewless vehicles too
		string wanted = arg;
		wanted = wanted.Trim();
		wanted.ToLower();
		CLW_ForceEntry best;
		int bestRank = 100;
		if (!wanted.IsEmpty())
		{
			// WP11: the depot menu sends the exact type id; an exact id wins over the loose name matching below
			foreach (CLW_ForceEntry exact : entries)
			{
				if (!exact || !exact.m_bEnabled || !exact.HasVehicle())
					continue;

				string exactType = exact.m_sType;
				exactType.ToLower();
				if (exactType == wanted)
					return exact;
			}
		}

		foreach (CLW_ForceEntry entry : entries)
		{
			if (!entry || !entry.m_bEnabled || !entry.HasVehicle())
				continue;

			if (!wanted.IsEmpty() && !Matches(entry, wanted))
				continue;

			int rank = 2;
			if (entry.m_sKind == CLW_ForceKind.MOTORIZED)
				rank = 0;
			else if (entry.m_sKind == CLW_ForceKind.LIGHT)
				rank = 1;

			if (!best || rank < bestRank || (rank == bestRank && VehicleCost(entry) < VehicleCost(best)))
			{
				best = entry;
				bestRank = rank;
			}
		}

		return best;
	}

	// The infantry team to issue: arg as above; empty = the faction's main infantry unit (highest buy weight, then most soldiers).
	static CLW_ForceEntry PickInfantry(string factionKey, string arg)
	{
		CLW_ForceCatalog catalog = CLW_GameMode.GetForceCatalog();
		if (!catalog)
			return null;

		array<CLW_ForceEntry> entries = {};
		catalog.GetEntries(factionKey, entries);
		string wanted = arg;
		wanted = wanted.Trim();
		wanted.ToLower();
		CLW_ForceEntry best;
		foreach (CLW_ForceEntry entry : entries)
		{
			if (!entry || !entry.m_bEnabled || entry.HasVehicle() || entry.m_GroupPrefab.IsEmpty())
				continue;

			if (!wanted.IsEmpty() && !Matches(entry, wanted))
				continue;

			if (!best || entry.m_iWeight > best.m_iWeight || (entry.m_iWeight == best.m_iWeight && entry.m_iGroupMembers > best.m_iGroupMembers))
				best = entry;
		}

		return best;
	}

	// Price of the empty vehicle: the vehicle share of the entry's price (same rule as CLW_VehicleUnit).
	static int VehicleCost(CLW_ForceEntry entry)
	{
		if (entry.m_iVehicleCost > 0)
			return Math.Min(entry.m_iVehicleCost, entry.m_iCost);

		int share = entry.m_iCost - entry.m_iCargoMembers * CLW_WarConfig.DISCOVER_SOLDIER_COST;
		return Math.Max(share, Math.Round(entry.m_iCost * 0.4));
	}

	// ---- issued vehicles / boxes (caps, cleanup) ----

	static void TrackVehicle(string factionKey, IEntity vehicle)
	{
		s_aIssuedVehicles.Insert(vehicle);
		s_aIssuedFactions.Insert(factionKey);
	}

	static int CountIssuedVehicles(string factionKey)
	{
		int count;
		for (int i = s_aIssuedVehicles.Count() - 1; i >= 0; i--)
		{
			if (!s_aIssuedVehicles[i])
			{
				s_aIssuedVehicles.Remove(i);
				s_aIssuedFactions.Remove(i);
				continue;
			}

			if (s_aIssuedFactions[i] == factionKey)
				count++;
		}

		return count;
	}

	// A player has at most one resupply box: the previous one goes away. Returns after registering the new one with its removal timer.
	static void TrackBox(int playerId, IEntity box)
	{
		for (int i = s_aBoxes.Count() - 1; i >= 0; i--)
		{
			if (!s_aBoxes[i])
			{
				s_aBoxes.Remove(i);
				s_aBoxPlayers.Remove(i);
				continue;
			}

			if (s_aBoxPlayers[i] == playerId)
			{
				SCR_EntityHelper.DeleteEntityAndChildren(s_aBoxes[i]);
				s_aBoxes.Remove(i);
				s_aBoxPlayers.Remove(i);
			}
		}

		s_aBoxes.Insert(box);
		s_aBoxPlayers.Insert(playerId);
		GetGame().GetCallqueue().CallLater(RemoveBox, CLW_RequestConfig.RESUPPLY_LIFETIME_MS, false, box);
	}

	protected static void RemoveBox(IEntity box)
	{
		if (!box)
			return;

		int at = s_aBoxes.Find(box);
		if (at >= 0)
		{
			s_aBoxes.Remove(at);
			s_aBoxPlayers.Remove(at);
		}

		SCR_EntityHelper.DeleteEntityAndChildren(box);
		Print("CLW: request resupply box removed after its lifetime", LogLevel.NORMAL);
	}

	// Nearest vehicle depot of the side to 'pos' (HQ depot or the depot of an active FOB). False when the side has none.
	static bool NearestDepot(string factionKey, vector pos, out IEntity depot, out string depotBase, out float dist)
	{
		array<CLW_ServiceInstance> list = {};
		CLW_Services.GetInstances(factionKey, "", list);
		float best = 1000000000;
		foreach (CLW_ServiceInstance inst : list)
		{
			if (inst.m_sKind != CLW_ServiceKind.VEHICLE_DEPOT || !inst.m_Entity)
				continue;

			if (inst.m_sBase != CLW_Services.BASE_HQ)
			{
				CLW_Fob fob = CLW_FobManager.Find(inst.m_sBase);
				if (!fob || !fob.IsActive())
					continue;
			}

			float d = vector.DistanceXZ(pos, inst.m_Entity.GetOrigin());
			if (d < best)
			{
				best = d;
				depot = inst.m_Entity;
				depotBase = inst.m_sBase;
			}
		}

		dist = best;
		return depot != null;
	}

	// Centre of a base: the HQ position or the FOB position. Zero when unknown.
	static vector BaseAnchor(string factionKey, string baseId)
	{
		if (CLW_SupplyStore.IsHq(baseId))
		{
			CLW_Bases bases = CLW_GameMode.GetBases();
			if (bases)
				return bases.GetBasePos(factionKey);

			return vector.Zero;
		}

		CLW_Fob fob = CLW_FobManager.Find(baseId);
		if (fob)
			return fob.m_vPos;

		return vector.Zero;
	}

	// Parking spot for a requested vehicle (collision-free: CLW_FreeSpot). Rules:
	//   request at a terminal (baseId set)      the depot of that base (FOB without a depot: ground beside the FOB)
	//   chat request, requester within DEPOT_NEAR_M of a depot (or of the HQ when the side has no depot)   that depot
	//   chat request further away               near the requester: the nearest road within REQUEST_ROAD_M (road edge, aligned with the
	//                                           road), else open ground within REQUEST_GROUND_M; if neither exists the nearest depot
	// False when no clear spot was found. where = text for the answer ("depot", "HQ", "FOB1", "roadside near you", ...).
	static bool FindVehicleSpot(string factionKey, out vector spot, out float yaw, out string via, out string where, string baseId = "", vector requesterPos = vector.Zero)
	{
		CLW_Bases bases = CLW_GameMode.GetBases();
		vector anchor;
		if (bases)
			anchor = bases.GetBasePos(factionKey);

		IEntity depot;
		string depotBase = baseId;
		string label = "vehicle " + factionKey;
		if (!CLW_SupplyStore.IsHq(baseId))
		{
			// M3 phase L: a request made at a FOB terminal parks the vehicle at that FOB (its depot, else beside its position)
			CLW_Fob fob = CLW_FobManager.Find(baseId);
			if (fob && fob.m_sSide == factionKey && fob.IsActive())
			{
				anchor = fob.m_vPos;
				depot = CLW_Services.FindAt(factionKey, baseId, CLW_ServiceKind.VEHICLE_DEPOT);
				if (!depot)
				{
					where = baseId;
					CLW_SpotParams fobParams = CLW_FreeSpot.VehicleParams(label + " " + baseId, anchor);
					fobParams.m_fMinRadius = 12;
					fobParams.m_fMaxRadius = 60;
					fobParams.m_fRoadWeight = 0.5;
					fobParams.m_bRoadYaw = true;
					CLW_SpotResult fobRes = new CLW_SpotResult();
					if (CLW_FreeSpot.FindFreeSpot(fobParams, fobRes))
					{
						spot = fobRes.m_vPos;
						yaw = fobRes.m_fYaw;
						via = fobRes.m_sVia;
						return true;
					}

					return false;
				}
			}
		}
		else if (baseId.IsEmpty() && requesterPos != vector.Zero)
		{
			// chat request: the depot nearest to the requester, used when the requester is close to it
			float depotDist;
			bool haveDepot = NearestDepot(factionKey, requesterPos, depot, depotBase, depotDist);
			bool atBase;
			if (haveDepot)
				atBase = depotDist <= CLW_SpotConfig.DEPOT_NEAR_M;
			else if (anchor != vector.Zero)
				atBase = vector.DistanceXZ(requesterPos, anchor) <= CLW_SpotConfig.DEPOT_NEAR_M;

			if (haveDepot)
				anchor = BaseAnchor(factionKey, depotBase);

			if (!atBase)
			{
				if (CLW_FreeSpot.FindNearRequester(label, requesterPos, spot, yaw, via))
				{
					where = "roadside near you";
					if (via == "open-ground")
						where = "open ground near you";

					return true;
				}

				Print(string.Format("CLW: request vehicle %1: no clear road or ground near the requester at %2, using the base", factionKey, requesterPos), LogLevel.NORMAL);
			}
		}
		else
			depot = CLW_Services.FindAt(factionKey, CLW_Services.BASE_HQ, CLW_ServiceKind.VEHICLE_DEPOT);

		if (!depot && CLW_SupplyStore.IsHq(baseId))
			depot = CLW_Services.Find(factionKey, CLW_ServiceKind.VEHICLE_DEPOT);
		if (depot)
		{
			where = "depot";
			vector depotPos = depot.GetOrigin();
			if (CLW_FreeSpot.FindDepotSpot(label + " depot", depotPos, 8, 45, anchor, spot, yaw, via))
				return true;

			if (CLW_FreeSpot.FindDepotSpot(label + " depot wide", depotPos, 8, 90, anchor, spot, yaw, via))
				return true;
		}

		// the scenario's hand-placed slots / road spots of the HQ (trusted ground, occupancy checked there)
		where = "HQ";
		if (!bases)
			return false;

		return bases.FindVehicleSpot(factionKey, CLW_WarConfig.SPAWN_RADIUS_M, CLW_WarConfig.SPAWN_VEHICLE_CYLINDER_R, CLW_WarConfig.SPAWN_VEHICLE_CYLINDER_H, spot, yaw, via);
	}

	// Spawn an empty vehicle. Null (logged) on failure.
	static IEntity SpawnVehicle(ResourceName prefab, vector spot, float yaw)
	{
		Resource resource = Resource.Load(prefab);
		if (!resource || !resource.IsValid())
		{
			Print("CLW: request vehicle prefab does not load: " + prefab, LogLevel.ERROR);
			return null;
		}

		vector pos = spot;
		pos[1] = GetGame().GetWorld().GetSurfaceY(pos[0], pos[2]);
		EntitySpawnParams params = new EntitySpawnParams();
		params.TransformMode = ETransformMode.WORLD;
		vector mat[4];
		Math3D.AnglesToMatrix(Vector(yaw, 0, 0), mat);
		params.Transform[0] = mat[0];
		params.Transform[1] = mat[1];
		params.Transform[2] = mat[2];
		params.Transform[3] = pos;
		return GetGame().SpawnEntityPrefab(resource, GetGame().GetWorld(), params);
	}

	// ---- entry points ----

	// Chat "/request ..." and the terminal actions: args = "<kind> [arg]" or "" (the menu). Runs on the server, answers the player.
	static void Chat(int playerId, string args, vector terminalPos = vector.Zero)
	{
		if (!Replication.IsServer())
			return;

		array<string> tokens = {};
		args.Split(" ", tokens, true);
		if (tokens.IsEmpty())
		{
			ShowMenu(playerId);
			return;
		}

		string kind = tokens[0];
		kind.ToLower();
		string arg;
		for (int i = 1; i < tokens.Count(); i++)
		{
			if (!arg.IsEmpty())
				arg += " ";
			arg += tokens[i];
		}

		Handle(playerId, kind, arg, terminalPos);
	}

	// Fix A: a terminal only serves requesters of its own side. The side was bound when the terminal was spawned. True = ok, baseId set.
	protected static bool CheckTerminal(int playerId, vector terminalPos, string faction, out string baseId, out string refusal)
	{
		// WP11: the position may be a vehicle depot of the requester's side (the depot menu); the requester must stand near it
		CLW_ServiceInstance depot = CLW_DepotLookup.FindAt(faction, terminalPos);
		if (depot)
		{
			if (!CLW_DepotLookup.PlayerNear(playerId, depot, refusal))
			{
				Print(string.Format("CLW: depot request player=%1 refused at %2 %3: %4", playerId, depot.m_sSide, depot.m_sBase, refusal), LogLevel.NORMAL);
				return false;
			}

			baseId = depot.m_sBase;
			return true;
		}

		CLW_TerminalInfo terminal = CLW_RequestTerminals.FindAt(terminalPos);
		if (!terminal)
		{
			refusal = "that request terminal is not in service";
			Print(string.Format("CLW: request terminal at %1 unknown or removed: refused", terminalPos), LogLevel.NORMAL);
			return false;
		}

		if (terminal.m_sSide != faction)
		{
			refusal = "this terminal belongs to " + terminal.m_sSide + ", you are " + faction;
			Print(string.Format("CLW: request terminal %1 %2 refused a %3 requester", terminal.m_sSide, terminal.m_sBase, faction), LogLevel.NORMAL);
			return false;
		}

		baseId = terminal.m_sBase;
		return true;
	}

	// One request: validates and executes through the registry, records and answers. Returns the answer text.
	static string Handle(int playerId, string kind, string arg, vector terminalPos = vector.Zero)
	{
		string faction = FactionKeyOf(playerId);
		string role = RoleOf(playerId);
		bool granted;
		string reply;
		string terminalBase;	// base the request terminal belongs to ("" = chat request = HQ)
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (kind == "help" || kind == "list" || kind == "?")
		{
			ShowMenu(playerId);
			return "";
		}

		if (!CLW_RequestKind.IsKnown(kind))
		{
			reply = "unknown request '" + kind + "'. Use /request vehicle|squad|resupply|reinforce|recon";
		}
		else if (!war || !war.IsRunning())
		{
			reply = "the war has not started yet";
		}
		else if (faction.IsEmpty())
		{
			reply = "you are not on a side";
		}
		else if (terminalPos != vector.Zero && !CheckTerminal(playerId, terminalPos, faction, terminalBase, reply))
		{
			// fix A: reply holds the refusal (unknown terminal, or it belongs to the other side)
		}
		else
		{
			CLW_ActionCall call = new CLW_ActionCall();
			call.m_sAction = CLW_RequestKind.ActionOf(kind);
			call.m_sFaction = faction;
			call.m_sAuthority = CLW_ActionEntry.AUTH_PLAYER;
			call.m_sSource = "player";
			call.m_iPlayerId = playerId;
			call.m_sReason = "request by " + PlayerName(playerId);
			call.SetString("base", terminalBase);	// M3 phase L: spending draws from this base's stock (empty = HQ)
			CLW_RemoteCommander commander = CLW_GameMode.GetCommander(faction);
			if (commander)
				call.m_Rules = commander.GetRules();

			if (kind == CLW_RequestKind.RECON || kind == CLW_RequestKind.REINFORCE)
				call.SetString("where", arg);
			else
				call.SetString("type", arg);

			string result;
			granted = CLW_ActionRegistry.Get().Run(call, result);
			reply = result;
		}

		CLW_RequestRecord record = new CLW_RequestRecord();
		s_iSeq++;
		record.m_iId = s_iSeq;
		record.m_iPlayer = playerId;
		record.m_sName = PlayerName(playerId);
		record.m_sFaction = faction;
		record.m_sRole = role;
		record.m_sKind = kind;
		record.m_sArg = arg;
		record.m_bGranted = granted;
		record.m_sReason = reply;
		record.m_fTime = Now();
		s_aHistory.Insert(record);
		if (s_aHistory.Count() > CLW_RequestConfig.HISTORY_MAX)
			s_aHistory.RemoveOrdered(0);

		string verdict = "REFUSED";
		if (granted)
			verdict = "GRANTED";

		Print(string.Format("CLW: request #%1 player=%2(%3) faction=%4 role=%5 kind=%6 arg='%7' -> %8: %9", record.m_iId, playerId, record.m_sName, faction, role, kind, arg, verdict, reply), LogLevel.NORMAL);
		string answer = "HQ: request " + kind + " refused - " + reply;
		if (granted)
			answer = "HQ: " + reply;

		// M4 WP4: vehicle delivery feedback. The executor tracked the new vehicle (TrackVehicle) just before it returned.
		if (granted && kind == CLW_RequestKind.VEHICLE && !s_aIssuedVehicles.IsEmpty())
		{
			IEntity delivered = s_aIssuedVehicles[s_aIssuedVehicles.Count() - 1];
			if (delivered)
			{
				answer = "HQ: vehicle delivered, grid " + CLW_Places.Grid(delivered.GetOrigin()) + ". " + reply;
				CLW_VehicleDelivery.Track(playerId, faction, delivered);
			}
		}

		Answer(playerId, answer, true);
		return answer;
	}

	// ---- the menu ----

	static void ShowMenu(int playerId)
	{
		string faction = FactionKeyOf(playerId);
		string role = RoleOf(playerId);
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (faction.IsEmpty() || !war || !war.IsRunning())
		{
			Answer(playerId, "HQ: requests are available once the war is running and you are on a side.", true);
			return;
		}

		Print(string.Format("CLW: request menu shown to player=%1 faction=%2 role=%3", playerId, faction, role), LogLevel.NORMAL);
		Answer(playerId, "HQ requests (you are a " + role + ", supplies " + war.GetSupplies(faction) + "):", false);
		string line;
		CLW_ForceEntry vehicle = PickVehicle(faction, "");
		line = "/request vehicle [type] - empty vehicle at the depot (near you when you are far from a base)";
		if (vehicle)
			line += ", default " + vehicle.GetVehicleName() + " cost " + VehicleCost(vehicle);
		Answer(playerId, line, false);

		CLW_ForceEntry team = PickInfantry(faction, "");
		line = "/request squad [type] - AI team joins your squad (squad leaders)";
		if (team)
			line += ", default " + team.m_sName + " cost " + team.m_iCost;
		Answer(playerId, line, false);
		Answer(playerId, "/request resupply - ammo box at your position near a base, else at HQ, cost " + CLW_RequestConfig.RESUPPLY_COST, false);
		Answer(playerId, "/request reinforce - HQ sends the nearest free group to you (squad leaders, free)", false);
		Answer(playerId, "/request recon <grid|here|objective> - HQ sends a group to look (squad leaders, free)", false);
		Answer(playerId, "/report <count> <inf|veh|armor> <dist m> <dir> - report enemy contact to HQ", false);
		array<string> types = {};
		ListTypes(faction, types);
		if (!types.IsEmpty())
		{
			line = "types:";
			foreach (string text : types)
			{
				line += " " + text;
			}
			Answer(playerId, line, false);
		}
	}

	// "truck(60)" style list of the vehicle and infantry types a request can name (max 8).
	protected static void ListTypes(string factionKey, notnull array<string> outTypes)
	{
		CLW_ForceCatalog catalog = CLW_GameMode.GetForceCatalog();
		if (!catalog)
			return;

		array<CLW_ForceEntry> entries = {};
		catalog.GetEntries(factionKey, entries, true);
		foreach (CLW_ForceEntry entry : entries)
		{
			if (!entry || !entry.m_bEnabled || outTypes.Count() >= 8)
				continue;

			if (entry.HasVehicle())
				outTypes.Insert(entry.m_sType + "(v" + VehicleCost(entry) + ")");
			else
				outTypes.Insert(entry.m_sType + "(s" + entry.m_iCost + ")");
		}
	}

	// ---- answers ----

	// Chat line (and a hint when asked) to the requester.
	static void Answer(int playerId, string text, bool hint)
	{
		if (playerId <= 0)
			return;

		if (playerId == SCR_PlayerController.GetLocalPlayerId())
		{
			CLW_RequestClient.Show(text, hint);	// listen-server host: no RPC
			return;
		}

		SCR_PlayerController controller = SCR_PlayerController.Cast(GetGame().GetPlayerManager().GetPlayerController(playerId));
		if (controller)
			controller.CLW_RequestAnswer(text, hint);
	}

	// ---- squad completion (CLW_ForceSpawner.Finish) ----

	// The AI team bought by a request is complete: it joins the requester's squad.
	static void OnSquadReady(int playerId, SCR_AIGroup group, string unitId, string typeName)
	{
		SCR_AIGroup squad = GroupOf(playerId);
		if (!squad)
		{
			Print(string.Format("CLW: request squad %1 (%2) is ready but player %3 has no squad any more: it stays with HQ", unitId, typeName, playerId), LogLevel.WARNING);
			Answer(playerId, "HQ: " + typeName + " arrived at the base, but you have no squad to join; HQ keeps it.", true);
			return;
		}

		int moved = CLW_PlayerSquad.AdoptAI(squad, group, playerId);
		Print(string.Format("CLW: request squad %1 (%2) READY: %3 AI joined the squad of player %4", unitId, typeName, moved, playerId), LogLevel.NORMAL);
		if (moved > 0)
			Answer(playerId, "HQ: " + typeName + " (" + moved + " men) has joined your squad.", true);
		else
			Answer(playerId, "HQ: " + typeName + " arrived but nobody could join your squad.", true);
	}

	// ---- costs for the terminal action labels ----

	static string CostWire(string factionKey)
	{
		string wire = "";
		CLW_ForceEntry vehicle = PickVehicle(factionKey, "");
		if (vehicle)
			wire += "vehicle=" + VehicleCost(vehicle) + ";";
		CLW_ForceEntry team = PickInfantry(factionKey, "");
		if (team)
			wire += "squad=" + team.m_iCost + ";";
		wire += "resupply=" + CLW_RequestConfig.RESUPPLY_COST + ";";
		return wire;
	}

	static void SendCosts(int playerId)
	{
		if (!Replication.IsServer() || playerId <= 0)
			return;

		string faction = FactionKeyOf(playerId);
		if (faction.IsEmpty())
			return;

		string wire = CostWire(faction);
		if (playerId == SCR_PlayerController.GetLocalPlayerId())
		{
			CLW_RequestClient.SetCosts(wire);
			return;
		}

		SCR_PlayerController controller = SCR_PlayerController.Cast(GetGame().GetPlayerManager().GetPlayerController(playerId));
		if (controller)
			controller.CLW_RequestCosts(wire);
	}

	protected static void SendCostsToAll()
	{
		PlayerManager players = GetGame().GetPlayerManager();
		if (!players)
			return;

		array<int> ids = {};
		players.GetPlayers(ids);
		foreach (int id : ids)
		{
			SendCosts(id);
		}
	}

	// ---- situation (protocol v3 "requests") ----

	// JSON array of the faction's recent requests, newest first: [{"id","player","role","kind","args","status","reason","ageSec"}].
	static string GetJson(string factionKey)
	{
		string text = "";
		int written;
		float now = Now();
		for (int i = s_aHistory.Count() - 1; i >= 0 && written < CLW_RequestConfig.SITUATION_MAX; i--)
		{
			CLW_RequestRecord record = s_aHistory[i];
			if (record.m_sFaction != factionKey)
				continue;

			string status = "refused";
			if (record.m_bGranted)
				status = "granted";

			if (written > 0)
				text += ",";
			text += "{\"id\":" + record.m_iId + ",\"player\":\"" + CLW_Situation.Escape(record.m_sName) + "\",\"role\":\"" + record.m_sRole + "\",\"kind\":\"" + CLW_Situation.Escape(record.m_sKind) + "\"";
			text += ",\"args\":\"" + CLW_Situation.Escape(record.m_sArg) + "\",\"status\":\"" + status + "\",\"reason\":\"" + CLW_Situation.Escape(record.m_sReason) + "\",\"ageSec\":" + Math.Round(Math.Max(0, now - record.m_fTime)) + "}";
			written++;
		}

		return "[" + text + "]";
	}

	// Rematch (CLW_Rematch.c): delete the vehicles and boxes issued to players, remove the delivery markers, forget history and cooldowns
	// (OnGameStart also resets reports, /hq chat and deliveries). Returns the entities deleted.
	static int RematchReset()
	{
		int deleted;
		foreach (IEntity issued : s_aIssuedVehicles)
		{
			if (issued)
			{
				SCR_EntityHelper.DeleteEntityAndChildren(issued);
				deleted++;
			}
		}

		foreach (IEntity box : s_aBoxes)
		{
			if (box)
			{
				SCR_EntityHelper.DeleteEntityAndChildren(box);
				deleted++;
			}
		}

		CLW_VehicleDelivery.RematchReset();
		OnGameStart();
		return deleted;
	}
}
