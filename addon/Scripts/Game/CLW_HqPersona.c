// HQ character traits (design: dev/notes/hq-comms-env-traits-design.md section 3). Each side's HQ gets one trait at war start:
// chosen in the setup (params hqTraitA / hqTraitB, side A / B = the war's factionA / factionB) or rolled at random (logged, never told to
// the enemy). Effects: the rule commander tuning (CLW_HqPersonaTuning, below), and the situation key "hq" ({"trait":"aggressive"}) that the
// sidecar turns into personality text for the LLM commander, the SITREP tone and the /hq radio style (dev/tools/agent/personality.mjs).
// Trait key "" = no trait: neutral tuning, exactly the doctrine constants of CLW_RuleCommander. Server only (reads the war state).
//
// Hookups (the war mode constructor / OnWarStart / situation builder call these):
//   CLW_HqPersona.AddParams(m_aParams);   CLW_HqPersona.OnWarStart();   CLW_HqPersona.GetJson(faction) -> situation key "hq".

// Tuning of the rule commander for one trait: multipliers on the doctrine constants. Every value is bounded (multipliers 0.6 .. 1.6, integer
// caps >= 1) so no trait can break the commander. All 1 / 0 = today's behaviour.
class CLW_HqPersonaTuning
{
	string m_sTrait;				// "" = neutral
	float m_fAttackRadius = 1;		// x IDLE_ATTACK_RADIUS_M: how far an idle unit chases a contact
	float m_fThreatRadius = 1;		// x THREAT_RADIUS_M: how close an enemy contact must be to threaten an own point
	float m_fMaxAttack = 1;			// x MAX_ATTACK_UNITS (result >= 1)
	float m_fMaxDefend = 1;			// x MAX_DEFEND_UNITS (result >= 1)
	float m_fReserve = 1;			// x the supply floor (max of SUPPLY_RESERVE and the hqReserve param)
	int m_iAttackBias;				// added to the priority of attack tasks (base 500 - rank * 10; defend 1000+, recon 600, buy floor 200)
	float m_fBoldChance;			// crazy: chance per roll to pick a far "bold strike" target
	float m_fBoldHoldSec;			// seconds between rolls and how long a bold target is kept

	static const float MULT_MIN = 0.6;
	static const float MULT_MAX = 1.6;
	static const int BIAS_MAX = 450;

	// Neutral tuning for trait "" and for unknown keys.
	static CLW_HqPersonaTuning ForTrait(string trait)
	{
		CLW_HqPersonaTuning t = new CLW_HqPersonaTuning();
		if (trait == "aggressive")
		{
			t.m_fAttackRadius = 1.5;
			t.m_fThreatRadius = 0.8;
			t.m_fMaxAttack = 1.34;
			t.m_fMaxDefend = 0.67;
			t.m_fReserve = 0.6;
			t.m_iAttackBias = 400;
		}
		else if (trait == "defensive")
		{
			t.m_fAttackRadius = 0.8;
			t.m_fThreatRadius = 1.4;
			t.m_fMaxAttack = 0.67;
			t.m_fMaxDefend = 1.5;
			t.m_fReserve = 1.4;
			t.m_iAttackBias = -150;
		}
		else if (trait == "methodical")
		{
			t.m_fAttackRadius = 0.9;
			t.m_fThreatRadius = 1.1;
			t.m_fMaxAttack = 1.34;
			t.m_fReserve = 1.2;
		}
		else if (trait == "cautious")
		{
			t.m_fAttackRadius = 0.7;
			t.m_fThreatRadius = 1.3;
			t.m_fMaxAttack = 0.67;
			t.m_fMaxDefend = 1.5;
			t.m_fReserve = 1.6;
			t.m_iAttackBias = -250;
		}
		else if (trait == "opportunistic")
		{
			t.m_fAttackRadius = 1.6;
			t.m_fMaxDefend = 0.67;
			t.m_fReserve = 0.8;
			t.m_iAttackBias = 150;
		}
		else if (trait == "crazy")
		{
			t.m_fAttackRadius = 1.4;
			t.m_fThreatRadius = 0.8;
			t.m_fMaxAttack = 1.34;
			t.m_fMaxDefend = 0.67;
			t.m_fReserve = 0.6;
			t.m_fBoldChance = 0.35;
			t.m_fBoldHoldSec = 240;
		}
		else
		{
			return t;	// neutral (m_sTrait stays "")
		}

		t.m_sTrait = trait;
		t.Bound();
		return t;
	}

	protected void Bound()
	{
		m_fAttackRadius = Math.Clamp(m_fAttackRadius, MULT_MIN, MULT_MAX);
		m_fThreatRadius = Math.Clamp(m_fThreatRadius, MULT_MIN, MULT_MAX);
		m_fMaxAttack = Math.Clamp(m_fMaxAttack, MULT_MIN, MULT_MAX);
		m_fMaxDefend = Math.Clamp(m_fMaxDefend, MULT_MIN, MULT_MAX);
		m_fReserve = Math.Clamp(m_fReserve, MULT_MIN, MULT_MAX);
		m_iAttackBias = Math.Clamp(m_iAttackBias, -BIAS_MAX, BIAS_MAX);
		m_fBoldChance = Math.Clamp(m_fBoldChance, 0, 0.5);
		if (m_fBoldChance > 0)
			m_fBoldHoldSec = Math.Clamp(m_fBoldHoldSec, 120, 600);
	}

	bool IsNeutral()
	{
		return m_sTrait.IsEmpty();
	}

	float AttackRadius(float baseRadius)
	{
		return baseRadius * m_fAttackRadius;
	}

	float ThreatRadius(float baseRadius)
	{
		return baseRadius * m_fThreatRadius;
	}

