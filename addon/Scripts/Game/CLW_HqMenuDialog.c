// HQ menu dialog (M4 WP4): a vanilla SCR_ConfigurableDialogUi (preset Configs/CLW/UI/HqMenuDialog.conf, content layout
// UI/layouts/CLW/HqMenuContent.layout with four empty section boxes). The sections are generated at runtime from the server payload
// (CLW_HqMenuData), with the vanilla widget library layouts, exactly like the setup menu. Every button sends an action to the
// server (CLW_HqMenuClient.Act); the server runs the same code as the chat commands and pushes fresh data, which rebuilds the sections.
// Sections: REQUESTS (vehicle / squad / resupply / reinforce / recon), SQUAD (join / leave HQ squads, SITREP), BASE (stock, FOBs,
// convoys), CONTACT REPORT (count, type, distance, direction; cancel last report; Backup help call), LANGUAGE (HQ language of the player, same as /hq language).
// Texts: stringtable keys (CLW_Loc.F) and the server's wires / content names (CLW_Loc.Resolve) in the game language. Log prefix: "CLW: hqmenu".

class CLW_HqMenuButton
{
	Managed m_Comp;				// SCR_ButtonTextComponent (owned by the widget)
	string m_sAction;
	string m_sArg;
	int m_iRequest = -1;		// index into CLW_HqMenuData.m_aRequests for "req"
}

class CLW_HqMenuDialog : SCR_ConfigurableDialogUi
{
	static const ResourceName LAYOUT_SPINBOX = "{C9DF0E6590F6C388}UI/layouts/WidgetLibrary/SpinBox/WLib_SpinBox.layout";
	static const ResourceName LAYOUT_SLIDER = "{4A41296C0E9A889F}UI/layouts/WidgetLibrary/WLib_Slider.layout";
	static const ResourceName LAYOUT_TEXT = "{3FF33FC224E0C76C}UI/layouts/WidgetLibrary/RichTextWidgets/RichText_BodySmall.layout";
	static const ResourceName LAYOUT_BUTTON = "{75C912A1C89BE6C2}UI/layouts/WidgetLibrary/Buttons/WLib_ButtonText.layout";

	ref CLW_HqMenuData m_Data;
	protected bool m_bOpen;
	protected Widget m_wRequestBox;
	protected Widget m_wSquadBox;
	protected Widget m_wBaseBox;
	protected Widget m_wReportBox;
	protected Widget m_wLangBox;
	protected ref array<ref CLW_HqMenuButton> m_aButtons = {};
	protected ref map<string, int> m_mSelection = new map<string, int>();	// request id -> chosen option, kept across refreshes

	// contact report inputs, kept across refreshes
	protected float m_fRepCount = 3;
	protected float m_fRepDist = 300;
	protected int m_iRepKind;
	protected int m_iRepDir;
	protected Managed m_RepCountComp;
	protected Managed m_RepDistComp;
	protected Managed m_RepKindComp;
	protected Managed m_RepDirComp;
	protected Managed m_VCatComp;		// depot menu: category spin box
	protected Managed m_LangComp;		// language spin box
	protected ref array<string> m_aLangCodes = {};	// codes of the language spin box items ("auto" first)

	bool IsOpen()
	{
		return m_bOpen;
	}

	protected override void OnMenuOpen(SCR_ConfigurableDialogUiPreset preset)
	{
		super.OnMenuOpen(preset);
		m_bOpen = true;
		Print("CLW: hqmenu opened", LogLevel.NORMAL);

		Widget root = GetRootWidget();
		if (!root || !m_Data)
		{
			Print("CLW: hqmenu has no root widget or data", LogLevel.ERROR);
			return;
		}

		m_wRequestBox = root.FindAnyWidget("RequestBox");
		m_wSquadBox = root.FindAnyWidget("SquadBox");
		m_wBaseBox = root.FindAnyWidget("BaseBox");
		m_wReportBox = root.FindAnyWidget("ReportBox");
		m_wLangBox = root.FindAnyWidget("LanguageBox");	// optional: an older layout has no language section
		if (!m_wRequestBox || !m_wSquadBox || !m_wBaseBox || !m_wReportBox)
		{
			Print("CLW: hqmenu layout is missing its section boxes (RequestBox / SquadBox / BaseBox / ReportBox)", LogLevel.ERROR);
			return;
		}

		SetTitle(CLW_Loc.F("#CLW-HqMenu_Title"));
		m_OnButtonPressed.Insert(OnTagPressed);
		Build();
	}

