// Controller of the HQ radial menu (WP2): the vanilla SCR_RadialMenuController, created in script instead of a .conf. It listens to the input action
// CLW_HqRadial (hold), takes over the GLOBAL radial menu from whoever used it last (commanding, emotes) and fills it with the HQ entries (CLW_HqRadial).
// Hold the key, point at a level, click (or release the key on an entry) ; right click / back goes one level up. 1-9 pick an entry directly.
// Log prefix: "CLW: hqradial".
class CLW_HqRadialController : SCR_RadialMenuController
{
	protected bool m_bViaCommand;		// opened by /hqr: no key is held, so releasing it must not close or perform

	void CLW_HqRadialController()
	{
		// the base constructor ran with no inputs (nothing to listen to): build them here. Attribute defaults only apply to config-loaded objects.
		m_sEnableControl = true;
		SCR_RadialMenuControllerInputs inputs = new SCR_RadialMenuControllerInputs();	// typed local: the radial fields are not on the base class of m_RMControls
		inputs.m_sOpenAction = CLW_HqRadial.ACTION_OPEN;
		inputs.m_bCloseOnReleaseOpen = true;
		inputs.m_bPerformOnClose = true;
		inputs.m_bCloseOnPerform = true;
		inputs.m_bOpenInRoot = true;
		inputs.m_bPreventEmptyMenuOpen = false;
		inputs.m_bShowWhileUnconcious = false;
		inputs.m_bUseRightStick = true;
		inputs.m_bDeselectInCenter = true;
		inputs.m_bUseLargeSize = true;
		inputs.m_fCustomSize = -1;
		inputs.m_bShowCrosshair = false;
		inputs.m_bShowInnerBackground = true;
		inputs.m_bUseQuickActions = true;
		m_RMControls = inputs;

		GetGame().GetInputManager().AddActionListener(CLW_HqRadial.ACTION_OPEN, EActionTrigger.DOWN, OnInputOpen);
	}

	// Chat fallback: open without the key.
	void OpenFromCommand()
	{
		m_bViaCommand = true;
		OnInputOpen();
	}

	// Only with a living character, no map. The war state is the server's business (entries are disabled from the payload).
	protected bool CanOpen()
	{
		IEntity entity = SCR_PlayerController.GetLocalControlledEntity();
		if (!entity)
		{
			Print("CLW: hqradial not opened: no controlled character", LogLevel.NORMAL);
			return false;
		}

		CharacterControllerComponent character = CharacterControllerComponent.Cast(entity.FindComponent(CharacterControllerComponent));
		if (character && character.GetLifeState() != ECharacterLifeState.ALIVE)
		{
			Print("CLW: hqradial not opened: character is not alive", LogLevel.NORMAL);
			return false;
		}

		SCR_MapEntity mapEntity = SCR_MapEntity.GetMapInstance();
		if (mapEntity && mapEntity.IsOpen())
		{
			Print("CLW: hqradial not opened: map is open", LogLevel.NORMAL);
			return false;
		}

		return true;
	}

	override void OnInputOpen()
	{
		bool viaCommand = m_bViaCommand;
		m_bViaCommand = false;
		if (IsMenuOpen())
			return;

		if (!CanOpen())
			return;

		// a held key closes the menu on release and performs the pointed entry; a command-opened menu waits for a click
		m_RMControls.m_bCloseOnReleaseOpen = !viaCommand;
		m_RMControls.m_bPerformOnClose = !viaCommand;

		// take the global menu first: the previous owner clears it and drops its listeners when the controls change
		if (!HasControl())
			Control(SCR_PlayerController.GetLocalControlledEntity());

		SCR_RadialMenu menu = GetRadialMenu();
		if (!menu)
		{
			Print("CLW: hqradial not opened: no global radial menu (SCR_RadialMenuGameModeComponent missing on the player controller?)", LogLevel.ERROR);
			return;
		}

		Print(string.Format("CLW: hqradial open viaCommand=%1 data=%2", viaCommand, CLW_HqRadial.HasData()), LogLevel.NORMAL);
		CLW_HqRadial.RequestRefresh();

		array<ref SCR_SelectionMenuEntry> entries = {};
		CLW_HqRadial.BuildRoot(entries);
		menu.AddEntries(entries, true);
		if (!menu.HasDisplay())
			menu.SetMenuDisplay();

		super.OnInputOpen();
	}

	// A fresh payload arrived: rebuild the first circle while it is open and nobody went into a level yet (a level in use keeps its entries).
	void Rebuild()
	{
		if (!IsMenuOpen())
			return;

		SCR_RadialMenu menu = GetRadialMenu();
		if (!menu || menu.CurrentCategory())
			return;

		array<ref SCR_SelectionMenuEntry> entries = {};
		CLW_HqRadial.BuildRoot(entries);
		menu.AddEntries(entries, true);
	}
}
