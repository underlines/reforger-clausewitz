// Client side data + dialog of the war setup menu (phase P5).
// The dialog is a vanilla SCR_ConfigurableDialogUi (preset Configs/CLW/UI/SetupDialog.conf, content layout
// UI/layouts/CLW/SetupDialogContent.layout). The content is generated at runtime from the server's wire text
// (CLW_SetupServer.BuildWire), using the vanilla widget library layouts (slider / spin box / checkbox), so a new
// mode or parameter needs no UI change.

// One parameter as received from the server.
class CLW_SetupParam
{
	string m_sId;
	string m_sLabel;
	string m_sDescription;
	int m_iType;						// CLW_ModeParam.TYPE_*
	float m_fMin;
	float m_fMax;
	float m_fStep = 1;
	float m_fValue;						// INT/FLOAT value, BOOL 0/1, CHOICE index (kept in sync with the widget)
	float m_fDefault;
	ref array<string> m_aChoices = {};
	Managed m_Comp;						// widget component of the current row (owned by the widget)
}

class CLW_SetupMode
{
	string m_sId;
	string m_sName;
	string m_sDescription;
	ref array<ref CLW_SetupParam> m_aParams = {};
}

class CLW_SetupData
{
	string m_sDefaultMode;				// mode the dialog pre-selects (the remembered one after a rematch)
	string m_sResetMode;				// mode "Reset to defaults" selects
	bool m_bRemembered;					// the values are the last round's setup
	ref array<ref CLW_SetupParam> m_aBattle = {};
	ref array<ref CLW_SetupMode> m_aModes = {};

	// Widget value of a wire value text (BOOL 0/1, CHOICE index, number).
	static float ValueFromText(CLW_SetupParam param, string valueText)
	{
		if (param.m_iType == CLW_ModeParam.TYPE_BOOL)
		{
			if (valueText == "true")
				return 1;

			return 0;
		}

		if (param.m_iType == CLW_ModeParam.TYPE_CHOICE)
		{
			int index = param.m_aChoices.Find(valueText);
			if (index < 0)
				index = 0;
			return index;
		}

		return valueText.ToFloat();
	}

	// Parses the line based payload (see CLW_SetupServer.BuildWire). Null when nothing usable arrived.
	static CLW_SetupData Parse(string wire)
	{
		CLW_SetupData data = new CLW_SetupData();
		array<string> lines = {};
		wire.Split("\n", lines, true);
		foreach (string line : lines)
		{
			array<string> f = {};
			line.Split("\t", f, false);
			if (f.IsEmpty())
				continue;

			if (f[0] == "H" && f.Count() >= 2)
			{
				data.m_sDefaultMode = f[1];
				data.m_sResetMode = f[1];
				if (f.Count() >= 3 && !f[2].IsEmpty())
					data.m_sResetMode = f[2];
				if (f.Count() >= 4 && f[3] == "1")
					data.m_bRemembered = true;
			}
			else if (f[0] == "M" && f.Count() >= 4)
			{
				CLW_SetupMode mode = new CLW_SetupMode();
				mode.m_sId = f[1];
				mode.m_sName = f[2];
				mode.m_sDescription = f[3];
				data.m_aModes.Insert(mode);
			}
			else if (f[0] == "P" && f.Count() >= 10)
			{
				CLW_SetupParam param = new CLW_SetupParam();
				param.m_sId = f[2];
				param.m_sLabel = f[3];
				param.m_sDescription = f[4];
				param.m_iType = f[5].ToInt();
				param.m_fMin = f[6].ToFloat();
				param.m_fMax = f[7].ToFloat();
				param.m_fStep = f[8].ToFloat();
				if (param.m_fStep <= 0)
					param.m_fStep = 1;

				if (f.Count() >= 11 && !f[10].IsEmpty())
					f[10].Split("|", param.m_aChoices, true);

				if (param.m_iType == CLW_ModeParam.TYPE_CHOICE)
				{
					param.m_fMin = 0;
					param.m_fMax = Math.Max(0, param.m_aChoices.Count() - 1);
				}

				param.m_fValue = ValueFromText(param, f[9]);
				param.m_fDefault = param.m_fValue;
				if (f.Count() >= 12)
					param.m_fDefault = ValueFromText(param, f[11]);		// "Reset to defaults" (the remembered values may differ)
				if (f[1] == "B")
				{
					data.m_aBattle.Insert(param);
				}
				else if (f[1].StartsWith("M:"))
				{
					string modeId = f[1].Substring(2, f[1].Length() - 2);
					foreach (CLW_SetupMode owner : data.m_aModes)
					{
						if (owner.m_sId == modeId)
							owner.m_aParams.Insert(param);
					}
				}
			}
		}

		if (data.m_aModes.IsEmpty())
			return null;

		return data;
	}
}