	override void OnMenuClose()
	{
		super.OnMenuClose();
		m_bOpen = false;
		CLW_HqMenuClient.OnClosed();
		Print("CLW: hqmenu closed", LogLevel.NORMAL);
	}

	// New payload from the server while the menu is open.
	void Update(CLW_HqMenuData data)
	{
		m_Data = data;
		if (m_wRequestBox)
			Build();
	}

	// ---- build ----

	protected void Build()
	{
		ClearChildren(m_wRequestBox);
		ClearChildren(m_wSquadBox);
		ClearChildren(m_wBaseBox);
		ClearChildren(m_wReportBox);
		ClearChildren(m_wLangBox);
		m_aButtons.Clear();
		m_RepCountComp = null;
		m_RepDistComp = null;
		m_RepKindComp = null;
		m_RepDirComp = null;
		m_VCatComp = null;
		m_LangComp = null;

		string headline = CLW_Loc.F("#CLW-HqMenu_Headline", m_Data.m_sFaction, "#CLW-Req_Role_" + m_Data.m_sRole, m_Data.m_sSupplies, m_Data.m_sBase);
		if (!m_Data.m_bRunning)
			headline = CLW_Loc.F("#CLW-HqMenu_NotRunning");
		if (m_Data.m_sStatus.IsEmpty())
			SetStatus(headline, false);
		else
			SetStatus(m_Data.m_sStatus, false);

		BuildRequests();
		BuildSquads();
		BuildBase();
		BuildReport();
		BuildLanguage();
	}

	protected void BuildRequests()
	{
		if (m_Data.m_bDepot)
		{
			BuildDepotVehicles();
			return;
		}

		if (m_Data.m_aRequests.IsEmpty())
		{
			AddText(m_wRequestBox, CLW_Loc.F("#CLW-HqMenu_NoRequests"), Color.FromSRGBA(160, 160, 160, 255));
			return;
		}

		foreach (int i, CLW_HqRequest request : m_Data.m_aRequests)
		{
			AddText(m_wRequestBox, CLW_Loc.Resolve(request.m_sTitle), null);
			request.m_Spin = null;
			if (request.m_aOptions.Count() > 1 || (request.m_aOptions.Count() == 1 && request.m_sId == "recon"))
			{
				Widget spinWidget = GetGame().GetWorkspace().CreateWidgets(LAYOUT_SPINBOX, m_wRequestBox);
				SCR_SpinBoxComponent spin;
				if (spinWidget)
					spin = SCR_SpinBoxComponent.Cast(spinWidget.FindHandler(SCR_SpinBoxComponent));
				if (spin)
				{
					spin.SetLabel(CLW_Loc.F("#CLW-HqMenu_Type"));
					foreach (CLW_HqOption option : request.m_aOptions)
					{
						spin.AddItem(CLW_Loc.Resolve(option.m_sLabel));
					}

					int kept;
					m_mSelection.Find(request.m_sId, kept);
					request.m_iSel = Math.Clamp(kept, 0, request.m_aOptions.Count() - 1);
					spin.SetCurrentItem(request.m_iSel, false, false, false);
					spin.m_OnChanged.Insert(OnSpinChanged);
					request.m_Spin = spin;
				}
			}

			AddButton(m_wRequestBox, CLW_Loc.F("#CLW-Req_Action", "#CLW-Req_Kind_" + request.m_sId), "req", "", i);
			if (!request.m_sBlocked.IsEmpty())
				AddText(m_wRequestBox, CLW_Loc.F("#CLW-HqMenu_NotPossible", request.m_sBlocked), Color.FromSRGBA(240, 130, 90, 255));
		}
	}

