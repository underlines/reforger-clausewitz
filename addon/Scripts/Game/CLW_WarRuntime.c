// War runtime of Clausewitz (M4 takeover, option C of dev/research/gm-takeover-spike.md).
//
// Everything the war needs at run time (bases, control points, war state, registry, catalog, spawner, commanders, vehicle manager,
// timers, the game start bootstrap, the player flow) lives here, in a plain holder, and NOT in an entity class. The holder is hosted by
// ONE game mode entity (m_Owner):
//   - CLW_GameMode (our own prefab / wrapper worlds): always, on every machine, exactly as before the refactor.
//   - any other SCR_BaseGameMode (e.g. the vanilla SCR_GameModeEditor of a Game Master world): only when the takeover switch is on
//     (server only; command line -clwTakeover or "takeover": true in $profile:Clausewitz/overrides.json), see TakeoverSwitch.
// The game mode entity forwards its events through the thin hooks in CLW_GameModeHooks.c (modded SCR_BaseGameMode). The static getters
// of CLW_GameMode (GetBases, GetWarState, ...) keep working in both cases: they read this holder.
// A takeover logs every bootstrap step as "CLW: takeover step <n> <what> ok|FAILED <why>".

// Optional flag in overrides.json (other keys of the file are ignored; absent = false).
class CLW_TakeoverFlags
{
	bool takeover;
}

class CLW_WarRuntime
{
	protected static const bool RUN_ORDER_SPIKE = false;
	protected static const bool RUN_REST_SPIKE = false;	// M1 test (needs dev/tools/echo_server), off for normal play
	protected static const int SQUAD_RETRY_MS = 5000;		// player squad search interval
	protected static const int SQUAD_RETRY_MAX = 24;		// 2 min, then a free infantry squad is spawned
	protected static const int SQUAD_RETRY_EXTRA = 12;		// further tries for that squad to appear
	protected static const int TAKEOVER_CHECK_MS = 12000;	// delayed health line of a takeover

	static const string CLI_TAKEOVER = "clwTakeover";
	static const string CLI_SCENARIO = "clwScenario";
	// Engine infrastructure (AI perception), spawned when a taken-over world has none.
	static const ResourceName PERCEPTION_PREFAB = "{028DAEAD63E056BE}Prefabs/World/Game/PerceptionManager.et";
	static const ResourceName CHAT_PREFAB = "{F69BC912AC8236F9}Prefabs/MP/ScriptedChatEntity.et";	// as placed by the wrapper worlds

	protected static ref CLW_WarRuntime s_Instance;
	protected static bool s_bClientSeen;	// a client received a war status from a taken-over game mode

	protected SCR_BaseGameMode m_Owner;
	protected bool m_bTakeover;
	protected int m_iStep;

	// M2 war systems (server only, started in this order: scenario -> bases -> control points -> war state -> registry/commanders).
	protected ref CLW_Bases m_Bases;
	protected ref CLW_ControlPoints m_Points;
	protected ref CLW_WarState m_War;
	protected ref CLW_ForceCatalog m_Catalog;
	protected ref CLW_VehicleManager m_Vehicles;
	protected ref CLW_ForceSpawner m_Spawner;
	protected ref CLW_Activity m_Activity = new CLW_Activity();

	// Which AI groups HQ commands (scene-placed and added at runtime, e.g. from Game Master); the commanders and markers query it.
	protected ref CLW_SquadRegistry m_Registry = new CLW_SquadRegistry();
	// One contact picture, marker set and commander per scenario faction, keyed by faction key. The keys can change at war start
	// (faction choice in the setup), so these are (re)created from the scenario in BuildFactionObjects, never cached at init.
	protected ref map<string, ref CLW_ContactBook> m_mBooks = new map<string, ref CLW_ContactBook>();
	protected ref map<string, ref CLW_ContactMarkers> m_mMarkers = new map<string, ref CLW_ContactMarkers>();
	protected ref map<string, ref CLW_RemoteCommander> m_mCommanders = new map<string, ref CLW_RemoteCommander>();
	protected ref array<string> m_aBuiltKeys = {};

	// ---- static access (CLW_GameMode getters, hooks) ----

	static CLW_WarRuntime Get()
	{
		return s_Instance;
	}

	// The runtime when it is hosted by exactly this game mode entity, else null (hooks of every other game mode do nothing).
	static CLW_WarRuntime Of(SCR_BaseGameMode mode)
	{
		if (s_Instance && mode && s_Instance.m_Owner == mode)
			return s_Instance;

		return null;
	}

	// The game mode entity that hosts the war (CLW_GameMode or a taken-over one), null before the game start.
	static SCR_BaseGameMode GetOwner()
	{
		if (!s_Instance)
			return null;

		return s_Instance.m_Owner;
	}

	// True when this game is a Clausewitz war: our own game mode (any machine), or a taken-over one (server: the runtime exists,
	// client: a war status arrived). Replaces the old "game mode is a CLW_GameMode" test.
	static bool IsActive()
	{
		if (CLW_GameMode.Cast(GetGame().GetGameMode()) != null)
			return true;

		if (s_Instance && s_Instance.m_bTakeover)
			return true;

		return s_bClientSeen;
	}

	static bool IsTakeover()
	{
		return s_Instance && s_Instance.m_bTakeover;
	}

	CLW_Bases GetBases()
	{
		return m_Bases;
	}

	CLW_ControlPoints GetControlPoints()
	{
		return m_Points;
	}

	CLW_WarState GetWarState()
	{
		return m_War;
	}

	CLW_SquadRegistry GetRegistry()
	{
		return m_Registry;
	}

	CLW_ForceCatalog GetForceCatalog()
	{
		return m_Catalog;
	}

	CLW_ForceSpawner GetForceSpawner()
	{
		return m_Spawner;
	}

	CLW_VehicleManager GetVehicleManager()
	{
		return m_Vehicles;
	}