class CLW_SetupDialog : SCR_ConfigurableDialogUi
{
	// Widget library layouts (vanilla; GUIDs from Configs/ServerBrowser/ServerHosting/WidgetEntry_*.conf)
	static const ResourceName LAYOUT_SPINBOX = "{C9DF0E6590F6C388}UI/layouts/WidgetLibrary/SpinBox/WLib_SpinBox.layout";
	static const ResourceName LAYOUT_SLIDER = "{4A41296C0E9A889F}UI/layouts/WidgetLibrary/WLib_Slider.layout";
	static const ResourceName LAYOUT_CHECKBOX = "{5D5055E10FD00549}UI/layouts/WidgetLibrary/ToolBoxes/WLib_Checkbox.layout";
	static const ResourceName LAYOUT_TEXT = "{3FF33FC224E0C76C}UI/layouts/WidgetLibrary/RichTextWidgets/RichText_BodySmall.layout";

	ref CLW_SetupData m_Data;			// set by CLW_SetupClient before the dialog is created
	protected bool m_bOpen;
	protected bool m_bSubmitting;
	protected Widget m_wModeBox;
	protected Widget m_wTabBox;
	protected Widget m_wSectionParams;
	protected RichTextWidget m_wModeDesc;
	protected SCR_SpinBoxComponent m_ModeSpin;
	protected SCR_SpinBoxComponent m_SectionSpin;
	protected CLW_SetupMode m_CurrentMode;
	protected int m_iSection = CLW_SetupSections.BASICS;		// section shown (CLW_SetupSections)
	protected ref array<int> m_aSectionIds = {};				// section id per entry of the section selector (empty sections are left out)
	protected ref array<ref array<CLW_SetupParam>> m_aSectionParams = {};	// parameters of the current mode per section id, in display order

	bool IsOpen()
	{
		return m_bOpen;
	}

	// ---- build ----

	protected override void OnMenuOpen(SCR_ConfigurableDialogUiPreset preset)
	{
		super.OnMenuOpen(preset);
		m_bOpen = true;
		Print("CLW: setup menu opened", LogLevel.NORMAL);

		Widget root = GetRootWidget();
		if (!root || !m_Data)
		{
			Print("CLW: setup menu has no root widget or data", LogLevel.ERROR);
			return;
		}

		m_wModeBox = root.FindAnyWidget("ModeBox");
		m_wTabBox = root.FindAnyWidget("TabBox");
		m_wSectionParams = root.FindAnyWidget("SectionParams");
		m_wModeDesc = RichTextWidget.Cast(root.FindAnyWidget("ModeDesc"));
		if (!m_wModeBox || !m_wTabBox || !m_wSectionParams)
		{
			Print("CLW: setup menu layout is missing its containers (ModeBox / TabBox / SectionParams)", LogLevel.ERROR);
			return;
		}

		RichTextWidget modeHeader = RichTextWidget.Cast(root.FindAnyWidget("ModeHeader"));
		if (modeHeader)
			modeHeader.SetText(CLW_Loc.Fmt("#CLW-Setup_WarMode"));
		RichTextWidget tabHeader = RichTextWidget.Cast(root.FindAnyWidget("TabHeader"));
		if (tabHeader)
			tabHeader.SetText(CLW_Loc.Fmt("#CLW-Setup_Options"));

		SetTitle(CLW_Loc.Fmt("#CLW-Setup_Title"));
		string intro = CLW_Loc.Fmt("#CLW-Setup_Intro");
		if (m_Data.m_bRemembered)
			intro += " " + CLW_Loc.Fmt("#CLW-Setup_Remembered");
		SetMessage(intro);
		Print(string.Format("CLW: setup menu prefilled from the last round: %1", m_Data.m_bRemembered), LogLevel.NORMAL);
		m_OnButtonPressed.Insert(OnTagPressed);

		BuildAll();
	}

