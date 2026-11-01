module ProbeGI;

public struct ProbeGIConstants
{
	public float4 Origin; // xyz minimum probe centre, w enabled
	public float4 Spacing; // xyz metres between probes, w maximum trace distance
	public uint4 Grid; // xyz probe counts, w total probes
	public uint4 Update; // first probe, probe count this frame, rays per probe, deterministic frame index
	public uint4 Scene; // node count, light count, unused, unused
	public float4 Tuning; // normal bias, view bias, hysteresis, maximum visibility distance
};

public float2 ProbeOctEncode(float3 direction)
{
	direction /= max(abs(direction.x) + abs(direction.y) + abs(direction.z), 1e-8);
	float2 uv = direction.xy;
	if (direction.z < 0.0)
		uv = (1.0 - abs(uv.yx)) * float2(uv.x >= 0.0 ? 1.0 : -1.0, uv.y >= 0.0 ? 1.0 : -1.0);
	return uv * 0.5 + 0.5;
}

public float3 ProbeOctDecode(float2 uv)
{
	float2 p = uv * 2.0 - 1.0;
	float3 direction = float3(p, 1.0 - abs(p.x) - abs(p.y));
	float t = saturate(-direction.z);
	direction.xy += float2(direction.x >= 0.0 ? -t : t, direction.y >= 0.0 ? -t : t);
	return normalize(direction);
}

public float3 ProbePosition(uint index, ProbeGIConstants params)
{
	uint3 cell = uint3(index % params.Grid.x, (index / params.Grid.x) % params.Grid.y, index / (params.Grid.x * params.Grid.y));
	return params.Origin.xyz + float3(cell) * params.Spacing.xyz;
}

// RGBA32_FLOAT supports storage and sampled loads on Vulkan 1.3, but linear filtering is optional.
// Four explicit taps preserve full precision for squared distance moments on every supported adapter.
float4 SampleProbeTile(Texture2DArray<float4> texture, float2 uv, uint layer)
{
	float2 pixel = uv * 8.0 - 0.5;
	int2 base = int2(floor(pixel));
	float2 alpha = frac(pixel);
	int2 low = clamp(base, int2(0), int2(7));
	int2 high = clamp(base + 1, int2(0), int2(7));
	return lerp(lerp(texture.Load(int4(low, int(layer), 0)), texture.Load(int4(high.x, low.y, int(layer), 0)), alpha.x),
		lerp(texture.Load(int4(low.x, high.y, int(layer), 0)), texture.Load(int4(high, int(layer), 0)), alpha.x), alpha.y);
}

// RGB is irradiance / PI; alpha says whether the position belongs to this volume. A volume with no visible,
// initialized probes returns darkness rather than substituting unoccluded sky through enclosing walls.
public float4 SampleProbeGI(float3 position, float3 normal, float3 view, ProbeGIConstants params,
	Texture2DArray<float4> irradiance, Texture2DArray<float4> distance)
{
	if (params.Origin.w == 0.0)
		return float4(0.0);
	float3 gridPosition = (position - params.Origin.xyz) / params.Spacing.xyz;
	if (any(gridPosition < 0.0) || any(gridPosition > float3(params.Grid.xyz - 1)))
		return float4(0.0);
	float3 biased = position + normal * params.Tuning.x + view * params.Tuning.y;
	uint3 base = uint3(min(floor(gridPosition), float3(params.Grid.xyz - 2)));
	float3 alpha = saturate(gridPosition - float3(base));
	float3 sum = float3(0.0);
	float weightSum = 0.0;
	for (uint corner = 0; corner < 8; ++corner)
	{
		uint3 offset = uint3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1);
		uint3 cell = base + offset;
		uint index = cell.x + params.Grid.x * (cell.y + params.Grid.y * cell.z);
		float3 probe = ProbePosition(index, params);
		float3 delta = biased - probe;
		float lengthToProbe = length(delta);
		float3 direction = delta / max(lengthToProbe, 1e-6);
		float2 distanceUV = (ProbeOctEncode(direction) * 6.0 + 1.0) / 8.0;
		float4 moments = SampleProbeTile(distance, distanceUV, index);
		if (moments.w < 0.5)
			continue;
		float variance = max(moments.y - moments.x * moments.x, 1e-5);
		float difference = max(lengthToProbe - moments.x, 0.0);
		float visibility = variance / (variance + difference * difference);
		visibility *= visibility * visibility;
		float3 tri = lerp(1.0 - alpha, alpha, float3(offset));
		float facing = saturate(dot(normal, normalize(probe - position + normal * 1e-6)) * 0.5 + 0.5);
		float weight = tri.x * tri.y * tri.z * (facing * facing + 0.01) * visibility;
		float2 irradianceUV = (ProbeOctEncode(normal) * 6.0 + 1.0) / 8.0;
		sum += SampleProbeTile(irradiance, irradianceUV, index).rgb * weight;
		weightSum += weight;
	}
	return float4(weightSum > 1e-5 ? sum / weightSum : float3(0.0), 1.0);
}