	CLW_RemoteCommander GetCommander(string factionKey)
	{
		CLW_RemoteCommander commander;
		if (m_mCommanders.Find(factionKey, commander))
			return commander;

		return null;
	}

	// ---- takeover switch ----

	// "cli" (-clwTakeover), "overrides" (overrides.json "takeover": true) or "" (off, the default).
	static string TakeoverSwitch()
	{
		if (System.IsCLIParam(CLI_TAKEOVER))
			return "cli";

		if (!FileIO.FileExists(CLW_CatalogOverrides.FILE_PATH))
			return "";

		string text = SCR_FileIOHelper.GetFileStringContent(CLW_CatalogOverrides.FILE_PATH, false);
		if (text.IsEmpty())
			return "";

		SCR_JsonLoadContext ctx = new SCR_JsonLoadContext();
		CLW_TakeoverFlags flags = new CLW_TakeoverFlags();
		if (ctx.ImportFromString(text) && ctx.ReadValue("", flags) && flags.takeover)
			return "overrides";

		return "";
	}

	// Entry point of the OnGameStart hook (every SCR_BaseGameMode, after its own start code). Decides whether this game mode entity
	// hosts the war: CLW_GameMode always, anything else only on the server with the takeover switch on.
	static void OnGameModeStart(SCR_BaseGameMode mode)
	{
		CLW_GameMode own = CLW_GameMode.Cast(mode);
		string switchName;
		if (!own)
		{
			if (!Replication.IsServer())
				return;

			switchName = TakeoverSwitch();
			if (switchName.IsEmpty())
			{
				Print(string.Format("CLW: takeover off for game mode %1 in %2 (no -%3, no \"takeover\": true in overrides.json)", mode.ClassName(), GetGame().GetWorldFile(), CLI_TAKEOVER), LogLevel.NORMAL);
				return;
			}
		}

		CLW_WarRuntime runtime = new CLW_WarRuntime();
		s_Instance = runtime;
		s_bClientSeen = false;
		runtime.Start(mode, own != null, switchName);
	}

	// A client got a war status from a taken-over game mode: it is a Clausewitz war (arsenal pool, commanding limit).
	static void OnClientStatus()
	{
		if (s_bClientSeen)
			return;

		s_bClientSeen = true;
		if (!CLW_GameMode.Cast(GetGame().GetGameMode()))
		{
			Print("CLW: takeover: war status received, client side Clausewitz rules on", LogLevel.NORMAL);
			CLW_PlayerSquad.ApplyCommandingLimit();
		}
	}

	// ---- bootstrap ----

	// Takeover log: "CLW: takeover step <n> <what> ok|FAILED <why>" (silent for our own game mode).
	protected void Step(string what, bool ok, string why = "")
	{
		if (!m_bTakeover)
			return;

		m_iStep++;
		string state = "ok";
		LogLevel level = LogLevel.NORMAL;
		if (!ok)
		{
			state = "FAILED";
			level = LogLevel.ERROR;
		}

		Print("CLW: takeover step " + m_iStep.ToString() + " " + what + " " + state + " " + why, level);
	}

	protected void Start(SCR_BaseGameMode owner, bool own, string switchName)
	{
		m_Owner = owner;
		m_bTakeover = !own;
		Print("CLW: build " + CLW_Build.STAMP, LogLevel.NORMAL);
		Print("CLW: game mode started", LogLevel.NORMAL);
		if (m_bTakeover)
			Print(string.Format("CLW: takeover of %1 in %2 (switch: %3)", owner.ClassName(), GetGame().GetWorldFile(), switchName), LogLevel.NORMAL);

		if (!m_bTakeover)
			CLW_PlayerSquad.ApplyCommandingLimit();	// every machine

		if (!Replication.IsServer())
			return;

		if (m_bTakeover)
			CheckHost(switchName);

		if (RUN_REST_SPIKE)
			GetGame().GetCallqueue().CallLater(CLW_RestSpike.Run, 10000);
		if (RUN_ORDER_SPIKE)
			GetGame().GetCallqueue().CallLater(SpikeOrderLoop, 15000);

		BootWar(false);

		// one-time hooks of this game mode entity (a rematch does not repeat them)
		m_Owner.GetOnPlayerRegistered().Insert(OnClwPlayerRegistered);
		GetGame().GetCallqueue().CallLater(CLW_WorldInfo.Start, 5000);
		GetGame().GetCallqueue().CallLater(SayBuild, 8000);

		if (m_bTakeover)
			GetGame().GetCallqueue().CallLater(TakeoverHealth, TAKEOVER_CHECK_MS);
	}