	// Mode selector, then the section selector + the parameters of the shown section (battle parameters and the selected mode's).
	protected void BuildAll()
	{
		ReleaseControls();
		ClearChildren(m_wModeBox);
		ClearChildren(m_wTabBox);
		ClearChildren(m_wSectionParams);
		m_ModeSpin = null;
		m_SectionSpin = null;

		Widget spinWidget = GetGame().GetWorkspace().CreateWidgets(LAYOUT_SPINBOX, m_wModeBox);
		if (spinWidget)
			m_ModeSpin = SCR_SpinBoxComponent.Cast(spinWidget.FindHandler(SCR_SpinBoxComponent));

		if (!m_ModeSpin)
		{
			Print("CLW: setup menu cannot create the mode spin box, using the default mode", LogLevel.ERROR);
			SelectMode(FindModeIndex(m_Data.m_sDefaultMode));
			return;
		}

		m_ModeSpin.SetLabel(CLW_Loc.Fmt("#CLW-Setup_ModeLabel"));
		foreach (CLW_SetupMode mode : m_Data.m_aModes)
		{
			m_ModeSpin.AddItem(CLW_Loc.Resolve(mode.m_sName));
		}

		int index = FindModeIndex(m_Data.m_sDefaultMode);
		m_ModeSpin.SetCurrentItem(index, false, false, false);
		m_ModeSpin.m_OnChanged.Insert(OnSpinChanged);
		SelectMode(index);
	}

	protected int FindModeIndex(string id)
	{
		foreach (int i, CLW_SetupMode mode : m_Data.m_aModes)
		{
			if (mode.m_sId == id)
				return i;
		}

		return 0;
	}

	protected void SelectMode(int index)
	{
		if (index < 0 || index >= m_Data.m_aModes.Count())
			return;

		m_CurrentMode = m_Data.m_aModes[index];
		Print("CLW: setup menu mode selected: " + m_CurrentMode.m_sId, LogLevel.NORMAL);
		if (m_wModeDesc)
			m_wModeDesc.SetText(CLW_Loc.Resolve(m_CurrentMode.m_sDescription));

		ReleaseControls();
		ComputeSections();
		BuildSectionSelector();
	}

	// The widgets are about to be removed: forget them (the parameter keeps its value in m_fValue, the events keep it in sync).
	protected void ReleaseControls()
	{
		foreach (CLW_SetupParam battleParam : m_Data.m_aBattle)
		{
			battleParam.m_Comp = null;
		}

		foreach (CLW_SetupMode mode : m_Data.m_aModes)
		{
			foreach (CLW_SetupParam modeParam : mode.m_aParams)
			{
				modeParam.m_Comp = null;
			}
		}
	}

	// Sorts the battle parameters and the current mode's parameters into the sections (by key, CLW_SetupSections).
	protected void ComputeSections()
	{
		m_aSectionParams.Clear();
		for (int i = 0; i < CLW_SetupSections.COUNT; i++)
		{
			m_aSectionParams.Insert(new array<CLW_SetupParam>());
		}

		foreach (CLW_SetupParam battleParam : m_Data.m_aBattle)
		{
			PlaceInSection(battleParam);
		}

		if (!m_CurrentMode)
			return;

		foreach (CLW_SetupParam modeParam : m_CurrentMode.m_aParams)
		{
			PlaceInSection(modeParam);
		}
	}