	protected static const int DEPOT_CATS = 6;

	protected static string DepotCatId(int index)
	{
		array<string> ids = {"all", "light", "motorized", "mech", "armor", "logistics"};
		return ids[Math.Clamp(index, 0, ids.Count() - 1)];
	}

	// Depot menu (WP11): one page of the vehicle list (the server sends only the page), category spin box, page buttons.
	protected void BuildDepotVehicles()
	{
		AddText(m_wRequestBox, CLW_Loc.F("#CLW-HqMenu_DepotTitle", CLW_Loc.N(m_Data.m_iTotal)), null);
		array<string> catNames = {};
		int catIndex;
		for (int c = 0; c < DEPOT_CATS; c++)
		{
			catNames.Insert(CLW_Loc.F("#CLW-HqMenu_Cat_" + DepotCatId(c)));
			if (DepotCatId(c) == m_Data.m_sCat)
				catIndex = c;
		}

		m_VCatComp = AddSpin(m_wRequestBox, CLW_Loc.F("#CLW-HqMenu_Category"), catNames, catIndex);
		if (!m_Data.m_sVBlocked.IsEmpty())
			AddText(m_wRequestBox, CLW_Loc.F("#CLW-HqMenu_NotPossible", m_Data.m_sVBlocked), Color.FromSRGBA(240, 130, 90, 255));

		if (m_Data.m_aVehicles.IsEmpty())
			AddText(m_wRequestBox, CLW_Loc.F("#CLW-HqMenu_DepotNone"), Color.FromSRGBA(160, 160, 160, 255));

		foreach (CLW_HqVehicle vehicle : m_Data.m_aVehicles)
		{
			string label = CLW_Loc.F("#CLW-HqMenu_VehicleBtn", vehicle.m_sLabel, CLW_Loc.N(vehicle.m_iCost));
			AddButton(m_wRequestBox, label, "vreq", vehicle.m_sType, -1);
		}

		if (m_Data.m_iPages > 1)
		{
			AddText(m_wRequestBox, CLW_Loc.F("#CLW-HqMenu_Page", CLW_Loc.N(m_Data.m_iPage + 1), CLW_Loc.N(m_Data.m_iPages)), null);
			if (m_Data.m_iPage > 0)
				AddButton(m_wRequestBox, CLW_Loc.F("#CLW-HqMenu_PagePrev"), "vpage", m_Data.m_sCat + "|" + (m_Data.m_iPage - 1).ToString(), -1);
			if (m_Data.m_iPage < m_Data.m_iPages - 1)
				AddButton(m_wRequestBox, CLW_Loc.F("#CLW-HqMenu_PageNext"), "vpage", m_Data.m_sCat + "|" + (m_Data.m_iPage + 1).ToString(), -1);
		}
	}

	protected void BuildSquads()
	{
		bool inSquad;
		foreach (CLW_HqSquad squad : m_Data.m_aSquads)
		{
			string info = CLW_Loc.F("#CLW-HqMenu_SquadInfo", squad.m_sName, CLW_Loc.N(squad.m_iHumans), CLW_Loc.N(squad.m_iMembers));
			if (squad.m_bMine)
			{
				inSquad = true;
				AddText(m_wSquadBox, CLW_Loc.F("#CLW-HqMenu_YourSquad", info), Color.FromSRGBA(140, 220, 140, 255));
			}
			else if (squad.m_bFull)
			{
				AddText(m_wSquadBox, CLW_Loc.F("#CLW-HqMenu_SquadFull", info), Color.FromSRGBA(160, 160, 160, 255));
			}
			else
			{
				AddButton(m_wSquadBox, CLW_Loc.F("#CLW-HqMenu_SquadJoin", info), "join", squad.m_sId, -1);
			}
		}

		if (m_Data.m_aSquads.IsEmpty())
			AddText(m_wSquadBox, CLW_Loc.F("#CLW-HqMenu_NoSquads"), Color.FromSRGBA(160, 160, 160, 255));

		if (inSquad)
			AddButton(m_wSquadBox, CLW_Loc.F("#CLW-HqMenu_Leave"), "leave", "", -1);

		AddButton(m_wSquadBox, CLW_Loc.F("#CLW-HqMenu_ShowSitrep"), "sitrep", "", -1);
	}

