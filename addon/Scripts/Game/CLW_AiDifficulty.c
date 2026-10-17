// AI difficulty per side (M4.6, spec 13b). Server only, self-contained: no spawner needs to know about it.
// Setup / /war start / overrides.json "setup" parameters aiDifficultyA / aiDifficultyB (easy | normal | hard | veteran, default normal).
// A server sweep every SWEEP_MS walks AIWorld.GetAIAgents; each new character agent of a side is seen once, and on the NEXT sweep (so the
// character's init frame has run: SCR_AICombatComponent.EOnInit resets the skill to the prefab default and SetPerceptionFactor needs
// the component's utility) the side's preset is applied once: SCR_AICombatComponent.SetAISkill + SetPerceptionFactor.
// "normal" is vanilla behaviour: the agent is not touched at all. Research: dev/research/ai-difficulty.md.
// Log: "CLW: ai difficulty <side> <preset> applied to N new agents" (one line per sweep and side that applied anything).
class CLW_AiDifficulty
{
	static const string PARAM_A = "aiDifficultyA";
	static const string PARAM_B = "aiDifficultyB";
	static const string PARAM_BOTH = "aiDifficulty";		// shortcut key of "/war start" and the overrides.json setup: both sides
	static const string EASY = "easy";
	static const string NORMAL = "normal";
	static const string HARD = "hard";
	static const string VETERAN = "veteran";
	static const int SWEEP_MS = 4000;

	protected static CLW_WarState s_War;
	protected static ref array<AIAgent> s_aPending = {};	// seen once, applied on the next sweep (weak: dead agents turn null)
	protected static ref array<AIAgent> s_aDone = {};		// applied or not applicable
	protected static ref array<string> s_aLoggedOff = {};	// sides whose "normal = vanilla" line was logged

	// Registers the two setup parameters (called by the war mode that offers them).
	static void AddParams(notnull array<ref CLW_ModeParam> params)
	{
		array<string> choices = {EASY, NORMAL, HARD, VETERAN};
		params.Insert(CLW_ModeParam.CreateChoice(PARAM_A, "#CLW-Param_aiDifficultyA", "#CLW-Param_aiDifficultyA_Desc", choices, NORMAL));
		params.Insert(CLW_ModeParam.CreateChoice(PARAM_B, "#CLW-Param_aiDifficultyB", "#CLW-Param_aiDifficultyB_Desc", choices, NORMAL));
	}

	// Skill and perception factor of a preset. False for "normal" and unknown names (nothing to apply).
	static bool GetPreset(string name, out EAISkill skill, out float perception)
	{
		if (name == EASY)
		{
			skill = EAISkill.ROOKIE;
			perception = 0.6;
			return true;
		}

		if (name == HARD)
		{
			skill = EAISkill.VETERAN;
			perception = 1.3;
			return true;
		}

		if (name == VETERAN)
		{
			skill = EAISkill.EXPERT;
			perception = 1.6;
			return true;
		}

		return false;
	}

	// War SETUP -> RUNNING (CLW_WarRuntime.OnWarStarted, before the start forces spawn).
	static void OnWarStart(CLW_WarState war)
	{
		if (!Replication.IsServer() || !war)
			return;

		Stop();
		s_War = war;
		CLW_WarMode mode = war.GetMode();
		if (mode)
			Print(string.Format("CLW: ai difficulty: side A '%1', side B '%2' (sweep every %3 ms)", mode.GetChoiceParam(PARAM_A), mode.GetChoiceParam(PARAM_B), SWEEP_MS), LogLevel.NORMAL);
		GetGame().GetCallqueue().CallLater(Sweep, SWEEP_MS, true);
	}

	// Rematch / game end (CLW_WarRuntime.RematchStop): forget everything, the next war starts clean.
	static void Stop()
	{
		GetGame().GetCallqueue().Remove(Sweep);
		s_War = null;
		s_aPending.Clear();
		s_aDone.Clear();
		s_aLoggedOff.Clear();
	}

	protected static void Sweep()
	{
		if (!s_War || !s_War.IsRunning())
			return;

		CLW_WarMode mode = s_War.GetMode();
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		AIWorld aiWorld = GetGame().GetAIWorld();
		if (!mode || !scenario || !aiWorld)
			return;

		Prune(s_aPending);
		Prune(s_aDone);

		array<AIAgent> agents = {};
		aiWorld.GetAIAgents(agents);
		array<AIAgent> nextPending = {};
		array<int> applied = {0, 0};
		foreach (AIAgent agent : agents)
		{
			if (!agent || s_aDone.Contains(agent))
				continue;

			if (!s_aPending.Contains(agent))
			{
				nextPending.Insert(agent);		// first sighting: wait one sweep
				continue;
			}

			s_aDone.Insert(agent);
			int side = ApplyTo(agent, mode, scenario);
			if (side >= 0 && side < 2)
				applied[side] = applied[side] + 1;
		}

		s_aPending.Clear();
		foreach (AIAgent waiting : nextPending)
		{
			s_aPending.Insert(waiting);
		}

		for (int i = 0; i < 2; i++)
		{
			if (applied[i] > 0)
				Print(string.Format("CLW: ai difficulty %1 %2 applied to %3 new agents", scenario.GetFactionKey(i), PresetName(mode, i), applied[i]), LogLevel.NORMAL);
		}
	}

	protected static string PresetName(CLW_WarMode mode, int side)
	{
		if (side == 0)
			return mode.GetChoiceParam(PARAM_A);

		return mode.GetChoiceParam(PARAM_B);
	}

	// Applies the preset of the agent's side. Returns the side index when something was applied, else -1.
	protected static int ApplyTo(AIAgent agent, CLW_WarMode mode, CLW_ScenarioConfig scenario)
	{
		IEntity entity = agent.GetControlledEntity();
		if (!entity)
			return -1;		// a group agent (no body)

		PlayerManager players = GetGame().GetPlayerManager();
		if (players && players.GetPlayerIdFromControlledEntity(entity) > 0)
			return -1;

		FactionAffiliationComponent affiliation = FactionAffiliationComponent.Cast(entity.FindComponent(FactionAffiliationComponent));
		if (!affiliation)
			return -1;

		Faction faction = affiliation.GetAffiliatedFaction();
		if (!faction)
			return -1;

		int side = scenario.GetSideIndex(faction.GetFactionKey());
		if (side < 0 || side > 1)
			return -1;

		string name = PresetName(mode, side);
		EAISkill skill;
		float perception;
		if (!GetPreset(name, skill, perception))
		{
			string sideKey = scenario.GetFactionKey(side);
			if (!s_aLoggedOff.Contains(sideKey))
			{
				s_aLoggedOff.Insert(sideKey);
				Print(string.Format("CLW: ai difficulty %1 '%2': vanilla skill and perception, agents are not touched", sideKey, name), LogLevel.NORMAL);
			}
			return -1;
		}

		SCR_AICombatComponent combat = SCR_AICombatComponent.Cast(entity.FindComponent(SCR_AICombatComponent));
		if (!combat)
			return -1;

		combat.SetAISkill(skill);
		combat.SetPerceptionFactor(perception);
		return side;
	}

	protected static void Prune(notnull array<AIAgent> list)
	{
		for (int i = list.Count() - 1; i >= 0; i--)
		{
			if (!list[i])
				list.Remove(i);
		}
	}
}
