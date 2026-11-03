import Common.Fullscreen;

struct BloomConstants
{
	float4 SourceSize;
	float4 Parameters; // threshold, soft knee fraction, threshold enabled, unused
	float4 Weights;    // high-resolution and accumulated-low-resolution weights
};
[[vk::push_constant]] ConstantBuffer<BloomConstants> g_Bloom;
Texture2D<float4> g_High : register(t0);
Texture2D<float4> g_Low : register(t1);
SamplerState g_Linear : register(s0);

[shader("vertex")] FullscreenVertex BloomVS(uint vertex : SV_VertexID) {
	return MakeFullscreenVertex(vertex);
}

float RoundStorageChannel(float value)
{
	// Half stores and pack/unpack intrinsics may both use RTZ. Round the float32
	// representation to nearest-even explicitly, then store an exactly representable half.
	if (value < 0.00006103515625)
	{
		float units = value * 16777216.0;
		float integer = floor(units);
		float remainder = units - integer;
		if (remainder > 0.5 || (remainder == 0.5 && (uint(integer) & 1) != 0))
			integer += 1;
		return integer * 0.000000059604644775390625;
	}
	uint bits = asuint(value);
	bits = (bits + 0xfffu + ((bits >> 13) & 1u)) & 0xffffe000u;
	return asfloat(bits);
}

float3 RoundStorage(float3 value)
{
	return float3(RoundStorageChannel(value.x), RoundStorageChannel(value.y), RoundStorageChannel(value.z));
}

float3 Extract(float3 color)
{
	// Sanitize each source texel before filtering, so a nonfinite channel cannot poison neighbours.
	color = float3(isfinite(color.r) ? max(color.r, 0) : 0,
				   isfinite(color.g) ? max(color.g, 0) : 0, isfinite(color.b) ? max(color.b, 0) : 0);
	if (g_Bloom.Parameters.z < 0.5 || g_Bloom.Parameters.x == 0)
		return color;
	float brightness = dot(color, float3(0.2126, 0.7152, 0.0722));
	if (brightness == 0)
		return float3(0);
	float knee = g_Bloom.Parameters.x * g_Bloom.Parameters.y;
	float soft = clamp(brightness - g_Bloom.Parameters.x + knee, 0, 2 * knee);
	soft = knee > 0 ? soft * soft / (4 * knee) : 0;
	return color * (max(soft, brightness - g_Bloom.Parameters.x) / brightness);
}

[shader("fragment")] float4 DownsamplePS(FullscreenVertex input)
	: SV_Target
{
	// Integrate source texel areas over equal destination footprints. Ceil-half dimensions
	// need at most three texels per axis; odd borders preserve the image mean too.
	float2 footprint = g_Bloom.SourceSize.xy / g_Bloom.SourceSize.zw;
	float2 begin = floor(input.Position.xy) * footprint;
	float2 end = begin + footprint;
	int2 first = int2(floor(begin));
	float3 sum = float3(0);
	[unroll] for (uint y = 0; y < 3; ++y)
		[unroll] for (uint x = 0; x < 3; ++x)
	{
		int2 source = first + int2(x, y);
		float2 overlap = max(float2(0), min(end, float2(source + 1)) - max(begin, float2(source)));
		float weight = overlap.x * overlap.y / (footprint.x * footprint.y);
		if (weight > 0)
			sum += Extract(g_High.Load(int3(min(source, int2(g_Bloom.SourceSize.xy) - 1), 0)).rgb) * weight;
	}
	return float4(RoundStorage(sum), 1);
}

// Explicit float weights avoid device-dependent fixed-point sampler interpolation at odd footprints.
float3 ReconstructLow(float2 uv)
{
	float2 pixel = uv * g_Bloom.SourceSize.xy - 0.5;
	int2 first = int2(floor(pixel));
	float2 fraction = frac(pixel);
	float3 value = float3(0);
	[unroll] for (uint i = 0; i < 4; ++i)
	{
		int2 offset = int2(i & 1, i >> 1);
		float weight = (offset.x != 0 ? fraction.x : 1 - fraction.x) * (offset.y != 0 ? fraction.y : 1 - fraction.y);
		int2 sample = clamp(first + offset, int2(0), int2(g_Bloom.SourceSize.xy) - 1);
		value += g_Low.Load(int3(sample, 0)).rgb * weight;
	}
	return value;
}

[shader("fragment")] float4 UpsamplePS(FullscreenVertex input)
	: SV_Target
{
	float3 high = g_High.Load(int3(uint2(input.Position.xy), 0)).rgb;
	// Integrate the clamped bilinear reconstruction across the destination pixel area.
	// A footprint crosses at most one source centre per axis; midpoint quadrature is
	// exact on each bilinear piece and preserves the mean even at odd-sized borders.
	float2 footprint = g_Bloom.SourceSize.xy / g_Bloom.SourceSize.zw;
	float2 begin = floor(input.Position.xy) * footprint;
	float2 end = begin + footprint;
	float2 split = clamp(floor(begin - 0.5) + 1.5, begin, end);
	float3 low = float3(0);
	[unroll] for (uint i = 0; i < 4; ++i)
	{
		float2 lo = float2((i & 1) != 0 ? split.x : begin.x, (i & 2) != 0 ? split.y : begin.y);
		float2 hi = float2((i & 1) != 0 ? end.x : split.x, (i & 2) != 0 ? end.y : split.y);
		float2 extent = hi - lo;
		float weight = extent.x * extent.y / (footprint.x * footprint.y);
		if (weight > 0)
			low += ReconstructLow((lo + hi) * 0.5 / g_Bloom.SourceSize.xy) * weight;
	}
	// Aggregate level weights sum to one, so level count changes spread rather than constant-field gain.
	return float4(RoundStorage(high * g_Bloom.Weights.x + low * g_Bloom.Weights.y), 1);
}
