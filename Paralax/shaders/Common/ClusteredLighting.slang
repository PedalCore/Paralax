module ClusteredLighting;

public struct ClusterGridConstants
{
	public uint4 Dimensions;
	public uint4 State;
	public float4 Depth[5];
};

public static const uint ClusterPrefixBytes = 16384;
public static const uint ClusterFallback = 0xffffffff;

// Bit tests precede arithmetic so FTZ/NotNaN cannot erase unsupported input cases.
public bool ClusterSupported(float value)
{
	uint bits = asuint(value) & 0x7fffffff;
	return bits == 0 || (bits >= 0x00800000 && bits <= asuint(1e10f));
}

public bool ClusterSupportedPosition(float3 value)
{
	return ClusterSupported(value.x) && ClusterSupported(value.y) && ClusterSupported(value.z);
}

struct Interval
{
	float Lo;
	float Hi;
};

Interval Exact(float value)
{
	Interval result;
	result.Lo = value;
	result.Hi = value;
	return result;
}

// Only scalar, correctly rounded add/multiply are used in certificates. precise emits
// NoContraction. Inputs are bounded by 1e10; intermediate magnitudes stay below 1e30.
// Widen subnormal endpoints to normal values, covering both input and output FTZ.
Interval Rounded(float value)
{
	uint bits = asuint(value);
	uint magnitude = bits & 0x7fffffff;
	Interval result;
	if (magnitude <= 0x00800000)
	{
		result.Lo = asfloat(0x80800001);
		result.Hi = asfloat(0x00800001);
	}
	else
	{
		result.Lo = asfloat((bits & 0x80000000) != 0 ? bits + 1 : bits - 1);
		result.Hi = asfloat((bits & 0x80000000) != 0 ? bits - 1 : bits + 1);
	}
	return result;
}

Interval Add(Interval a, Interval b)
{
	precise float lo = a.Lo + b.Lo;
	precise float hi = a.Hi + b.Hi;
	Interval result;
	result.Lo = Rounded(lo).Lo;
	result.Hi = Rounded(hi).Hi;
	return result;
}

Interval Multiply(Interval a, Interval b)
{
	precise float aa = a.Lo * b.Lo;
	precise float ab = a.Lo * b.Hi;
	precise float ba = a.Hi * b.Lo;
	precise float bb = a.Hi * b.Hi;
	Interval result;
	result.Lo = Rounded(min(min(aa, ab), min(ba, bb))).Lo;
	result.Hi = Rounded(max(max(aa, ab), max(ba, bb))).Hi;
	return result;
}

struct Plane
{
	Interval Coefficients[4];
};

Plane CombineRows(float4 a, float scaleA, float4 b, float scaleB)
{
	Plane result;
	for (uint i = 0; i < 4; ++i)
		result.Coefficients[i] = Add(Multiply(Exact(a[i]), Exact(scaleA)), Multiply(Exact(b[i]), Exact(scaleB)));
	return result;
}

Plane CellPlane(float4x4 vp, float4x4 view, ClusterGridConstants grid, uint3 cell, uint side)
{
	uint left = cell.x * 64;
	uint right = min(left + 64, grid.Dimensions.x);
	uint top = cell.y * 64;
	uint bottom = min(top + 64, grid.Dimensions.y);
	// Integer pixel factors are exactly representable; no division enters the certificate.
	if (side == 0)
		return CombineRows(vp[0], float(grid.Dimensions.x), vp[3], float(int(grid.Dimensions.x) - 2 * int(left)));
	if (side == 1)
		return CombineRows(vp[0], -float(grid.Dimensions.x), vp[3], float(2 * int(right) - int(grid.Dimensions.x)));
	if (side == 2)
		return CombineRows(vp[1], -float(grid.Dimensions.y), vp[3], float(int(grid.Dimensions.y) - 2 * int(top)));
	if (side == 3)
		return CombineRows(vp[1], float(grid.Dimensions.y), vp[3], float(2 * int(bottom) - int(grid.Dimensions.y)));
	if (side == 4)
		return CombineRows(view[2], -1, view[3], -grid.Depth[cell.z / 4][cell.z % 4]);
	uint end = cell.z + 1;
	return CombineRows(view[2], 1, view[3], grid.Depth[end / 4][end % 4]);
}