	// The war systems of one round, in order: action registry, scenario, bases, control points, war state, forces, mission board, squad join,
	// registry, faction objects, timers. Called once at the game start (restart false) and again by every rematch (restart true) after the
	// war entities were deleted and the managers reset (CLW_Rematch.c); then the existing manager objects are re-initialised and the war
	// state goes back to SETUP. Returns false when no scenario could be resolved.
	protected bool BootWar(bool restart)
	{
		CLW_Services.Reset();	// base services of a previous session (statics survive a Play restart)
		CLW_FobManager.Reset();	// FOBs of a previous session
		CLW_Convoys.Reset();	// M3 phase L: convoys + per-base stock of a previous session

		// 0. action registry of this war (data entries from Configs/CLW/Actions/Actions.conf; script entries register after this)
		CLW_ActionRegistry.Reset();
		CLW_ActionsRequests.Register();	// M3 phase Q: player request entries (script-registered)
		CLW_ActionsLogistics.Register();	// M3 phase L: RESUPPLY / CONVOY (script-registered)
		CLW_RequestService.OnGameStart();
		Step("action registry", true);

		// 1. scenario data (factions, bases, objectives, economy)
		CLW_ScenarioConfig previous = CLW_ScenarioConfig.Get();
		CLW_ScenarioConfig scenario;
		if (m_bTakeover)
			scenario = LoadTakeoverScenario();
		else
			scenario = LoadScenario(CLW_GameMode.Cast(m_Owner).GetScenarioAttribute());

		if (restart && scenario && scenario == previous)
			Print("CLW: rematch WARNING the scenario loader returned the previous (mutated) instance: sides and layout of the last round may leak into this one", LogLevel.WARNING);

		if (scenario)
		{
			Step("scenario " + scenario.m_sName, true);
			// 2. bases + respawn points, 3. control points, 4. war state
			if (!m_Bases)
				m_Bases = new CLW_Bases();
			m_Bases.Init(scenario);
			Step("bases", true);
			if (!m_Points)
				m_Points = new CLW_ControlPoints();
			m_Points.Init(scenario);
			Step("control points", true);
			if (restart && m_War)
			{
				m_War.Restart(scenario, m_Points);
			}
			else
			{
				m_War = new CLW_WarState();
				m_War.Init(scenario, m_Points);
			}
			Step("war state", true);
			// 5. forces: catalog (built at war start), vehicle units, spawner; start forces when the war starts
			m_Catalog = new CLW_ForceCatalog();
			m_Registry.SetCatalog(m_Catalog);
			if (!m_Vehicles)
				m_Vehicles = new CLW_VehicleManager();
			m_Vehicles.Init(m_Registry);
			if (!m_Spawner)
				m_Spawner = new CLW_ForceSpawner();
			m_Spawner.Init(m_Catalog, m_Registry, m_Vehicles);
			m_War.GetOnWarStarted().Remove(OnWarStarted);
			m_War.GetOnWarStarted().Insert(OnWarStarted);
			Step("forces (catalog, vehicles, spawner)", true);
			// 6. missions for human squad leaders (replaces the rule commander's legacy forced task; Install cleans the previous board up)
			CLW_MissionBoard.Install();
			Step("mission board", true);
		}
		else
		{
			Print("CLW: NO SCENARIO: war systems, registry adoption and commanders are disabled", LogLevel.ERROR);
			Step("scenario", false, "none resolved: war systems disabled");
		}

		// Squad join service: vanilla group events -> commandable squads; HQ squads are the only player groups.
		CLW_SquadJoin.Init();
		CLW_SquadGroups.LockNewGroups();
		GetGame().GetCallqueue().CallLater(PruneVanillaGroups, 3000);	// after the groups manager made its predefined groups
		Step("squad join + group lock", true);

		// Adopt the scene-placed squads now (before the first commander tick), then keep sweeping for new/lost groups.
		m_Registry.Start();
		Step("squad registry", true);
		// The commanders take their squads from the registry; human-led squads get missions instead of waypoints.
		if (scenario)
			BuildFactionObjects();

		if (scenario)
		{
			GetGame().GetCallqueue().Remove(PollContacts);
			GetGame().GetCallqueue().Remove(ThinkCommanders);
			GetGame().GetCallqueue().CallLater(PollContacts, 20000, true);
			m_Activity.Start();
			GetGame().GetCallqueue().CallLater(ThinkCommanders, 10000, true);
			Step("timers (contacts, activity, commanders)", true);
		}

		return scenario != null;
	}

	// ---- rematch (CLW_Rematch.c) ----

	// Rematch step 1: stop what plans or buys (contact poll, commander think, activity tick), remove the contact markers and drop the
	// per-faction objects (rebuilt by BootWar). Returns the number of commanders dropped.
	int RematchStop()
	{
		GetGame().GetCallqueue().Remove(PollContacts);
		GetGame().GetCallqueue().Remove(ThinkCommanders);
		m_Activity.RematchReset();
		foreach (string markerKey, CLW_ContactMarkers markers : m_mMarkers)
		{
			if (markers)
				markers.RemoveAll();
		}

		int commanders = m_mCommanders.Count();
		m_mBooks.Clear();
		m_mMarkers.Clear();
		m_mCommanders.Clear();
		m_aBuiltKeys.Clear();
		return commanders;
	}

	// Rematch last step: the war systems of a new round (scenario reloaded, war state back in SETUP). False when no scenario resolved.
	bool RematchRestart()
	{
		return BootWar(true);
	}

	// The game mode ends (world shutdown / reload, every machine): forget the holder so a reloaded world starts clean. A stale holder
	// kept a dead game mode entity and its timers across the reload (statics survive it).
	static void OnGameModeEnd(SCR_BaseGameMode mode)
	{
		s_bClientSeen = false;
		if (!s_Instance || s_Instance.m_Owner != mode)
			return;

		Print("CLW: game mode ended: war runtime released (statics cleared for a world reload)", LogLevel.NORMAL);
		if (Replication.IsServer())
		{
			s_Instance.RematchStop();
			GetGame().GetCallqueue().Remove(s_Instance.TakeoverHealth);
			GetGame().GetCallqueue().Remove(s_Instance.PruneVanillaGroups);
			CLW_Rematch.ClearStatics();
		}

		s_Instance = null;
	}