	protected void PlaceInSection(CLW_SetupParam param)
	{
		array<CLW_SetupParam> list = m_aSectionParams[CLW_SetupSections.SectionOf(param.m_sId)];
		int order = CLW_SetupSections.OrderOf(param.m_sId);
		int at = list.Count();
		for (int i = 0; i < list.Count(); i++)
		{
			if (CLW_SetupSections.OrderOf(list[i].m_sId) > order)
			{
				at = i;
				break;
			}
		}

		list.InsertAt(param, at);
	}

	// The section selector (a spin box: arrows or click, like the war mode) and the first / previously shown section.
	protected void BuildSectionSelector()
	{
		ClearChildren(m_wTabBox);
		m_SectionSpin = null;
		m_aSectionIds.Clear();

		Widget spinWidget = GetGame().GetWorkspace().CreateWidgets(LAYOUT_SPINBOX, m_wTabBox);
		if (spinWidget)
			m_SectionSpin = SCR_SpinBoxComponent.Cast(spinWidget.FindHandler(SCR_SpinBoxComponent));

		int current;
		for (int s = 0; s < CLW_SetupSections.COUNT; s++)
		{
			if (m_aSectionParams[s].IsEmpty())
				continue;

			if (s == m_iSection)
				current = m_aSectionIds.Count();
			m_aSectionIds.Insert(s);
			if (m_SectionSpin)
				m_SectionSpin.AddItem(CLW_Loc.Resolve(CLW_SetupSections.Name(s)));
		}

		if (m_aSectionIds.IsEmpty())
			return;

		if (m_SectionSpin)
		{
			m_SectionSpin.SetLabel(CLW_Loc.Fmt("#CLW-Setup_SectionLabel"));
			m_SectionSpin.SetCurrentItem(current, false, false, false);
			m_SectionSpin.m_OnChanged.Insert(OnSpinChanged);
		}
		else
		{
			Print("CLW: setup menu cannot create the section selector, showing the first section only", LogLevel.ERROR);
		}

		ShowSection(m_aSectionIds[current]);
	}

	protected void ShowSection(int section)
	{
		ReleaseControls();
		ClearChildren(m_wSectionParams);
		m_iSection = section;
		foreach (CLW_SetupParam param : m_aSectionParams[section])
		{
			AddParamRow(m_wSectionParams, param);
		}
	}

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

	// One control by parameter type + a grey description line below it.
	protected void AddParamRow(Widget parent, CLW_SetupParam param)
	{
		param.m_Comp = null;
		if (param.m_iType == CLW_ModeParam.TYPE_BOOL)
		{
			Widget checkWidget = GetGame().GetWorkspace().CreateWidgets(LAYOUT_CHECKBOX, parent);
			SCR_CheckboxComponent check;
			if (checkWidget)
				check = SCR_CheckboxComponent.Cast(checkWidget.FindHandler(SCR_CheckboxComponent));
			if (check)
			{
				check.SetLabel(CLW_Loc.Resolve(param.m_sLabel));
				check.SetChecked(param.m_fValue >= 0.5, false, false);
				check.m_OnChanged.Insert(OnCheckChanged);
				param.m_Comp = check;
			}
		}
		else if (param.m_iType == CLW_ModeParam.TYPE_CHOICE)
		{
			Widget spinWidget2 = GetGame().GetWorkspace().CreateWidgets(LAYOUT_SPINBOX, parent);
			SCR_SpinBoxComponent spin;
			if (spinWidget2)
				spin = SCR_SpinBoxComponent.Cast(spinWidget2.FindHandler(SCR_SpinBoxComponent));
			if (spin)
			{
				spin.SetLabel(CLW_Loc.Resolve(param.m_sLabel));
				foreach (string choice : param.m_aChoices)
				{
					spin.AddItem(CLW_Loc.Choice(param.m_sId, choice));	// faction names are localisation keys (#AR-Faction_US)
				}
				spin.SetCurrentItem(Math.Round(param.m_fValue), false, false, false);
				spin.m_OnChanged.Insert(OnSpinChanged);
				param.m_Comp = spin;
			}
		}
		else
		{
			Widget sliderWidget = GetGame().GetWorkspace().CreateWidgets(LAYOUT_SLIDER, parent);
			SCR_SliderComponent slider;
			if (sliderWidget)
				slider = SCR_SliderComponent.Cast(sliderWidget.FindHandler(SCR_SliderComponent));
			if (slider)
			{
				slider.SetLabel(CLW_Loc.Resolve(param.m_sLabel));
				slider.SetFormatText("%1");
				slider.SetMax(param.m_fMax);		// max first: the widget's default range is 0..1
				slider.SetMin(param.m_fMin);
				slider.SetStep(param.m_fStep);
				slider.SetValue(param.m_fValue);
				slider.m_OnChanged.Insert(OnSliderChanged);
				slider.ShowCustomValue(FormatNumber(param, param.m_fValue));
				param.m_Comp = slider;
			}
		}

		if (!param.m_Comp)
			Print("CLW: setup menu could not create a control for '" + param.m_sId + "'", LogLevel.WARNING);

		if (!param.m_sDescription.IsEmpty())
			AddText(parent, CLW_Loc.Resolve(param.m_sDescription), Color.FromSRGBA(160, 160, 160, 255));
	}