	protected void BuildBase()
	{
		foreach (string line : m_Data.m_aLines)
		{
			AddText(m_wBaseBox, CLW_Loc.Resolve(line), null);
		}
	}

	protected void BuildReport()
	{
		if (!m_Data.m_bReport)
		{
			string reason = CLW_Loc.Resolve(m_Data.m_sReportReason);
			if (reason.IsEmpty())
				reason = CLW_Loc.F("#CLW-HqMenu_ReportNA");
			AddText(m_wReportBox, CLW_Loc.F("#CLW-HqMenu_ReportNo", reason), Color.FromSRGBA(160, 160, 160, 255));
			return;
		}

		AddText(m_wReportBox, CLW_Loc.F("#CLW-HqMenu_ReportHelp"), Color.FromSRGBA(160, 160, 160, 255));
		m_RepCountComp = AddSlider(m_wReportBox, CLW_Loc.F("#CLW-HqMenu_Count"), 1, 30, 1, m_fRepCount);
		array<string> kindNames = {CLW_Loc.F("#CLW-HqMenu_KindInf"), CLW_Loc.F("#CLW-HqMenu_KindVeh"), CLW_Loc.F("#CLW-HqMenu_KindArmor"), CLW_Loc.F("#CLW-HqMenu_KindAir"), CLW_Loc.F("#CLW-HqMenu_KindUnknown")};
		array<string> dirNames = {CLW_Loc.F("#CLW-HqMenu_DirAhead"), "N", "NE", "E", "SE", "S", "SW", "W", "NW"};
		m_RepKindComp = AddSpin(m_wReportBox, CLW_Loc.F("#CLW-HqMenu_Type"), kindNames, m_iRepKind);
		m_RepDistComp = AddSlider(m_wReportBox, CLW_Loc.F("#CLW-HqMenu_Distance"), 50, 1500, 50, m_fRepDist);
		m_RepDirComp = AddSpin(m_wReportBox, CLW_Loc.F("#CLW-HqMenu_Direction"), dirNames, m_iRepDir);
		AddButton(m_wReportBox, CLW_Loc.F("#CLW-HqMenu_SendReport"), "report", "", -1);
		AddButton(m_wReportBox, CLW_Loc.F("#CLW-HqMenu_CancelReport"), "report_cancel", "", -1);
		AddText(m_wReportBox, CLW_Loc.F("#CLW-HqMenu_BackupHelp"), Color.FromSRGBA(160, 160, 160, 255));
		AddButton(m_wReportBox, CLW_Loc.F("#CLW-HqMenu_Backup"), "backup", "", -1);
	}

	// HQ language of the player (wire line G): "auto" or a code. The list shows the common languages; any other ISO code is set
	// with /hq language <code> and then appears as an extra item. Choosing an item sends the action "lang" (server validates).
	protected void BuildLanguage()
	{
		if (!m_wLangBox || !m_Data.m_bRunning)
			return;

		// the item that shows is the stored override; "auto" when none is set (the effective language is in the status text)
		string selected = "auto";
		if (!m_Data.m_sLangOverride.IsEmpty())
			selected = m_Data.m_sLangOverride;

		m_aLangCodes.Clear();
		m_aLangCodes.Insert("auto");
		CLW_HqLang.GetMenuCodes(m_aLangCodes);
		if (!m_aLangCodes.Contains(selected))
			m_aLangCodes.Insert(selected);

		array<string> names = {};
		int current;
		foreach (int i, string code : m_aLangCodes)
		{
			names.Insert(CLW_Loc.F(CLW_HqLang.Label(code)));
			if (code == selected)
				current = i;
		}

		string currentText = selected;
		if (selected == "auto")
			currentText = CLW_Loc.W("#CLW-Lang_AutoOf", m_Data.m_sLang);
		AddText(m_wLangBox, CLW_Loc.F("#CLW-HqMenu_LangCurrent", currentText), null);
		m_LangComp = AddSpin(m_wLangBox, CLW_Loc.F("#CLW-HqMenu_LangSpin"), names, current);
		AddText(m_wLangBox, CLW_Loc.F("#CLW-HqMenu_LangHint"), Color.FromSRGBA(160, 160, 160, 255));
	}