	// Takeover only: what the foreign world has and lacks. Never aborts (a missing piece is logged, the bootstrap goes on).
	protected void CheckHost(string switchName)
	{
		Step("switch " + switchName + ", game mode " + m_Owner.ClassName() + ", world " + GetGame().GetWorldFile(), true);

		FactionManager factionManager = GetGame().GetFactionManager();
		if (factionManager)
		{
			array<Faction> factions = {};
			factionManager.GetFactionsList(factions);
			string factionText;
			foreach (Faction faction : factions)
			{
				SCR_Faction scrFaction = SCR_Faction.Cast(faction);
				string playable = "?";
				if (scrFaction)
					playable = scrFaction.IsPlayable().ToString();
				factionText += faction.GetFactionKey() + "(playable=" + playable + ") ";
			}
			Step("faction manager", !factions.IsEmpty(), factionManager.ClassName() + " " + factionText);
		}
		else
		{
			Step("faction manager", false, "none: no sides can be set up");
		}

		Step("loadout manager", GetGame().GetLoadoutManager() != null);
		Step("ai world", GetGame().GetAIWorld() != null);

		EnsureChat();

		EnsurePerception();
		CheckMapMarkers();

		Step("respawn system", SCR_RespawnSystemComponent.GetInstance() != null, "spawn points now " + SCR_SpawnPoint.CountSpawnPoints().ToString() + " (base points are made by CLW_Bases)");
		Step("commanding manager", SCR_CommandingManagerComponent.GetInstance() != null);
		ReplaceSpawnLogic();
		CLW_PlayerSquad.ApplyCommandingLimit();

		// Game Master modes switch supplies off: arsenals then show no prices and kits are free, the supply link has nothing to sync
		SCR_BaseGameMode hostMode = SCR_BaseGameMode.Cast(m_Owner);
		if (hostMode && !hostMode.IsResourceTypeEnabled(EResourceType.SUPPLIES))
		{
			hostMode.SetResourceTypeEnabled(true, EResourceType.SUPPLIES);
			Step("supplies", hostMode.IsResourceTypeEnabled(EResourceType.SUPPLIES), "were disabled by the host game mode, enabled");
		}
		else
			Step("supplies", true, "already enabled");
	}

	// The chat is reached through GetGame().GetChat() (SCR_ChatPanelManager does the same): when the world has none, spawn the
	// vanilla ScriptedChatEntity our wrapper worlds place (chat commands /request /war ... need it).
	protected void EnsureChat()
	{
		if (ScriptedChatEntity.Cast(GetGame().GetChat()))
		{
			Step("chat entity", true, "present (GetGame().GetChat())");
			return;
		}

		EntitySpawnParams chatParams = new EntitySpawnParams();
		chatParams.TransformMode = ETransformMode.WORLD;
		IEntity chatSpawned = GetGame().SpawnEntityPrefab(Resource.Load(CHAT_PREFAB), GetGame().GetWorld(), chatParams);
		Step("chat entity", ScriptedChatEntity.Cast(GetGame().GetChat()) != null, string.Format("was missing, spawned %1 -> %2", CHAT_PREFAB, chatSpawned));
	}

	// The spawn logic is a prefab attribute of the vanilla respawn system (a modded SCR_MenuSpawnLogic breaks every vanilla prefab that
	// uses it), so the takeover swaps in a CLW_SpawnLogic instance at runtime (CLW_RespawnSwap.c).
	protected void ReplaceSpawnLogic()
	{
		SCR_RespawnSystemComponent respawnSystem = SCR_RespawnSystemComponent.GetInstance();
		if (!respawnSystem)
		{
			Step("spawn logic", false, "no SCR_RespawnSystemComponent: vanilla deploy menu stays");
			return;
		}

		string previousName;
		string reason;
		if (respawnSystem.CLW_ReplaceSpawnLogic(new CLW_SpawnLogic(), previousName, reason))
			Step("spawn logic", true, "replaced " + previousName + " -> CLW_SpawnLogic");
		else
			Step("spawn logic", false, reason + " (previous " + previousName + ": vanilla deploy menu stays)");
	}

	// Our own-side structure / objective markers are static military markers of the game mode's SCR_MapMarkerManagerComponent: it needs a marker
	// config with the PLACED_MILITARY entry (PrepareMilitaryMarker probe), and the player map needs the markers UI (map config of the game mode).
	protected void CheckMapMarkers()
	{
		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		if (!manager)
		{
			Step("map markers", false, "no SCR_MapMarkerManagerComponent on " + m_Owner.ClassName() + ": no structure / objective markers");
			return;
		}

		SCR_MapMarkerConfig config = manager.GetMarkerConfig();
		if (!config)
		{
			Step("map markers", false, "marker manager has no marker config (m_sMarkerCfgPath did not load)");
			return;
		}

		string mapConfig = "no SCR_MapConfigComponent";
		SCR_MapConfigComponent mapConfigComponent = SCR_MapConfigComponent.Cast(m_Owner.FindComponent(SCR_MapConfigComponent));
		if (mapConfigComponent)
			mapConfig = "gadget map config " + mapConfigComponent.GetGadgetMapConfig();

		SCR_MapMarkerEntryMilitary military = SCR_MapMarkerEntryMilitary.Cast(config.GetMarkerEntryConfigByType(SCR_EMapMarkerType.PLACED_MILITARY));
		SCR_MapMarkerBase probe = manager.PrepareMilitaryMarker(EMilitarySymbolIdentity.INDFOR, EMilitarySymbolDimension.INSTALLATION, EMilitarySymbolIcon.MOBILEHQ);
		string detail = string.Format("%1 entry configs, military entry %2, installation marker probe %3, %4 static markers, %5", config.GetMarkerEntryConfigs().Count(), military != null, probe != null, manager.GetStaticMarkers().Count(), mapConfig);
		Step("map markers", military != null && probe != null, detail);
	}

	// AI perception needs a PerceptionManager in the world (Cain's GM world may lack one): spawn the vanilla prefab when none exists.
	protected void EnsurePerception()
	{
		if (GetGame().GetPerceptionManager())
		{
			Step("perception manager", true, "present");
			return;
		}

		EntitySpawnParams spawnParams = new EntitySpawnParams();
		spawnParams.TransformMode = ETransformMode.WORLD;
		IEntity spawned = GetGame().SpawnEntityPrefab(Resource.Load(PERCEPTION_PREFAB), GetGame().GetWorld(), spawnParams);
		Step("perception manager", GetGame().GetPerceptionManager() != null, string.Format("was missing, spawned %1 -> %2", PERCEPTION_PREFAB, spawned));
	}

