// Resolves what an LLM / rule mission offer names to a real objective (M4.6, "mission not completed after a capture").
// An offer that names its objective by display name ("Timber Hub") or only by a position near it used to become a bare positional
// offer, and a positional CAPTURE completes only when the leader stands within RECON_RADIUS_M of the point (the owner is ignored).
// Order: id (exact, then case-insensitive) -> display name (case-insensitive, then a unique "contains") -> position inside the
// capture radius + margin (nearest objective). Pure lookup, no side effects. Server only (reads CLW_ControlPoints).
class CLW_MissionTarget
{
	static const float POSITION_MARGIN_M = 100;	// a position this far outside the capture radius still counts as "at the objective"
	static const int MIN_PARTIAL_LENGTH = 4;		// shortest name fragment accepted for a "contains" match

	// target = id or name as the offer wrote it (may be empty), hasPos/x/z = position of the offer (world X, Z).
	// how = "id" | "name" | "position". Null when nothing matches (the offer then stays a bare position).
	static CLW_ControlPoint Resolve(string target, bool hasPos, float x, float z, out string how)
	{
		how = "";
		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		if (!points)
			return null;

		string wanted = target.Trim();
		if (!wanted.IsEmpty())
		{
			CLW_ControlPoint exact = points.Find(wanted);
			if (exact)
			{
				how = "id";
				return exact;
			}

			string lower = wanted;
			lower.ToLower();
			for (int i = 0; i < points.Count(); i++)
			{
				CLW_ControlPoint byId = points.Get(i);
				if (!byId)
					continue;

				string id = byId.m_sId;
				id.ToLower();
				if (id == lower)
				{
					how = "id";
					return byId;
				}
			}

			for (int n = 0; n < points.Count(); n++)
			{
				CLW_ControlPoint byName = points.Get(n);
				if (!byName)
					continue;

				string name = byName.m_sName;
				name.ToLower();
				if (!name.IsEmpty() && name == lower)
				{
					how = "name";
					return byName;
				}
			}

			CLW_ControlPoint partial = PartialName(points, lower);
			if (partial)
			{
				how = "name";
				return partial;
			}
		}

		if (!hasPos)
			return null;

		CLW_ControlPoint best;
		float bestDistance = 1000000;
		vector at = Vector(x, 0, z);
		for (int k = 0; k < points.Count(); k++)
		{
			CLW_ControlPoint near = points.Get(k);
			if (!near)
				continue;

			float distance = vector.DistanceXZ(at, near.m_vPos);
			if (distance <= near.m_fRadius + POSITION_MARGIN_M && distance < bestDistance)
			{
				bestDistance = distance;
				best = near;
			}
		}

		if (best)
			how = "position";

		return best;
	}

	// "Timber Hub (C)" / "capture Timber Hub": the offer text contains exactly one objective name (or one name contains the text).
	protected static CLW_ControlPoint PartialName(CLW_ControlPoints points, string lower)
	{
		if (lower.Length() < MIN_PARTIAL_LENGTH)
			return null;

		CLW_ControlPoint found;
		for (int i = 0; i < points.Count(); i++)
		{
			CLW_ControlPoint point = points.Get(i);
			if (!point)
				continue;

			string name = point.m_sName;
			name.ToLower();
			if (name.Length() < MIN_PARTIAL_LENGTH)
				continue;

			if (lower.Contains(name) || name.Contains(lower))
			{
				if (found)
					return null;	// ambiguous

				found = point;
			}
		}

		return found;
	}
}
