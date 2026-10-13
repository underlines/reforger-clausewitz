// Friendly units on the player's map: the four on/off switches (war-mode params, setup menu, /war start key=value,
// overrides.json "setup"). Server side reads Mask() and ships it to clients inside every picture (CLW_FriendlyPicture).
// Design: dev/notes/map-friendly-design.md.
class CLW_FriendlyMapSettings
{
	static const string KEY_ME = "mapMe";
	static const string KEY_SQUADMATES = "mapSquadmates";
	static const string KEY_SQUADS = "mapSquads";
	static const string KEY_VEHICLES = "mapVehicles";

	static const int BIT_ME = 1;
	static const int BIT_SQUADMATES = 2;
	static const int BIT_SQUADS = 4;
	static const int BIT_VEHICLES = 8;

	// Declares the four bool params on a war mode (call from the mode constructor).
	static void AddParams(notnull array<ref CLW_ModeParam> params)
	{
		params.Insert(CLW_ModeParam.CreateBool(KEY_ME, "#CLW-Param_mapMe", "#CLW-Param_mapMe_Desc", true));
		params.Insert(CLW_ModeParam.CreateBool(KEY_SQUADMATES, "#CLW-Param_mapSquadmates", "#CLW-Param_mapSquadmates_Desc", true));
		params.Insert(CLW_ModeParam.CreateBool(KEY_SQUADS, "#CLW-Param_mapSquads", "#CLW-Param_mapSquads_Desc", false));
		params.Insert(CLW_ModeParam.CreateBool(KEY_VEHICLES, "#CLW-Param_mapVehicles", "#CLW-Param_mapVehicles_Desc", false));
	}

	// Bit mask of the switches that are on; 0 when no war or no mode.
	static int Mask()
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war || !war.GetMode())
			return 0;

		CLW_WarMode mode = war.GetMode();
		int mask = 0;
		if (mode.GetIntParam(KEY_ME, 1) != 0)
			mask = mask | BIT_ME;
		if (mode.GetIntParam(KEY_SQUADMATES, 1) != 0)
			mask = mask | BIT_SQUADMATES;
		if (mode.GetIntParam(KEY_SQUADS, 0) != 0)
			mask = mask | BIT_SQUADS;
		if (mode.GetIntParam(KEY_VEHICLES, 0) != 0)
			mask = mask | BIT_VEHICLES;
		return mask;
	}

	static bool Has(int bit)
	{
		return (Mask() & bit) != 0;
	}
}