	// ---- widget helpers ----

	protected void ClearChildren(Widget parent)
	{
		if (!parent)
			return;

		Widget child = parent.GetChildren();
		while (child)
		{
			Widget next = child.GetSibling();
			child.RemoveFromHierarchy();
			child = next;
		}
	}

	protected Widget AddText(Widget parent, string text, Color color)
	{
		Widget widget = GetGame().GetWorkspace().CreateWidgets(LAYOUT_TEXT, parent);
		if (!widget)
			return null;

		RichTextWidget rich = RichTextWidget.Cast(widget);
		if (rich)
			rich.SetText(text);
		if (color)
			widget.SetColor(color);
		return widget;
	}

	protected void AddButton(Widget parent, string label, string action, string arg, int requestIndex)
	{
		Widget widget = GetGame().GetWorkspace().CreateWidgets(LAYOUT_BUTTON, parent);
		SCR_ButtonTextComponent comp;
		if (widget)
			comp = SCR_ButtonTextComponent.Cast(widget.FindHandler(SCR_ButtonTextComponent));
		if (!comp)
		{
			Print("CLW: hqmenu could not create a button for '" + label + "'", LogLevel.WARNING);
			return;
		}

		comp.SetText(label);
		comp.m_OnClicked.Insert(OnButtonClicked);
		CLW_HqMenuButton button = new CLW_HqMenuButton();
		button.m_Comp = comp;
		button.m_sAction = action;
		button.m_sArg = arg;
		button.m_iRequest = requestIndex;
		m_aButtons.Insert(button);
	}

	protected Managed AddSlider(Widget parent, string label, float min, float max, float step, float value)
	{
		Widget widget = GetGame().GetWorkspace().CreateWidgets(LAYOUT_SLIDER, parent);
		SCR_SliderComponent slider;
		if (widget)
			slider = SCR_SliderComponent.Cast(widget.FindHandler(SCR_SliderComponent));
		if (!slider)
			return null;

		slider.SetLabel(label);
		slider.SetFormatText("%1");
		slider.SetMax(max);		// max first: the widget's default range is 0..1
		slider.SetMin(min);
		slider.SetStep(step);
		slider.SetValue(value);
		slider.m_OnChanged.Insert(OnSliderChanged);
		slider.ShowCustomValue(Math.Round(value).ToString());
		return slider;
	}

	protected Managed AddSpin(Widget parent, string label, notnull array<string> items, int current)
	{
		Widget widget = GetGame().GetWorkspace().CreateWidgets(LAYOUT_SPINBOX, parent);
		SCR_SpinBoxComponent spin;
		if (widget)
			spin = SCR_SpinBoxComponent.Cast(widget.FindHandler(SCR_SpinBoxComponent));
		if (!spin)
			return null;

		spin.SetLabel(label);
		foreach (string item : items)
		{
			spin.AddItem(item);
		}

		spin.SetCurrentItem(current, false, false, false);
		spin.m_OnChanged.Insert(OnSpinChanged);
		return spin;
	}

	// ---- widget events ----

	protected void OnSliderChanged(SCR_SliderComponent comp, float value)
	{
		comp.ShowCustomValue(Math.Round(value).ToString());
		if (comp == m_RepCountComp)
			m_fRepCount = value;
		else if (comp == m_RepDistComp)
			m_fRepDist = value;
	}