	// Delayed state line of a takeover: one place to see whether the pieces the player flow needs exist.
	protected void TakeoverHealth()
	{
		string perSide;
		string phase = "no war state";
		if (m_War)
		{
			array<string> sideKeys = {};
			m_War.GetFactionKeys(sideKeys);
			foreach (string sideKey : sideKeys)
			{
				perSide += " " + sideKey + "=" + SCR_SpawnPoint.GetSpawnPointCountForFaction(sideKey).ToString();
			}

			phase = "setup";
			if (m_War.IsRunning())
				phase = "running";
			else if (m_War.IsEnded())
				phase = "ended";
		}

		Step("spawn points after start", SCR_SpawnPoint.CountSpawnPoints() > 0, "total " + SCR_SpawnPoint.CountSpawnPoints().ToString() + ", per side:" + perSide);
		Step("war state", m_War != null, phase);
		Step("joinable squads", true, m_Registry.CountJoinable().ToString());
	}

	// ---- scenario ----

	// Takeover scenario, in order: CLI -clwScenario <name> (Configs/CLW/Scenarios/<name>.conf); the world file name without a leading
	// "GM_" (GM_Arland -> Arland.conf; GM_Eden has none); then the normal chain (by-world, by-listing, auto scenario).
	protected CLW_ScenarioConfig LoadTakeoverScenario()
	{
		CLW_ScenarioConfig scenario;
		string cliName;
		if (System.GetCLIParam(CLI_SCENARIO, cliName) && !cliName.IsEmpty())
		{
			int cliLen = cliName.Length();
			if (cliLen > 5 && cliName.Substring(cliLen - 5, 5) == ".conf")
				cliName = cliName.Substring(0, cliLen - 5);

			ResourceName cliPath = CLW_AutoScenario.SCENARIO_DIR + "/" + cliName + ".conf";
			if (CLW_ScenarioConfig.Peek(cliPath))
			{
				scenario = CLW_ScenarioConfig.Load(cliPath);
				if (scenario)
				{
					Print(string.Format("CLW: scenario %1 (source takeover-cli: %2)", scenario.m_sName, cliPath), LogLevel.NORMAL);
					return scenario;
				}
			}

			Print(string.Format("CLW: takeover: -%1 %2 is not a usable scenario (%3), trying the world name", CLI_SCENARIO, cliName, cliPath), LogLevel.WARNING);
		}

		string worldName = CLW_AutoScenario.WorldName();
		string stripped = worldName;
		string prefix = worldName.Substring(0, Math.Min(3, worldName.Length()));
		prefix.ToUpper();
		if (prefix == "GM_" && worldName.Length() > 3)
			stripped = worldName.Substring(3, worldName.Length() - 3);

		if (stripped != worldName)
		{
			string strippedSource;
			ResourceName strippedFound = CLW_AutoScenario.FindForWorld(stripped, strippedSource);
			if (!strippedFound.IsEmpty())
			{
				scenario = CLW_ScenarioConfig.Load(strippedFound);
				if (scenario)
				{
					Print(string.Format("CLW: scenario %1 (source takeover-%2 '%3': %4)", scenario.m_sName, strippedSource, stripped, strippedFound), LogLevel.NORMAL);
					return scenario;
				}
			}
		}

		return LoadScenario("");
	}

	// Scenario of this world (the choice is logged as "CLW: scenario <name> (source ...)"):
	//   1. attribute  the m_ScenarioConfig of the game mode prefab / world (Arland); empty for a taken-over game mode
	//   2. by-world   Configs/CLW/Scenarios/<world file name without extension>.conf (e.g. CLW_Eden.conf)
	//   3. by-listing a scenario whose m_aWorlds lists the world
	//   4. auto       _Auto.conf, its HQs / objectives filled for this map right now (CLW_AutoScenario)
	protected CLW_ScenarioConfig LoadScenario(ResourceName attribute)
	{
		string worldName = CLW_AutoScenario.WorldName();
		Print(string.Format("CLW: scenario selection: world file '%1' (name '%2'), attribute '%3'", GetGame().GetWorldFile(), worldName, attribute), LogLevel.NORMAL);
		CLW_ScenarioConfig scenario;
		// The game mode PREFAB carries Arland's scenario, so every world inherits the attribute: it only counts when the scenario's world
		// list is empty (legacy) or names this world; otherwise this world gets its own / the auto scenario (Everon got Arland's sea HQs).
		bool useAttribute = !attribute.IsEmpty();
		if (useAttribute)
		{
			CLW_ScenarioConfig peeked = CLW_ScenarioConfig.Peek(attribute);
			if (peeked && peeked.m_aWorlds && !peeked.m_aWorlds.IsEmpty() && !peeked.ListsWorld(worldName))
			{
				Print(string.Format("CLW: scenario attribute %1 is for other worlds, not '%2': ignored", attribute, worldName), LogLevel.NORMAL);
				useAttribute = false;
			}
		}

		if (useAttribute)
		{
			scenario = CLW_ScenarioConfig.Load(attribute);
			if (scenario)
			{
				Print(string.Format("CLW: scenario %1 (source attribute)", scenario.m_sName), LogLevel.NORMAL);
				return scenario;
			}

			Print("CLW: scenario attribute failed, looking for a scenario of this world", LogLevel.WARNING);
		}

		string source;
		ResourceName found = CLW_AutoScenario.FindForWorld(worldName, source);
		if (!found.IsEmpty())
		{
			scenario = CLW_ScenarioConfig.Load(found);
			if (scenario)
			{
				Print(string.Format("CLW: scenario %1 (source %2: %3)", scenario.m_sName, source, found), LogLevel.NORMAL);
				return scenario;
			}

			Print("CLW: scenario " + found + " failed to load, using the auto scenario", LogLevel.WARNING);
		}

		scenario = CLW_AutoScenario.Create(worldName);
		if (scenario)
		{
			Print(string.Format("CLW: scenario %1 (source auto)", scenario.m_sName), LogLevel.NORMAL);
			return scenario;
		}

		Print("CLW: scenario: no scenario for world '" + worldName + "' and the auto scenario failed", LogLevel.ERROR);
		return null;
	}

	// ---- war flow ----

