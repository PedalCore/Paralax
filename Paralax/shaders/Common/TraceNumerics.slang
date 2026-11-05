module TraceNumerics;

// Import only into optional kernels after checking SupportsFloat32RTE on the host. The caller must
// invoke this in every optional entry point; native Slang emits the mode on that actual entry point.
public void RequireTraceRTE32()
{
	spirv_asm
	{
		OpCapability RoundingModeRTE;
		OpExecutionMode __entryPoint RoundingModeRTE 32;
	};
}

public struct TraceInterval
{
	public float Lo;
	public float Hi;
};

public struct TraceBox
{
	public TraceInterval X;
	public TraceInterval Y;
	public TraceInterval Z;
};

public TraceInterval TraceExact(float value) { return { value, value }; }

public TraceInterval TraceWrap(float value)
{
	uint bits = asuint(value);
	if ((bits & 0x7fffffffu) <= 0x00800000u)
		return { asfloat(0x80800001u), asfloat(0x00800001u) };
	bool negative = (bits & 0x80000000u) != 0;
	return { asfloat(negative ? bits + 1u : bits - 1u), asfloat(negative ? bits - 1u : bits + 1u) };
}

public TraceInterval TraceAdd(TraceInterval a, TraceInterval b)
{
	precise float lo = a.Lo + b.Lo;
	precise float hi = a.Hi + b.Hi;
	return { TraceWrap(lo).Lo, TraceWrap(hi).Hi };
}

public TraceInterval TraceSubtract(TraceInterval a, TraceInterval b)
{
	precise float lo = a.Lo - b.Hi;
	precise float hi = a.Hi - b.Lo;
	return { TraceWrap(lo).Lo, TraceWrap(hi).Hi };
}

public TraceInterval TraceMultiply(TraceInterval a, TraceInterval b)
{
	precise float aa = a.Lo * b.Lo;
	precise float ab = a.Lo * b.Hi;
	precise float ba = a.Hi * b.Lo;
	precise float bb = a.Hi * b.Hi;
	// Widen before extrema: a flushed product may otherwise change which endpoint wins.
	TraceInterval p = TraceWrap(aa), q = TraceWrap(ab), r = TraceWrap(ba), s = TraceWrap(bb);
	return { min(min(p.Lo, q.Lo), min(r.Lo, s.Lo)), max(max(p.Hi, q.Hi), max(r.Hi, s.Hi)) };
}

public TraceInterval TraceAffineRow(TraceBox box, float4 row)
{
	return TraceAdd(TraceAdd(TraceAdd(TraceMultiply(TraceExact(row.x), box.X), TraceMultiply(TraceExact(row.y), box.Y)), TraceMultiply(TraceExact(row.z), box.Z)), TraceExact(row.w));
}

public float TraceNominalRow(float3 point, float4 row)
{
	precise float x = point.x * row.x;
	precise float y = point.y * row.y;
	precise float z = point.z * row.z;
	precise float xy = x + y;
	precise float xyz = xy + z;
	precise float result = xyz + row.w;
	return result;
}

public TraceBox TraceTransformBox(TraceBox box, float4 rowX, float4 rowY, float4 rowZ)
{
	return { TraceAffineRow(box, rowX), TraceAffineRow(box, rowY), TraceAffineRow(box, rowZ) };
}

public TraceInterval TraceUnion(TraceInterval a, TraceInterval b) { return { min(a.Lo, b.Lo), max(a.Hi, b.Hi) }; }
public TraceBox TraceUnion(TraceBox a, TraceBox b) { return { TraceUnion(a.X, b.X), TraceUnion(a.Y, b.Y), TraceUnion(a.Z, b.Z) }; }

public bool TraceSourceScalar(float value, uint limit)
{
	uint bits = asuint(value) & 0x7fffffffu;
	return bits == 0u || (bits >= 0x00800000u && bits <= limit);
}

// 0 certified, 1 bypass ALL bounds, 2 invalid. A finite unsupported query must never enter only part
// of a hierarchy: this global decision precedes every node/candidate gate.
public uint TraceClassifyQuery(float3 anchor, float3 offset, float3 direction, float maximum)
{
	bool finite = (asuint(maximum) & 0x7fffffffu) < 0x7f800000u;
	bool eligible = TraceSourceScalar(maximum, 0x4a800000u);
	for (uint i = 0; i < 3; ++i)
	{
		finite = finite && (asuint(anchor[i]) & 0x7fffffffu) < 0x7f800000u && (asuint(offset[i]) & 0x7fffffffu) < 0x7f800000u && (asuint(direction[i]) & 0x7fffffffu) < 0x7f800000u;
		eligible = eligible && TraceSourceScalar(anchor[i], 0x4a800000u) && TraceSourceScalar(offset[i], 0x4a800000u) && TraceSourceScalar(direction[i], 0x40000000u);
	}
	if (!finite)
		return 2;
	return eligible && maximum >= 0.0 ? 0 : 1;
}

public bool TraceCoordinateOverlap(TraceInterval box, float direction, float maximum)
{
	TraceInterval end = TraceMultiply(TraceExact(maximum), TraceExact(direction));
	return !(box.Hi < min(0.0, end.Lo) || box.Lo > max(0.0, end.Hi));
}

public bool TraceProjectionContainsZero(TraceBox box, float3 axis)
{
	TraceInterval dot = TraceAdd(TraceAdd(TraceMultiply(TraceExact(axis.x), box.X), TraceMultiply(TraceExact(axis.y), box.Y)), TraceMultiply(TraceExact(axis.z), box.Z));
	return !(dot.Hi < 0.0 || dot.Lo > 0.0);
}

// Closed segment gate, using ORIGINAL maximum. Preconditions are whole-source and whole-query
// admission, plus a box built by the shared transform/union. No per-node fallback or closest pruning.
public bool TraceSegmentBox(TraceBox box, float3 anchor, float3 offset, float3 direction, float maximum)
{
	TraceBox relative = {
		TraceSubtract(TraceSubtract(box.X, TraceExact(anchor.x)), TraceExact(offset.x)),
		TraceSubtract(TraceSubtract(box.Y, TraceExact(anchor.y)), TraceExact(offset.y)),
		TraceSubtract(TraceSubtract(box.Z, TraceExact(anchor.z)), TraceExact(offset.z))
	};
	return TraceCoordinateOverlap(relative.X, direction.x, maximum)
		&& TraceCoordinateOverlap(relative.Y, direction.y, maximum)
		&& TraceCoordinateOverlap(relative.Z, direction.z, maximum)
		&& TraceProjectionContainsZero(relative, float3(0, direction.z, -direction.y))
		&& TraceProjectionContainsZero(relative, float3(-direction.z, 0, direction.x))
		&& TraceProjectionContainsZero(relative, float3(direction.y, -direction.x, 0));
}
