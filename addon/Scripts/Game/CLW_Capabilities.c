// War-mode capabilities (M3 design section 1): a mode declares a set of capability names (CLW_WarMode.GetCapabilities), a bool mode
// param "cap_<name>" (when the mode defines one) switches a declared capability off in the setup menu. Everything that depends on a
// capability asks here; action registry entries whose `requires` are unmet drop out of the manifest and the validator.
// Names in use: services, fob, logistics, requests, reports, kitCost. Free-form strings, new modes/phases add their own.
class CLW_Capabilities
{
	// True when the active war mode declares `name` and it is not switched off. False when no war mode is active.
	static bool Has(string name)
	{
		CLW_WarMode mode = ActiveMode();
		if (!mode)
			return false;

		return mode.HasCapability(name);
	}

	// All capabilities that are currently on (declared and not switched off). Appends to outNames; nothing without a war mode.
	static void GetAll(notnull array<string> outNames)
	{
		CLW_WarMode mode = ActiveMode();
		if (!mode)
			return;

		mode.GetActiveCapabilities(outNames);
	}

	protected static CLW_WarMode ActiveMode()
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war)
			return null;

		return war.GetMode();
	}
}
