module ProbeGI;

public struct ProbeGIConstants
{
	public float4 Origin; // xyz minimum probe centre, w enabled
	public float4 Spacing; // xyz metres between probes, w maximum trace distance
	public uint4 Grid; // xyz probe counts, w total probes
	public uint4 Update; // first probe, probe count this frame, rays per probe, deterministic frame index
	public uint4 Scene; // node count, light count, distance tile size, unused
	public float4 Tuning; // normal bias, view bias, hysteresis, maximum visibility distance
	public float4 Diagnostics; // selected probe (-1 none), visibility enabled, bounce feedback enabled, unused
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
float4 SampleProbeTile(Texture2DArray<float4> texture, float2 uv, uint layer, uint size = 8)
{
	float2 pixel = uv * float(size) - 0.5;
	int2 base = int2(floor(pixel));
	float2 alpha = frac(pixel);
	int2 low = clamp(base, int2(0), int2(size - 1));
	int2 high = clamp(base + 1, int2(0), int2(size - 1));
	return lerp(lerp(texture.Load(int4(low, int(layer), 0)), texture.Load(int4(high.x, low.y, int(layer), 0)), alpha.x),
		lerp(texture.Load(int4(low.x, high.y, int(layer), 0)), texture.Load(int4(high, int(layer), 0)), alpha.x), alpha.y);
}

public struct ProbeSamplingPoint
{
	public bool Inside;
	public uint3 Base;
	public float3 Alpha;
	public float3 Position;
	public float3 Biased;
	public float3 Normal;
};

public struct ProbeNeighbour
{
	public uint Index;
	public float3 Position;
	public float Distance;
	public float4 Moments;
	public float RawVariance;
	public float Variance;
	public float ExpectedMass;
	public float Confidence;
	public float Trilinear;
	public float Facing;
	public float Visibility;
	public float Support;
	public float EligibleSupport;
	public float Weight;
};

public ProbeSamplingPoint PrepareProbeSample(float3 position, float3 normal, float3 view, ProbeGIConstants params, float viewDistance = 1000000.0)
{
	ProbeSamplingPoint point = {};
	if (params.Origin.w == 0.0)
		return point;
	float3 gridPosition = (position - params.Origin.xyz) / params.Spacing.xyz;
	if (any(gridPosition < 0.0) || any(gridPosition > float3(params.Grid.xyz - 1)))
		return point;
	point.Inside = true;
	point.Position = position;
	point.Normal = normal;
	// The view ray is known to be clear up to its camera/probe origin. Do not move past that
	// endpoint when the configured spacing-relative bias is longer than a near-surface ray.
	float viewBias = min(params.Tuning.y, max(viewDistance, 0.0) * 0.5);
	point.Biased = position + normal * params.Tuning.x + view * viewBias;
	gridPosition = clamp((point.Biased - params.Origin.xyz) / params.Spacing.xyz, float3(0.0), float3(params.Grid.xyz - 1));
	point.Base = uint3(min(floor(gridPosition), float3(params.Grid.xyz - 2)));
	point.Alpha = saturate(gridPosition - float3(point.Base));
	return point;
}

// Shared by shading and on-demand GPU inspection so diagnostic weights describe the actual reconstruction.
public ProbeNeighbour GetProbeNeighbour(ProbeSamplingPoint point, uint corner, ProbeGIConstants params,
	Texture2DArray<float4> distance)
{
	ProbeNeighbour neighbour = {};
	uint3 offset = uint3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1);
	uint3 cell = point.Base + offset;
	neighbour.Index = cell.x + params.Grid.x * (cell.y + params.Grid.y * cell.z);
	neighbour.Position = ProbePosition(neighbour.Index, params);
	float3 delta = point.Biased - neighbour.Position;
	neighbour.Distance = length(delta);
	float3 direction = delta / max(neighbour.Distance, 1e-6);
	uint distanceSize = max(params.Scene.z, 8);
	float2 distanceUV = (ProbeOctEncode(direction) * float(distanceSize - 2) + 1.0) / float(distanceSize);
	neighbour.Moments = SampleProbeTile(distance, distanceUV, neighbour.Index, distanceSize);
	if (neighbour.Moments.z > 0.0)
		neighbour.Moments.xy /= neighbour.Moments.z;
	else
		neighbour.Moments.w = 0.0;
	float interiorSize = float(distanceSize - 2);
	float exponent = log(0.5) / log(1.0 - 2.0 / (interiorSize * interiorSize));
	neighbour.ExpectedMass = 1.0 / (2.0 * (exponent + 1.0));
	neighbour.Confidence = saturate(neighbour.Moments.z / neighbour.ExpectedMass);
	neighbour.RawVariance = neighbour.Moments.y - neighbour.Moments.x * neighbour.Moments.x;
	neighbour.Variance = max(neighbour.RawVariance, 1e-5);
	float difference = max(neighbour.Distance - neighbour.Moments.x, 0.0);
	float visibility = neighbour.Variance / (neighbour.Variance + difference * difference);
	visibility *= visibility * visibility;
	neighbour.Visibility = params.Diagnostics.y > 0.5 ? visibility : 1.0;
	float3 tri = lerp(1.0 - point.Alpha, point.Alpha, float3(offset));
	neighbour.Trilinear = tri.x * tri.y * tri.z;
	float3 toProbe = neighbour.Position - point.Position;
	float facing = saturate(dot(point.Normal, toProbe / max(length(toProbe), 1e-6)) * 0.5 + 0.5);
	neighbour.Facing = facing * facing + 0.01;
	neighbour.Support = neighbour.Trilinear * neighbour.Facing;
	neighbour.EligibleSupport = neighbour.Moments.w >= 0.5 ? neighbour.Support * neighbour.Confidence : 0.0;
	neighbour.Weight = neighbour.EligibleSupport * neighbour.Visibility;
	return neighbour;
}

