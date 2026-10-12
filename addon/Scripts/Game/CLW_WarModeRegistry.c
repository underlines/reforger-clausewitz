// Maps scenario mode config classes to mode classes. Add a mode: new config class + new mode class + one line in Create.
class CLW_WarModeRegistry
{
	static CLW_WarMode Create(CLW_WarModeConfig config)
	{
		if (!config)
			return null;

		CLW_WarModeConfig_Short shortConfig = CLW_WarModeConfig_Short.Cast(config);
		if (shortConfig)
			return new CLW_WarMode_ShortConflict(shortConfig);

		Print("CLW: war mode config has no mode class: " + config.Type().ToString(), LogLevel.ERROR);
		return null;
	}

	// All modes of the scenario (in scenario order).
	static void CreateAll(notnull CLW_ScenarioConfig scenario, notnull array<ref CLW_WarMode> outModes)
	{
		if (!scenario.m_aModes)
			return;

		foreach (CLW_WarModeConfig config : scenario.m_aModes)
		{
			CLW_WarMode mode = Create(config);
			if (mode)
				outModes.Insert(mode);
		}
	}
}