Interval Evaluate(Plane plane, float3 position, float radius)
{
	Interval result = plane.Coefficients[3];
	for (uint i = 0; i < 3; ++i)
	{
		Interval extent;
		precise float lo = position[i] - radius;
		precise float hi = position[i] + radius;
		extent.Lo = Rounded(lo).Lo;
		extent.Hi = Rounded(hi).Hi;
		result = Add(result, Multiply(plane.Coefficients[i], extent));
	}
	return result;
}

public bool ClusterLightMayIntersect(float4x4 vp, float4x4 view, ClusterGridConstants grid, uint3 cell, float4 light)
{
	if (!ClusterSupportedPosition(light.xyz) || !ClusterSupported(light.w))
		return true;
	precise float squared = light.w * light.w;
	uint squaredBits = asuint(squared) & 0x7fffffff;
	if (squaredBits < asuint(1e-20f) || squaredBits >= 0x7f800000)
		return true;
	// For a legacy accepted range predicate, basic subtraction/product/sum errors require
	// < 16 epsilon radial enlargement (including any contracted dot). Normal squared ranges
	// >=1e-20 squared ranges also dwarf additive FTZ errors in squared components/sums.
	// Smaller or unsupported ranges retain the light instead.
	precise float expanded = abs(light.w) * (1.0f + 16.0f * 1.1920928955078125e-7f);
	float radius = Rounded(expanded).Hi;
	for (uint side = 0; side < 6; ++side)
		if (Evaluate(CellPlane(vp, view, grid, cell, side), light.xyz, radius).Hi < 0)
			return false;
	return true;
}

public uint ClusterLookup(float4x4 vp, float4x4 view, ClusterGridConstants grid, float3 position)
{
	if (grid.State.y == 0 || !ClusterSupportedPosition(position))
		return ClusterFallback;
	Plane w = CombineRows(vp[3], 1, vp[3], 0);
	if (Evaluate(w, position, 0).Lo <= 0)
		return ClusterFallback;
	float4 clip = mul(vp, float4(position, 1));
	if (asuint(clip.w) < asuint(1e-10f) || asuint(clip.w) > asuint(1e21f))
		return ClusterFallback;
	float2 pixel = (clip.xy / clip.w * float2(0.5, -0.5) + 0.5) * float2(grid.Dimensions.xy);
	// Division is only a heuristic. Bit guards precede conversion, even if tiny W overflows.
	if (!ClusterSupported(pixel.x) || !ClusterSupported(pixel.y) || any(pixel < 0) || any(pixel >= float2(grid.Dimensions.xy)))
		return ClusterFallback;
	uint3 cell = uint3(uint2(pixel) / 64, 0);
	float depth = -mul(view, float4(position, 1)).z;
	for (uint i = 1; i < 16; ++i)
		if (depth >= grid.Depth[i / 4][i % 4])
			cell.z = i;
	for (uint side = 0; side < 6; ++side)
		if (Evaluate(CellPlane(vp, view, grid, cell, side), position, 0).Lo <= 0)
			return ClusterFallback;
	return (cell.z * grid.Dimensions.w + cell.y) * grid.Dimensions.z + cell.x;
}

public uint ClusterFullWord(uint count, uint word)
{
	uint first = word * 32;
	uint remaining = count > first ? min(count - first, 32u) : 0;
	return remaining == 32 ? 0xffffffff : (1u << remaining) - 1;
}

public uint ClusterLoadWord(ByteAddressBuffer data, uint count, uint cell, uint word)
{
	return cell == ClusterFallback ? ClusterFullWord(count, word) : data.Load(ClusterPrefixBytes + (cell * 8 + word) * 4);
}

// Stateful iterator keeps the original ascending accumulation order in both modes.
public struct ClusterLightIterator
{
	public uint Cell;
	public uint Word;
	public uint Mask;
	public uint Next;
};

public ClusterLightIterator ClusterBeginLights(uint cell)
{
	ClusterLightIterator result;
	result.Cell = cell;
	result.Word = 0;
	result.Mask = 0;
	result.Next = 0;
	return result;
}

public bool ClusterNextLight(ByteAddressBuffer data, uint count, inout ClusterLightIterator iterator, out uint index)
{
	if (iterator.Cell == ClusterFallback)
	{
		index = iterator.Next++;
		return index < count;
	}
	while (iterator.Mask == 0 && iterator.Word < 8)
		iterator.Mask = ClusterLoadWord(data, count, iterator.Cell, iterator.Word++);
	if (iterator.Mask == 0)
	{
		index = count;
		return false;
	}
	index = (iterator.Word - 1) * 32 + firstbitlow(iterator.Mask);
	iterator.Mask &= iterator.Mask - 1;
	return index < count;
}