	// War SETUP -> RUNNING: build the force catalogs (factions may have been chosen in the setup) and field the start forces.
	protected void OnWarStarted()
	{
		Print("CLW: war started: building force catalogs and spawning start forces", LogLevel.NORMAL);
		if (m_Catalog)
			m_Catalog.BuildAll();
		CLW_Services.OnWarStart(m_War);	// base services + supply link, before the start forces take their parking spots
		CLW_RequestService.OnWarStart();	// M3 phase Q: report marker parser, request costs to the players
		CLW_FobManager.OnWarStart(m_War);	// M3 phase F: FOB site pool (after the battle layout) + FOB tick
		CLW_Convoys.OnWarStart(m_War);	// M3 phase L: logistics (per-base stock, convoys); needs the FOB manager
		if (m_Spawner)
			m_Spawner.SpawnStartForces();
		CLW_SquadGroups.LockNewGroups();	// HQ squads are the only player groups; the registry registers them in its next sweep

		// the factions are final now: rebuild books, markers and commanders from the scenario's current keys
		BuildFactionObjects();
		CLW_MissionHook.Get().OnWarStarted();
	}

	// (Re)create the per-faction contact book, marker set and commander from the scenario's current faction keys.
	protected void BuildFactionObjects()
	{
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (!scenario)
			return;

		foreach (string oldKey, CLW_ContactMarkers oldMarkers : m_mMarkers)
		{
			if (oldMarkers)
				oldMarkers.RemoveAll();
		}

		m_mBooks.Clear();
		m_mMarkers.Clear();
		m_mCommanders.Clear();
		m_aBuiltKeys.Clear();
		array<string> keys = {};
		scenario.GetFactionKeys(keys);
		foreach (string key : keys)
		{
			CLW_ContactBook book = new CLW_ContactBook();
			CLW_RemoteCommander commander = new CLW_RemoteCommander();
			commander.Init(key, book, m_Registry);
			m_mBooks.Set(key, book);
			m_mMarkers.Set(key, new CLW_ContactMarkers());
			m_mCommanders.Set(key, commander);
			m_aBuiltKeys.Insert(key);
		}

		Print("CLW: faction objects (contact books, markers, commanders) built for " + keys.Count() + " factions", LogLevel.NORMAL);
		Step("faction objects", true, keys.Count().ToString() + " factions");
	}

	// Rebuild when the scenario's faction keys differ from the ones the objects were built for.
	protected void EnsureFactionObjects()
	{
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (!scenario)
			return;

		array<string> keys = {};
		scenario.GetFactionKeys(keys);
		bool same = keys.Count() == m_aBuiltKeys.Count();
		if (same)
		{
			foreach (string key : keys)
			{
				if (!m_aBuiltKeys.Contains(key))
					same = false;
			}
		}

		if (!same)
		{
			Print("CLW: scenario faction keys changed, rebuilding faction objects", LogLevel.NORMAL);
			BuildFactionObjects();
		}
	}

	// ---- game mode hooks (called by CLW_GameModeHooks.c on the hosting entity) ----

	// Respawn gate: a faction without tickets (or a finished war) cannot spawn players.
	bool CanPlayerSpawn(SCR_SpawnRequestComponent requestComponent, out SCR_ESpawnResult result)
	{
		if (!m_War || !requestComponent)
			return true;

		Faction faction = SCR_FactionManager.SGetPlayerFaction(requestComponent.GetPlayerId());
		if (faction && CLW_ScenarioConfig.IsClwFaction(faction.GetFactionKey()) && !m_War.CanFactionSpawn(faction.GetFactionKey()))
		{
			Print(string.Format("CLW: respawn denied for player %1 faction %2 (no tickets / war over)", requestComponent.GetPlayerId(), faction.GetFactionKey()), LogLevel.NORMAL);
			result = SCR_ESpawnResult.SPAWN_NOT_ALLOWED;
			return false;
		}

		return true;
	}

	// Kill accounting: every destroyed controllable (AI, player, vehicle) costs its faction tickets; players: the war state
	// de-duplicates against the controllable event by entity.
	void OnEntityDestroyed(notnull SCR_InstigatorContextData instigatorContextData)
	{
		if (m_War && Replication.IsServer())
			m_War.OnEntityDestroyed(instigatorContextData.GetVictimEntity(), instigatorContextData.GetVictimPlayerID());
	}

	// Player flow: after the spawn (auto in SETUP, deploy menu later) the player is in an HQ squad (menu choice) or gets the fallback
	// auto-join into the AI rifle squad of their faction.
	void OnPlayerSpawnFinalize(IEntity entity)
	{
		int playerId = GetGame().GetPlayerManager().GetPlayerIdFromControlledEntity(entity);
		Faction faction = SCR_FactionManager.SGetPlayerFaction(playerId);
		Print(string.Format("CLW: player %1 spawned faction=%2 at %3", playerId, faction, entity.GetOrigin()), LogLevel.NORMAL);
		if (m_War)
			m_War.OnHumanPlayerSpawned(playerId);
		if (!faction)
			return;

		CLW_SquadGroups.MirrorAll(playerId);	// idempotent safety net for clients that connected before the squads existed
		CLW_RequestService.SendCosts(playerId);	// M3 phase Q: request costs for the terminal action labels
		TryJoinPlayer(playerId, faction.GetFactionKey(), 0);
	}

	// A client connected: tell it which HQ squads are in the vanilla group lists (after its controller is up).
	protected void OnClwPlayerRegistered(int playerId)
	{
		GetGame().GetCallqueue().CallLater(CLW_SquadGroups.MirrorAll, 3000, false, playerId);
	}

	protected void PruneVanillaGroups()
	{
		if (m_Registry)
			CLW_SquadGroups.PruneVanillaGroups(m_Registry);
	}

	protected void SayBuild()
	{
		CLW_Debug.Say("build " + CLW_Build.STAMP);
	}

	// Perception -> contact book -> map markers, for both factions.
	protected void PollContacts()
	{
		if (!m_Owner)
		{
			GetGame().GetCallqueue().Remove(PollContacts);
			return;
		}

		EnsureFactionObjects();
		foreach (string key : m_aBuiltKeys)
		{
			UpdateFaction(key, m_mBooks.Get(key), m_mMarkers.Get(key));
		}
	}