	protected string FormatNumber(CLW_SetupParam param, float value)
	{
		if (param.m_iType == CLW_ModeParam.TYPE_INT)
			return Math.Round(value).ToString();

		float rounded = Math.Round(value * 100) / 100;
		return rounded.ToString();
	}

	// ---- widget events (each keeps the parameter's value in sync) ----

	protected CLW_SetupParam FindParam(Managed comp)
	{
		foreach (CLW_SetupParam param : m_Data.m_aBattle)
		{
			if (param.m_Comp == comp)
				return param;
		}

		if (m_CurrentMode)
		{
			foreach (CLW_SetupParam modeParam : m_CurrentMode.m_aParams)
			{
				if (modeParam.m_Comp == comp)
					return modeParam;
			}
		}

		return null;
	}

	protected void OnSliderChanged(SCR_SliderComponent comp, float value)
	{
		CLW_SetupParam param = FindParam(comp);
		if (!param)
			return;

		param.m_fValue = value;
		comp.ShowCustomValue(FormatNumber(param, value));
	}

	protected void OnCheckChanged(SCR_CheckboxComponent comp, bool checked)
	{
		CLW_SetupParam param = FindParam(comp);
		if (!param)
			return;

		if (checked)
			param.m_fValue = 1;
		else
			param.m_fValue = 0;
	}

	protected void OnSpinChanged(SCR_SpinBoxComponent comp, int index)
	{
		if (comp == m_ModeSpin)
		{
			SelectMode(index);
			return;
		}

		if (comp == m_SectionSpin)
		{
			if (index >= 0 && index < m_aSectionIds.Count())
				ShowSection(m_aSectionIds[index]);
			return;
		}

		CLW_SetupParam param = FindParam(comp);
		if (param)
			param.m_fValue = index;
	}

	// ---- buttons ----

	protected void OnTagPressed(SCR_ConfigurableDialogUi dialog, string tag)
	{
		if (tag != "defaults")
			return;

		Print("CLW: setup menu: reset to defaults (overrides.json preset or built-in values, not the last round)", LogLevel.NORMAL);
		m_Data.m_sDefaultMode = m_Data.m_sResetMode;
		foreach (CLW_SetupParam param : m_Data.m_aBattle)
		{
			param.m_fValue = param.m_fDefault;
		}

		foreach (CLW_SetupMode mode : m_Data.m_aModes)
		{
			foreach (CLW_SetupParam modeParam : mode.m_aParams)
			{
				modeParam.m_fValue = modeParam.m_fDefault;
			}
		}

		BuildAll();
		SetStatus(CLW_Loc.Fmt("#CLW-Setup_DefaultsRestored"), false);
	}

