// Vehicle kind (CLW_ForceKind string, or air_rotary / air_fixed) from the editor labels of a prefab: data driven, no prefab names,
// so RHS / WCS vehicles classify the same way (same labels as CLW_ForceCatalog discovery). Cached per prefab.
class CLW_FriendlyKinds
{
	static const string AIR_ROTARY = "air_rotary";
	static const string AIR_FIXED = "air_fixed";

	protected static ref map<string, string> s_mCache;

	static string OfPrefab(string prefabName)
	{
		if (prefabName.IsEmpty())
			return CLW_ForceKind.MOTORIZED;

		if (!s_mCache)
			s_mCache = new map<string, string>();

		string cached;
		if (s_mCache.Find(prefabName, cached))
			return cached;

		string kind = CLW_ForceKind.MOTORIZED;
		SCR_EditableEntityUIInfo info = SCR_EditableEntityUIInfo.ExtractEditableUIInfoFromPrefab(prefabName);
		if (info)
		{
			array<EEditableEntityLabel> labels = {};
			info.GetEntityLabels(labels);
			if (labels.Contains(EEditableEntityLabel.VEHICLE_HELICOPTER))
				kind = AIR_ROTARY;
			else if (labels.Contains(EEditableEntityLabel.VEHICLE_AIRPLANE))
				kind = AIR_FIXED;
			else if (labels.Contains(EEditableEntityLabel.VEHICLE_APC))
				kind = CLW_ForceKind.MECH;
			else if (labels.Contains(EEditableEntityLabel.TRAIT_ARMOR))
				kind = CLW_ForceKind.ARMOR;
			else if (labels.Contains(EEditableEntityLabel.VEHICLE_TRUCK) && labels.Contains(EEditableEntityLabel.TRAIT_LOGISTICS))
				kind = CLW_ForceKind.LOGISTICS;
			else if (labels.Contains(EEditableEntityLabel.VEHICLE_TRUCK))
				kind = CLW_ForceKind.MOTORIZED;
			else if (labels.Contains(EEditableEntityLabel.TRAIT_ARMED))
				kind = CLW_ForceKind.LIGHT;
		}

		s_mCache.Set(prefabName, kind);
		return kind;
	}

	static string OfVehicle(IEntity vehicle)
	{
		if (!vehicle)
			return CLW_ForceKind.MOTORIZED;

		EntityPrefabData data = vehicle.GetPrefabData();
		if (!data)
			return CLW_ForceKind.MOTORIZED;

		return OfPrefab(data.GetPrefabName());
	}
}