	protected void UpdateFaction(string factionKey, CLW_ContactBook book, CLW_ContactMarkers markers)
	{
		if (!book || !markers)
			return;

		array<string> squads = {};
		m_Registry.GetIds(factionKey, squads);
		foreach (string name : squads)
		{
			SCR_AIGroup group = m_Registry.Find(name);
			if (!group)
				continue;

			array<ref CLW_Contact> sightings = {};
			CLW_Contact.FromGroup(group, name, sightings);
			book.Report(factionKey, sightings);

			vector leaderPos;
			IEntity squadLeader = CLW_SquadInfo.LeaderEntity(group);
			if (squadLeader)
				leaderPos = squadLeader.GetOrigin();
			array<IEntity> members = CLW_SquadInfo.Members(group);
			float maxDist;
			foreach (IEntity member : members)
			{
				maxDist = Math.Max(maxDist, vector.Distance(leaderPos, member.GetOrigin()));
			}
			Print(string.Format("CLW: squad %1 leader=%2 players=%3 firstPlayerLeader=%4 humanLeader=%5 leaderId=%6 waypoint=%7", name, leaderPos, group.GetPlayerCount(), group.GetFirstPlayerLeaderID(), CLW_RuleCommander.HasHumanLeader(group), group.GetLeaderID(), group.GetCurrentWaypoint() != null), LogLevel.NORMAL);
			SCR_AIGroup slave = group.GetSlave();
			if (slave)
			{
				vector slaveWp;
				AIWaypoint slaveCurrent = slave.GetCurrentWaypoint();
				if (slaveCurrent)
					slaveWp = slaveCurrent.GetOrigin();
				Print(string.Format("CLW: squad %1 slave=%2 slaveAgents=%3 slaveWaypoint=%4 slaveWpPos=%5 groupId=%6", name, slave, slave.GetAgentsCount(), slaveCurrent != null, slaveWp, group.GetGroupID()), LogLevel.NORMAL);
			}
			else
			{
				Print(string.Format("CLW: squad %1 slave=none groupId=%2", name, group.GetGroupID()), LogLevel.NORMAL);
			}
			PrintFormat("CLW: squad %1 agents=%2 alive=%3 aiMembers=%4 maxDist=%5", name, group.GetAgentsCount(), CLW_SquadInfo.AliveCount(group), members.Count(), Math.Round(maxDist), level: LogLevel.NORMAL);
			if (CLW_SquadInfo.AliveCount(group) <= 2)
				CLW_SquadInfo.LogMembers(name, group);
		}

		array<int> removed = {};
		book.Expire(removed);
		markers.Remove(removed);

		array<CLW_Contact> known = {};
		book.GetContacts(known);
		markers.Sync(factionKey, known);
		Print(string.Format("CLW: %1 book contacts=%2 removed=%3", factionKey, known.Count(), removed.Count()), LogLevel.NORMAL);
		foreach (CLW_Contact contact : known)
		{
			Print("CLW: contact " + contact.Describe(), LogLevel.NORMAL);
		}
	}

	protected void ThinkCommanders()
	{
		if (!m_Owner)
		{
			GetGame().GetCallqueue().Remove(ThinkCommanders);
			return;
		}

		EnsureFactionObjects();
		foreach (string key : m_aBuiltKeys)
		{
			CLW_RemoteCommander commander = m_mCommanders.Get(key);
			if (commander)
				commander.Think();
		}
	}

	// First adopted infantry squad of the faction that is led by a human (more players join it), else the first free one.
	// The scenario's preferred squad name wins when it is adopted and of this faction.
	protected SCR_AIGroup FindStartSquad(string factionKey)
	{
		m_Registry.Sweep();
		string preferredSquad;
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (scenario)
		{
			CLW_FactionSetup factionSetup = scenario.FindFaction(factionKey);
			if (factionSetup)
				preferredSquad = factionSetup.m_sPreferredSquad;
		}

		if (!preferredSquad.IsEmpty())
		{
			SCR_AIGroup preferred = m_Registry.Find(preferredSquad);
			if (preferred && CLW_SquadRegistry.GetFactionKey(preferred) == factionKey && !preferred.IsFull())
				return preferred;
		}

		array<string> ids = {};
		m_Registry.GetIds(factionKey, ids);
		SCR_AIGroup firstFree;
		foreach (string id : ids)
		{
			SCR_AIGroup group = m_Registry.Find(id);
			if (!group || !m_Registry.IsJoinable(group) || group.IsFull())
				continue;

			if (CLW_SquadInfo.HumanLeaderEntity(group))
				return group;

			if (!firstFree)
				firstFree = group;
		}

		return firstFree;
	}