	int MaxAttack(int baseUnits)
	{
		int units = Math.Round(baseUnits * m_fMaxAttack);
		return Math.Max(1, units);
	}

	int MaxDefend(int baseUnits)
	{
		int units = Math.Round(baseUnits * m_fMaxDefend);
		return Math.Max(1, units);
	}

	int Reserve(int baseFloor)
	{
		int floorSupplies = Math.Round(baseFloor * m_fReserve);
		return Math.Max(0, floorSupplies);
	}

	int AttackPriority(int basePriority)
	{
		return basePriority + m_iAttackBias;
	}

	string Describe()
	{
		string text = string.Format("trait '%1': attackRadius x%2, threatRadius x%3, maxAttack x%4, maxDefend x%5", m_sTrait, m_fAttackRadius, m_fThreatRadius, m_fMaxAttack, m_fMaxDefend);
		text += string.Format(", reserve x%1, attackPriority %2", m_fReserve, m_iAttackBias);
		if (m_fBoldChance > 0)
			text += string.Format(", bold strike chance %1 per %2 s", m_fBoldChance, m_fBoldHoldSec);
		return text;
	}
}

class CLW_HqPersona
{
	static const string PARAM_A = "hqTraitA";
	static const string PARAM_B = "hqTraitB";
	static const string RANDOM = "random";

	protected static ref map<string, string> s_mTrait = new map<string, string>();	// faction key -> trait key
	protected static ref map<string, ref CLW_HqPersonaTuning> s_mTuning = new map<string, ref CLW_HqPersonaTuning>();
	protected static ref CLW_HqPersonaTuning s_Neutral = new CLW_HqPersonaTuning();

	// The concrete traits (without "random").
	static void GetTraits(notnull array<string> outTraits)
	{
		outTraits.Insert("aggressive");
		outTraits.Insert("defensive");
		outTraits.Insert("methodical");
		outTraits.Insert("cautious");
		outTraits.Insert("opportunistic");
		outTraits.Insert("crazy");
	}

	static bool IsTrait(string trait)
	{
		array<string> traits = {};
		GetTraits(traits);
		return traits.Contains(trait);
	}

	// Declares the two choice params on a war mode (call from the mode constructor): random + the traits, default random.
	static void AddParams(notnull array<ref CLW_ModeParam> params)
	{
		array<string> choices = {};
		choices.Insert(RANDOM);
		GetTraits(choices);
		params.Insert(CLW_ModeParam.CreateChoice(PARAM_A, "#CLW-Param_hqTraitA", "#CLW-Param_hqTraitA_Desc", choices, RANDOM));
		params.Insert(CLW_ModeParam.CreateChoice(PARAM_B, "#CLW-Param_hqTraitB", "#CLW-Param_hqTraitB_Desc", choices, RANDOM));
	}

	// Server, at war start (after the sides are applied, so the war's faction keys are final): resolve side A / B to the factions,
	// roll the random ones, log, and tell each side's players its own commander's character. Never tells the enemy.
	static void OnWarStart()
	{
		if (!Replication.IsServer())
			return;

		s_mTrait.Clear();
		s_mTuning.Clear();
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war || !war.GetMode())
			return;

		CLW_WarMode mode = war.GetMode();
		array<string> keys = {};
		war.GetFactionKeys(keys);
		array<string> traits = {};
		GetTraits(traits);
		for (int side = 0; side < keys.Count() && side < 2; side++)
		{
			string faction = keys[side];
			string paramId = PARAM_A;
			if (side == 1)
				paramId = PARAM_B;

			string trait = mode.GetChoiceParam(paramId);
			string how = "chosen";
			if (!traits.Contains(trait))
			{
				trait = traits[Math.RandomInt(0, traits.Count())];
				how = "random";
			}

			s_mTrait.Set(faction, trait);
			s_mTuning.Set(faction, CLW_HqPersonaTuning.ForTrait(trait));
			Print(string.Format("CLW: persona %1 = %2 (%3)", faction, trait, how), LogLevel.NORMAL);
			TellFaction(faction, CLW_Loc.W("#CLW-Persona_Told", "#CLW-Trait_" + trait));
		}
	}

	// Trait key of the faction's HQ, "" when none (no war, unknown faction).
	static string Trait(string faction)
	{
		string trait;
		if (!s_mTrait.Find(faction, trait))
			return "";

		return trait;
	}

	// Situation key "hq": {"trait":"aggressive"}, "" without a trait.
	static string GetJson(string faction)
	{
		string trait = Trait(faction);
		if (trait.IsEmpty())
			return "";

		return "{\"trait\":\"" + CLW_Situation.Escape(trait) + "\"}";
	}

	// Rule commander tuning of the faction; never null (neutral without a trait).
	static CLW_HqPersonaTuning Tuning(string faction)
	{
		CLW_HqPersonaTuning tuning;
		if (!s_mTuning.Find(faction, tuning) || !tuning)
			return s_Neutral;

		return tuning;
	}

	// One localised line (wire) to every connected player of the faction (faction-scoped, unlike CLW_Debug.Announce).
	// Players who join later are not told.
	protected static void TellFaction(string faction, string wire)
	{
		PlayerManager players = GetGame().GetPlayerManager();
		if (!players)
			return;

		array<int> ids = {};
		players.GetPlayers(ids);
		foreach (int id : ids)
		{
			if (CLW_RequestService.FactionKeyOf(id) != faction)
				continue;

			if (id == SCR_PlayerController.GetLocalPlayerId())
			{
				CLW_Debug.Show(wire);	// listen server host: no RPC needed
				continue;
			}

			SCR_PlayerController controller = SCR_PlayerController.Cast(players.GetPlayerController(id));
			if (controller)
				controller.CLW_RpcShow(wire);
		}
	}
}