	protected void OnSpinChanged(SCR_SpinBoxComponent comp, int index)
	{
		if (comp == m_RepKindComp)
		{
			m_iRepKind = index;
			return;
		}

		if (m_VCatComp && comp == m_VCatComp)
		{
			SetStatus(CLW_Loc.F("#CLW-HqMenu_Sending"), false);
			CLW_HqMenuClient.Act("vpage", DepotCatId(index) + "|0");
			return;
		}

		if (m_LangComp && comp == m_LangComp)
		{
			if (index >= 0 && index < m_aLangCodes.Count())
			{
				SetStatus(CLW_Loc.F("#CLW-HqMenu_Sending"), false);
				CLW_HqMenuClient.Act("lang", m_aLangCodes[index]);
			}

			return;
		}

		if (comp == m_RepDirComp)
		{
			m_iRepDir = index;
			return;
		}

		foreach (CLW_HqRequest request : m_Data.m_aRequests)
		{
			if (request.m_Spin == comp)
			{
				request.m_iSel = index;
				m_mSelection.Set(request.m_sId, index);
				return;
			}
		}
	}

	protected void OnButtonClicked(SCR_ButtonBaseComponent comp)
	{
		foreach (CLW_HqMenuButton button : m_aButtons)
		{
			if (button.m_Comp == comp)
			{
				OnAction(button);
				return;
			}
		}
	}

	protected void OnAction(CLW_HqMenuButton button)
	{
		if (button.m_sAction == "sitrep")
		{
			// the SITREP lives on the client (CLW_SitrepClient): show the hint, ask the server for a fresh one
			CLW_SitrepClient.ShowHint(true);
			SCR_PlayerController sitrepController = SCR_PlayerController.Cast(GetGame().GetPlayerController());
			if (sitrepController)
				sitrepController.CLW_SitrepRequest();
			return;
		}

		string arg = button.m_sArg;
		if (button.m_sAction == "req")
		{
			if (button.m_iRequest < 0 || button.m_iRequest >= m_Data.m_aRequests.Count())
				return;

			CLW_HqRequest request = m_Data.m_aRequests[button.m_iRequest];
			string typeArg;
			if (request.m_iSel >= 0 && request.m_iSel < request.m_aOptions.Count())
				typeArg = request.m_aOptions[request.m_iSel].m_sArg;
			if (typeArg == "-")
				typeArg = "";
			arg = request.m_sKind + "|" + typeArg;
		}
		else if (button.m_sAction == "report")
		{
			arg = ReportArgs();
		}

		SetStatus(CLW_Loc.F("#CLW-HqMenu_Sending"), false);
		CLW_HqMenuClient.Act(button.m_sAction, arg);
	}

	// "<count> <inf|veh|armor|air|unknown> <dist> <dir>" (CLW_Reports.Chat format; dir may be "ahead" = the view direction, read on the server).
	protected string ReportArgs()
	{
		array<string> kinds = {"inf", "veh", "armor", "air", "unknown"};
		array<string> dirs = {"ahead", "N", "NE", "E", "SE", "S", "SW", "W", "NW"};
		float count = m_fRepCount;
		float dist = m_fRepDist;
		SCR_SliderComponent countSlider = SCR_SliderComponent.Cast(m_RepCountComp);
		if (countSlider)
			count = countSlider.GetValue();
		SCR_SliderComponent distSlider = SCR_SliderComponent.Cast(m_RepDistComp);
		if (distSlider)
			dist = distSlider.GetValue();

		int kindIndex = Math.Clamp(m_iRepKind, 0, kinds.Count() - 1);
		int dirIndex = Math.Clamp(m_iRepDir, 0, dirs.Count() - 1);
		return string.Format("%1 %2 %3 %4", Math.Round(count), kinds[kindIndex], Math.Round(dist), dirs[dirIndex]);
	}

	protected void OnTagPressed(SCR_ConfigurableDialogUi dialog, string tag)
	{
		if (tag != "refresh")
			return;

		SetStatus(CLW_Loc.F("#CLW-HqMenu_Refreshing"), false);
		CLW_HqMenuClient.Act("refresh", "");
	}

	// ---- status line ----

	void SetStatus(string text, bool isError)
	{
		SetMessage(CLW_Loc.Resolve(text));
		if (isError)
			SetMessageColor(Color.FromSRGBA(240, 90, 70, 255));
		else
			SetMessageColor(Color.FromSRGBA(220, 220, 220, 255));
	}
}