	// Join the player to a squad; while there is none (start forces come with the war start) retry every 5 s for 2 min, then spawn
	// a free infantry squad of the faction and join that. Only counts attempts while the war is running (the setup phase keeps the
	// player alone at the base).
	protected void TryJoinPlayer(int playerId, string factionKey, int attempt)
	{
		if (!GetGame().GetPlayerManager().GetPlayerControlledEntity(playerId))
			return;	// player gone / dead: the next spawn starts a new search

		Faction current = SCR_FactionManager.SGetPlayerFaction(playerId);
		if (current)
			factionKey = current.GetFactionKey();

		bool running = m_War && m_War.IsRunning();
		SCR_AIGroup squad;
		if (running)
		{
			// entry point 1/2: the player already picked an HQ squad (deploy menu / groups menu): the hooks establish it, we only
			// make sure (idempotent). Anything else (no group, a vanilla group) = fallback auto-join below.
			SCR_GroupsManagerComponent groups = SCR_GroupsManagerComponent.GetInstance();
			SCR_AIGroup chosen;
			if (groups)
				chosen = groups.GetPlayerGroup(playerId);
			if (chosen && m_Registry.IsJoinable(chosen))
			{
				Print(string.Format("CLW: player %1 is in HQ squad %2 (chosen): establishing", playerId, chosen), LogLevel.NORMAL);
				CLW_PlayerSquad.Establish(chosen, playerId);
				return;
			}

			squad = FindStartSquad(factionKey);
		}

		if (squad)
		{
			Print(string.Format("CLW: player %1 joins squad %2 (after %3 retries)", playerId, squad, attempt), LogLevel.NORMAL);
			CLW_PlayerSquad.Join(squad, playerId);
			Print(string.Format("CLW: player %1 in squad=%2 players=%3 leaderId=%4", playerId, squad.IsPlayerInGroup(playerId), squad.GetPlayerCount(), squad.GetLeaderID()), LogLevel.NORMAL);
			return;
		}

		if (!running)
		{
			if (attempt == 0)
				Print(string.Format("CLW: player %1 waits for the war start to get a squad (setup: alone at the base)", playerId), LogLevel.NORMAL);
			GetGame().GetCallqueue().CallLater(TryJoinPlayer, SQUAD_RETRY_MS, false, playerId, factionKey, attempt);
			return;
		}

		attempt++;
		if (attempt == SQUAD_RETRY_MAX && m_Spawner && m_Catalog)
		{
			CLW_ForceEntry best;
			array<CLW_ForceEntry> entries = {};
			m_Catalog.GetEntries(factionKey, entries);
			foreach (CLW_ForceEntry entry : entries)
			{
				if (entry.m_sKind != CLW_ForceKind.INFANTRY || entry.HasVehicle())
					continue;

				if (!best || entry.m_iGroupMembers > best.m_iGroupMembers)
					best = entry;
			}

			string error;
			if (best && m_Spawner.SpawnFree(factionKey, best.m_sType, "player squad (none available)", error))
				Print(string.Format("CLW: player %1 got no squad in %2 retries: spawning a free %3", playerId, attempt, best.m_sType), LogLevel.WARNING);
			else
				Print(string.Format("CLW: player %1 got no squad and no infantry entry could be spawned: %2", playerId, error), LogLevel.ERROR);
		}
		else if (attempt > SQUAD_RETRY_MAX + SQUAD_RETRY_EXTRA)
		{
			Print(string.Format("CLW: player %1 gave up waiting for a squad", playerId), LogLevel.ERROR);
			return;
		}

		GetGame().GetCallqueue().CallLater(TryJoinPlayer, SQUAD_RETRY_MS, false, playerId, factionKey, attempt);
	}

	// ---- M0 spike (off) ----

	// M0 spike: squads spawned, then a canned MOVE order -> waypoint, then check the squad moved.
	protected void SpikeOrderLoop()
	{
		LogSquad("CLW_Squad_US_1");
		LogSquad("CLW_Squad_USSR_1");

		SCR_AIGroup group = FindSquad("CLW_Squad_US_1");
		if (!group || !group.GetLeaderEntity())
			return;

		// Advance on the enemy squad to about 100 m short of it, so perception has something to see.
		vector target = group.GetLeaderEntity().GetOrigin() + Vector(0, 0, -300);
		SCR_AIGroup enemy = FindSquad("CLW_Squad_USSR_1");
		if (enemy && enemy.GetLeaderEntity())
		{
			vector toEnemy = enemy.GetLeaderEntity().GetOrigin() - group.GetLeaderEntity().GetOrigin();
			toEnemy[1] = 0;
			target = enemy.GetLeaderEntity().GetOrigin() - toEnemy.Normalized() * 100;
		}
		target[1] = GetGame().GetWorld().GetSurfaceY(target[0], target[2]);
		if (IssueMove(group, target))
			Print("CLW: MOVE issued to CLW_Squad_US_1 -> " + target.ToString(), LogLevel.NORMAL);

		LogOrderState(group);
		foreach (int delay : {5000, 15000, 30000, 60000})
		{
			GetGame().GetCallqueue().CallLater(LogOrderState, delay, false, group);
		}
	}

	protected void LogOrderState(SCR_AIGroup group)
	{
		array<AIWaypoint> waypoints = {};
		group.GetWaypoints(waypoints);
		AIWaypoint current = group.GetCurrentWaypoint();
		vector leaderPos;
		if (group.GetLeaderEntity())
			leaderPos = group.GetLeaderEntity().GetOrigin();

		Print(string.Format("CLW: order state waypoints=%1 current=%2 leader=%3", waypoints.Count(), current, leaderPos), LogLevel.NORMAL);
	}

	protected bool IssueMove(SCR_AIGroup group, vector target)
	{
		EntitySpawnParams params = new EntitySpawnParams();
		params.TransformMode = ETransformMode.WORLD;
		params.Transform[3] = target;

		AIWaypoint waypoint = AIWaypoint.Cast(GetGame().SpawnEntityPrefab(Resource.Load(CLW_WarConfig.WAYPOINT_MOVE), GetGame().GetWorld(), params));
		if (!waypoint)
		{
			Print("CLW: failed to spawn move waypoint", LogLevel.ERROR);
			return false;
		}

		group.AddWaypoint(waypoint);
		return true;
	}

	protected SCR_AIGroup FindSquad(string name)
	{
		return SCR_AIGroup.Cast(GetGame().GetWorld().FindEntityByName(name));
	}

	protected void LogSquad(string name)
	{
		SCR_AIGroup group = FindSquad(name);
		if (!group)
		{
			Print("CLW: squad " + name + " not found", LogLevel.WARNING);
			return;
		}

		vector leaderPos;
		if (group.GetLeaderEntity())
			leaderPos = group.GetLeaderEntity().GetOrigin();

		Print(string.Format("CLW: squad %1 faction=%2 agents=%3 leader=%4", name, group.GetFaction().GetFactionKey(), group.GetAgentsCount(), leaderPos), LogLevel.NORMAL);
	}
}