public float3 ProbeDirectionalIrradiance(float3 normal, uint index, Texture2DArray<float4> irradiance)
{
	float2 uv = (ProbeOctEncode(normal) * 6.0 + 1.0) / 8.0;
	return SampleProbeTile(irradiance, uv, index).rgb;
}

// Visibility normally selects reliable probes without dimming the remaining visible neighbours. When all
// eligible support is occluded, normalized selection alone would amplify leakage back to full brightness.
// A black prior limits that amplification to 20x, fading below 5% mean visibility. Separately, support below
// 0.01% of the cell fades out rather than amplifying an arbitrarily small active corner at a cell boundary.
// These are dimensionless confidence policies, not floating-point epsilons. True trilinear zeros are retained.
public float ProbeNormalization(float weightSum, float eligibleSupport, float support)
{
	const float MinimumMeanVisibility = 0.05;
	const float MinimumEligibleFraction = 0.0001;
	float denominator = max(weightSum, max(MinimumMeanVisibility * eligibleSupport, MinimumEligibleFraction * support));
	return denominator > 0.0 ? 1.0 / denominator : 0.0;
}

// RGB is irradiance / PI; alpha says whether the position belongs to this volume. A volume with no visible,
// initialized probes returns darkness rather than substituting unoccluded sky through enclosing walls.
public float4 SampleProbeGI(float3 position, float3 normal, float3 view, ProbeGIConstants params,
	Texture2DArray<float4> irradiance, Texture2DArray<float4> distance, float viewDistance = 1000000.0)
{
	ProbeSamplingPoint point = PrepareProbeSample(position, normal, view, params, viewDistance);
	if (!point.Inside)
		return float4(0.0);
	float3 sum = float3(0.0);
	float weightSum = 0.0, eligibleSupport = 0.0, support = 0.0;
	for (uint corner = 0; corner < 8; ++corner)
	{
		ProbeNeighbour neighbour = GetProbeNeighbour(point, corner, params, distance);
		sum += ProbeDirectionalIrradiance(normal, neighbour.Index, irradiance) * neighbour.Weight;
		weightSum += neighbour.Weight;
		eligibleSupport += neighbour.EligibleSupport;
		support += neighbour.Support;
	}
	return float4(sum * ProbeNormalization(weightSum, eligibleSupport, support), 1.0);
}
