// Shared command set of the commanders (spec section 6). Order target = an objective position or any position.
enum CLW_ECommand
{
	MOVE,
	ATTACK,
	DEFEND,
	RECON,
	FALL_BACK
}

class CLW_Command
{
	static string ToName(CLW_ECommand command)
	{
		switch (command)
		{
			case CLW_ECommand.MOVE: return "MOVE";
			case CLW_ECommand.ATTACK: return "ATTACK";
			case CLW_ECommand.DEFEND: return "DEFEND";
			case CLW_ECommand.RECON: return "RECON";
			case CLW_ECommand.FALL_BACK: return "FALL_BACK";
		}

		return "MOVE";
	}

	// Case-insensitive; "FALL BACK" / "FALLBACK" accepted. False for unknown names (command untouched).
	static bool FromName(string name, out CLW_ECommand command)
	{
		name.ToUpper();
		name.Replace(" ", "_");
		if (name == "MOVE")
			command = CLW_ECommand.MOVE;
		else if (name == "ATTACK")
			command = CLW_ECommand.ATTACK;
		else if (name == "DEFEND")
			command = CLW_ECommand.DEFEND;
		else if (name == "RECON")
			command = CLW_ECommand.RECON;
		else if (name == "FALL_BACK" || name == "FALLBACK")
			command = CLW_ECommand.FALL_BACK;
		else
			return false;

		return true;
	}
}