	// Start war: send to the server and stay open until the server answers (error text) or announces the start (Close).
	protected override void OnConfirm()
	{
		if (m_bSubmitting)
			return;

		if (!m_CurrentMode)
		{
			SetStatus(CLW_Loc.Fmt("#CLW-Setup_NoMode"), true);
			return;
		}

		string battleJson = ValuesJson(m_Data.m_aBattle);
		string modeJson = ValuesJson(m_CurrentMode.m_aParams);
		string error = LocalCheck();
		if (!error.IsEmpty())
		{
			Print("CLW: setup menu local check failed: " + error, LogLevel.NORMAL);
			SetStatus(CLW_Loc.Resolve(error), true);
			return;
		}

		SCR_PlayerController controller = SCR_PlayerController.Cast(GetGame().GetPlayerController());
		if (!controller)
		{
			SetStatus(CLW_Loc.Fmt("#CLW-Setup_NoController"), true);
			return;
		}

		m_bSubmitting = true;
		SetStatus(CLW_Loc.Fmt("#CLW-Setup_Sending"), false);
		controller.CLW_SubmitSetup(battleJson, m_CurrentMode.m_sId, modeJson);
	}

	protected override void OnCancel()
	{
		Print("CLW: setup menu closed by the player (war not started)", LogLevel.NORMAL);
		super.OnCancel();
	}

	override void OnMenuClose()
	{
		super.OnMenuClose();
		m_bOpen = false;
		Print("CLW: setup menu closed", LogLevel.NORMAL);
	}

	// The two sides must differ (the server checks again).
	protected string LocalCheck()
	{
		string a;
		string b;
		foreach (CLW_SetupParam param : m_Data.m_aBattle)
		{
			string key = ChoiceText(param);
			if (param.m_sId == CLW_SetupServer.PARAM_FACTION_A)
				a = key;
			else if (param.m_sId == CLW_SetupServer.PARAM_FACTION_B)
				b = key;
		}

		if (!a.IsEmpty() && a == b)
			return CLW_Loc.W("#CLW-Setup_SidesDiffer");

		return "";
	}

	// ---- values ----

	protected string ChoiceText(CLW_SetupParam param)
	{
		SCR_SpinBoxComponent spin = SCR_SpinBoxComponent.Cast(param.m_Comp);
		int index = Math.Round(param.m_fValue);
		if (spin)
			index = spin.GetCurrentIndex();

		if (index < 0 || index >= param.m_aChoices.Count())
			return "";

		return param.m_aChoices[index];
	}

	// {"id":value,...} read from the widgets (CLW_ModeParam.ValuesToJson format).
	protected string ValuesJson(notnull array<ref CLW_SetupParam> list)
	{
		string json = "{";
		foreach (int i, CLW_SetupParam param : list)
		{
			if (i > 0)
				json += ",";

			json += "\"" + CLW_ModeParam.Escape(param.m_sId) + "\":";
			if (param.m_iType == CLW_ModeParam.TYPE_CHOICE)
			{
				json += "\"" + CLW_ModeParam.Escape(ChoiceText(param)) + "\"";
			}
			else if (param.m_iType == CLW_ModeParam.TYPE_BOOL)
			{
				bool checked = param.m_fValue >= 0.5;
				SCR_CheckboxComponent check = SCR_CheckboxComponent.Cast(param.m_Comp);
				if (check)
					checked = check.IsChecked();
				json += CLW_ModeParam.BoolText(checked);
			}
			else
			{
				float value = param.m_fValue;
				SCR_SliderComponent slider = SCR_SliderComponent.Cast(param.m_Comp);
				if (slider)
					value = slider.GetValue();

				if (param.m_iType == CLW_ModeParam.TYPE_INT)
					json += Math.Round(value).ToString();
				else
					json += (Math.Round(value * 1000) / 1000).ToString();
			}
		}

		return json + "}";
	}

	// ---- feedback from the server (via CLW_SetupClient) ----

	void SetStatus(string text, bool isError)
	{
		SetMessage(text);
		if (isError)
			SetMessageColor(Color.FromSRGBA(240, 90, 70, 255));
		else
			SetMessageColor(Color.FromSRGBA(220, 220, 220, 255));
	}

	void OnServerResult(bool ok, string text)
	{
		m_bSubmitting = false;
		if (ok)
			SetStatus(CLW_Loc.Fmt("#CLW-Setup_Starting"), false);
		else
			SetStatus(CLW_Loc.F("#CLW-Setup_Rejected", text), true);
	}
}
