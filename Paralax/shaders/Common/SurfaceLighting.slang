// Shared direct, shadow, probe and environment lighting plus diagnostic decomposition.
module SurfaceLighting;
import Common.ClusteredLighting;
import Common.LightData;
import Common.SceneBindings;
import Common.SceneData;
import Common.SurfaceContract;
import Common.PBR;
import Common.ProbeGI;

static const float2 PoissonDisk[16] = {
	float2(-0.94201624, -0.39906216),
	float2(0.94558609, -0.76890725),
	float2(-0.09418410, -0.92938870),
	float2(0.34495938, 0.29387760),
	float2(-0.91588581, 0.45771432),
	float2(-0.81544232, -0.87912464),
	float2(-0.38277543, 0.27676845),
	float2(0.97484398, 0.75648379),
	float2(0.44323325, -0.97511554),
	float2(0.53742981, -0.47373420),
	float2(-0.26496911, -0.41893023),
	float2(0.79197514, 0.19090188),
	float2(-0.24188840, 0.99706507),
	float2(-0.81409955, 0.91437590),
	float2(0.19984126, 0.78641367),
	float2(0.14383161, -0.14100790),
};

float InterleavedGradientNoise(float2 pixel)
{
	return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// Normal-offset biasing and a rotated 16-tap Poisson PCF, evaluated independently in each cascade's space.
float SampleSunCascade(uint cascade, float3 worldPosition, float3 normal, float3 receiverNormal, float2x2 rotation)
{
	// Normal offset and filter radius are expressed per cascade in world units, so the bias and the penumbra size
	// match across cascade boundaries.
	float texelWorld = g_View.CascadeTexelWorldSize[cascade];
	float3 offsetPosition = worldPosition + normal * g_View.ShadowParams.z * texelWorld;
	float4 lightClip = mul(g_View.CascadeViewProjection[cascade], float4(offsetPosition, 1.0));
	float3 lightNDC = lightClip.xyz / lightClip.w;
	float2 shadowUV = float2(lightNDC.x * 0.5 + 0.5, 0.5 - lightNDC.y * 0.5);
	if (any(shadowUV < 0.0) || any(shadowUV > 1.0) || lightNDC.z < 0.0 || lightNDC.z > 1.0)
		return 1.0;

	// A disk in light UV extrapolates a grazing receiver plane arbitrarily far along the light direction,
	// potentially moving the comparison past a real blocker. Keep the entire disk on finite world-space support.
	float radius = max(g_View.ShadowParams.w, texelWorld);
	float3 tangent = normalize(cross(receiverNormal, abs(receiverNormal.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0)));
	float3 bitangent = cross(receiverNormal, tangent);
	uint width, height, layers;
	g_ShadowMap.GetDimensions(width, height, layers);

	float lit = 0.0;
	[unroll] for (int i = 0; i < 16; i++)
	{
		float2 disk = mul(rotation, PoissonDisk[i]);
		disk *= rsqrt(max(dot(disk, disk), 1.0));
		float3 samplePosition = offsetPosition + radius * (tangent * disk.x + bitangent * disk.y);
		float4 sampleClip = mul(g_View.CascadeViewProjection[cascade], float4(samplePosition, 1));
		float3 sampleNDC = sampleClip.xyz / sampleClip.w;
		float2 uv = float2(sampleNDC.x * 0.5 + 0.5, 0.5 - sampleNDC.y * 0.5);
		if (any(uv < 0.0) || any(uv > 1.0) || sampleNDC.z < 0.0 || sampleNDC.z > 1.0)
		{
			lit += 1.0;
			continue;
		}
		// Nearest texels keep the remaining raster footprint within half a texel per axis, covered by the
		// caster's slope bias. Bilinear comparisons would require another unbounded receiver-plane correction.
		int2 texel = min(int2(uv * float2(width, height)), int2(width - 1, height - 1));
		float stored = g_ShadowMap.Load(int4(texel, int(cascade), 0));
		// Preserve the comparison sampler's strict standard-Z reference < stored convention.
		lit += sampleNDC.z - g_View.ShadowParams.y < stored ? 1.0 : 0.0;
	}
	return lit / 16.0;
}

public
float SunShadow(float3 worldPosition, float3 normal, float3 receiverNormal, float viewDepth, float2x2 rotation)
{
	if (g_View.SunColor.w < 0.5 || viewDepth >= g_View.CascadeSplits[MaxCascades - 1])
		return 1.0;

	uint cascade = 0;
	while (cascade + 1 < MaxCascades && viewDepth >= g_View.CascadeSplits[cascade])
		cascade++;
	float shadow = SampleSunCascade(cascade, worldPosition, normal, receiverNormal, rotation);
	float start = cascade == 0 ? g_View.CascadeNear : g_View.CascadeSplits[cascade - 1];
	float end = g_View.CascadeSplits[cascade];
	float blendWidth = (end - start) * g_View.CascadeBlend;
	if (blendWidth > 0.0 && viewDepth > end - blendWidth)
	{
		// The last cascade fades out too, so the shadow distance never introduces another hard seam.
		float next = cascade + 1 < MaxCascades ? SampleSunCascade(cascade + 1, worldPosition, normal, receiverNormal, rotation) : 1.0;
		shadow = lerp(shadow, next, smoothstep(end - blendWidth, end, viewDepth));
	}
	return shadow;
}

uint LocalShadowFace(float3 direction)
{
	float3 magnitude = abs(direction);
	if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z)
		return direction.x >= 0.0 ? 0u : 1u;
	if (magnitude.y >= magnitude.z)
		return direction.y >= 0.0 ? 2u : 3u;
	return direction.z >= 0.0 ? 4u : 5u;
}

float LocalShadow(GPULight light, float3 position, float3 normal, float3 receiverNormal, float2x2 rotation)
{
	if (light.Params.y < 0.5)
		return 1.0;
	LocalShadowSlot slot = g_LocalShadowSlots[uint(light.Params.y) - 1];
	float3 fromLight = position - light.PositionRange.xyz;
	float distance = slot.Atlas.y > 1.5 ? length(fromLight) : mul(slot.ViewProjection[0], float4(position, 1)).w;
	// For a spot, bound displacement after perspective division, including the change in w.
	// The 24-texel projection guard contains the eight-tap-radius + one normal-offset texel.
	float perspectiveBound = slot.Atlas.y > 1.5 ? 1.0 : 1.0 + slot.Atlas.w;
	float texelWorld = max(distance, slot.Bias.x) * 2.0 * slot.Atlas.w * slot.Atlas.z / perspectiveBound;
	float3 biased = position + normal * slot.Bias.w * texelWorld;
	float radius = clamp(slot.Bias.z, texelWorld, 8.0 * texelWorld);
	float3 tangent = normalize(cross(receiverNormal, abs(receiverNormal.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0)));
	float3 bitangent = cross(receiverNormal, tangent);
	uint width, height, layers;
	g_LocalShadowMap.GetDimensions(width, height, layers);
	float lit = 0.0;
	[unroll] for (int tap = 0; tap < 16; ++tap)
	{
		float2 disk = PoissonDisk[tap] / max(1.0, length(PoissonDisk[tap]));
		float2 offset = mul(rotation, disk) * radius;
		float3 samplePosition = biased + tangent * offset.x + bitangent * offset.y;
		uint face = slot.Atlas.y > 1.5 ? LocalShadowFace(samplePosition - light.PositionRange.xyz) : 0u;
		float4x4 matrix = slot.ViewProjection[face];
		float4 clip = mul(matrix, float4(samplePosition, 1));
		if (clip.w <= 0.0 || clip.z > clip.w || clip.z < 0.0)
		{
			lit += 1.0;
			continue;
		}
		float3 ndc = clip.xyz / clip.w;
		float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
		if (any(uv < 0.0) || any(uv > 1.0))
		{
			lit += 1.0;
			continue;
		}
		// Keep the reference at this finite world point. The bilinear half-texel correction could otherwise
		// jump nearby blockers at grazing angles despite the bounded disk. Nearest sampling uses the caster bias.
		int2 texel = min(int2(uv * float2(width, height)), int2(width - 1, height - 1));
		float stored = g_LocalShadowMap.Load(int4(texel, int(slot.Atlas.x) + int(face), 0));
		// Invert strict ref < stored for reversed Z, preserving equality and the clear depth of zero.
		lit += ndc.z >= stored ? 1.0 : 0.0;
	}
	return lit / 16.0;
}

float3 RotateY(float3 v, float angle)
{
	float s = sin(angle);
	float c = cos(angle);
	return float3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

struct EnvironmentContribution
{
	float3 Diffuse;
	float3 Specular;
	float3 Indirect;
	float Coverage;
};

// Orthographic rays have a common direction and a view origin at infinity: no distance clamp.
float4 ProbeCameraRay(float3 position)
{
	if (abs(g_View.Projection[3][3]) > 0.5)
		return float4(normalize(g_View.View[2].xyz), 1000000.0);
	float3 delta = g_View.CameraPosition.xyz - position;
	float distance = length(delta);
	return float4(delta / max(distance, 1e-6), distance);
}

EnvironmentContribution EvaluateEnvironment(SurfaceData surface)
{
	EnvironmentContribution result = {};
	float intensity = g_View.EnvironmentParams.x;
	if (intensity <= 0.0 && g_Probe.Origin.w == 0.0)
		return result;

	float3 N = surface.Normal;
	float3 V = surface.View;
	float NdotV = max(dot(N, V), 1e-4);
	float3 R = reflect(-V, N);
	float rotation = g_View.EnvironmentParams.z;

	float3 F0 = lerp(float3(0.04), surface.BaseColor, surface.Metallic);
	float3 F = FresnelSchlickRoughness(NdotV, F0, surface.Roughness);
	float2 brdf = g_BRDFLut.SampleLevel(g_LinearClamp, float2(NdotV, surface.Roughness), 0.0);

	float mip = surface.Roughness * (g_View.EnvironmentParams.y - 1.0);
	float3 prefiltered = g_SpecularEnvironment.SampleLevel(g_LinearClamp, RotateY(R, rotation), mip).rgb;
	float3 irradiance = g_DiffuseEnvironment.SampleLevel(g_LinearClamp, RotateY(N, rotation), 0.0).rgb;

	float3 kd = (1.0 - F) * (1.0 - surface.Metallic);
	float3 diffuse = kd * irradiance * surface.BaseColor;
	float4 probeView = ProbeCameraRay(surface.Position);
	float4 indirect = SampleProbeGI(surface.Position, N, probeView.xyz, g_Probe, g_ProbeIrradiance, g_ProbeDistance, probeView.w);
	result.Coverage = indirect.a;
	result.Specular = EvaluateIndirectSpecular(prefiltered, F0, brdf, NdotV, surface.Roughness, surface.Occlusion) * intensity * g_View.Diagnostics.x;
	if (indirect.a > 0.5)
		result.Indirect = kd * indirect.rgb * surface.BaseColor * surface.Occlusion;
	else
		result.Diffuse = diffuse * surface.Occlusion * intensity;
	return result;
}

public
float4 ShadeSurfaceWithSpecular(MaterialSurface material, float2 pixel, float viewDepth, bool screenAOEligible, out float3 environmentSpecular)
{
	SurfaceData surface = material.Surface;
	float3 N = surface.Normal;
	float3 shadowNormal = material.ShadowNormal;
	float3 receiverNormal = material.ReceiverNormal;

	float angle = InterleavedGradientNoise(pixel) * 6.28318530718;
	float2x2 shadowRotation = float2x2(cos(angle), -sin(angle), sin(angle), cos(angle));
	float localVisibility = 1.0;
	float3 directDiffuse = float3(0.0), directSpecular = float3(0.0);

	if (g_View.SunDirection.w > 0.5)
	{
		float shadow = SunShadow(surface.Position, shadowNormal, receiverNormal, viewDepth, shadowRotation);
		LightContribution contribution = EvaluateLightTerms(surface, -g_View.SunDirection.xyz, g_View.SunColor.rgb * shadow);
		directDiffuse += contribution.Diffuse;
		directSpecular += contribution.Specular;
	}

	uint cell = ClusterLookup(g_View.ViewProjection, g_View.View, g_View.Clusters, surface.Position);
	ClusterLightIterator lightIterator = ClusterBeginLights(cell);
	uint i;
	while (ClusterNextLight(g_LightData, g_View.LightCount, lightIterator, i))
	{
		GPULight light = LoadRawLight(g_LightData, i);
		float3 toLight = light.PositionRange.xyz - surface.Position;
		float distanceSquared = dot(toLight, toLight);
		if (distanceSquared > light.PositionRange.w * light.PositionRange.w)
			continue;
		float3 L = toLight * rsqrt(max(distanceSquared, 1e-8));
		float attenuation = DistanceAttenuation(distanceSquared, light.PositionRange.w);
		if (light.ColorIntensity.w > 0.5)
		{
			float cd = dot(-L, light.DirectionCos.xyz);
			float cone = saturate((cd - light.DirectionCos.w) / max(light.Params.x - light.DirectionCos.w, 1e-4));
			attenuation *= cone * cone;
		}
		float visibility = attenuation > 0.0 ? LocalShadow(light, surface.Position, shadowNormal, receiverNormal, shadowRotation) : 1.0;
		localVisibility = min(localVisibility, visibility);
		LightContribution contribution = EvaluateLightTerms(surface, L, light.ColorIntensity.rgb * attenuation * visibility);
		directDiffuse += contribution.Diffuse;
		directSpecular += contribution.Specular;
	}

	EnvironmentContribution environment = EvaluateEnvironment(surface);
	environmentSpecular = environment.Specular;
	float screenAO = 1;
	if (g_View.ScreenEffects.x > 0.5 && screenAOEligible)
	{
		screenAO = g_ScreenAO.Load(int3(int2(pixel), 0));
		environment.Diffuse *= screenAO;
		environment.Indirect *= screenAO;
	}
	float3 fill = float3(0);
	if (g_View.AmbientFill.w > 0)
	{
		float3 F0 = lerp(float3(0.04), surface.BaseColor, surface.Metallic);
		float3 F = FresnelSchlickRoughness(max(dot(N, surface.View), 1e-4), F0, surface.Roughness);
		float3 kd = (1 - F) * (1 - surface.Metallic);
		fill = kd * surface.BaseColor * g_View.AmbientFill.rgb * g_View.AmbientFill.w * surface.Occlusion * screenAO;
	}
	float3 color = surface.Emissive + directDiffuse + directSpecular + environment.Diffuse + environment.Specular + environment.Indirect;

	if (g_View.AmbientFill.w > 0)
		color += fill;

	if (g_View.DebugView == 1)
		color = N * 0.5 + 0.5;
	else if (g_View.DebugView == 2)
		color = surface.BaseColor;
	else if (g_View.DebugView == 3 || g_View.DebugView == 10)
		color = environment.Indirect;
	else if (g_View.DebugView == 4)
		color = float3(environment.Coverage);
	else if (g_View.DebugView == 5)
		color = directDiffuse;
	else if (g_View.DebugView == 6)
		color = directSpecular;
	else if (g_View.DebugView == 7)
		color = environment.Diffuse;
	else if (g_View.DebugView == 8)
		color = environment.Specular;
	else if (g_View.DebugView == 9)
		color = surface.Emissive;
	else if (g_View.DebugView == 17)
		color = float3(localVisibility);
	else if (g_View.DebugView == 18)
		color = float3(screenAO);
	else if (g_View.DebugView == 19)
		color = fill;
	else if (g_View.DebugView >= 11)
	{
		float4 probeView = ProbeCameraRay(surface.Position);
		ProbeSamplingPoint point = PrepareProbeSample(surface.Position, N, probeView.xyz, g_Probe, probeView.w);
		float weightSum = 0.0, eligibleSupport = 0.0, support = 0.0, active = 0.0, selectedWeight = 0.0;
		float3 selectedMoments = float3(0.0);
		if (point.Inside)
			for (uint corner = 0; corner < 8; ++corner)
			{
				ProbeNeighbour neighbour = GetProbeNeighbour(point, corner, g_Probe, g_ProbeDistance);
				weightSum += neighbour.Weight;
				eligibleSupport += neighbour.EligibleSupport;
				support += neighbour.Support;
				active += neighbour.Moments.w >= 0.5 ? 1.0 : 0.0;
				if (g_Probe.Diagnostics.x >= 0.0 && neighbour.Index == uint(g_Probe.Diagnostics.x))
				{
					selectedWeight = neighbour.Weight;
					selectedMoments = float3(neighbour.Moments.xy, neighbour.Variance);
				}
			}
		if (g_View.DebugView == 11)
			color = float3(weightSum);
		else if (g_View.DebugView == 12)
			color = float3(active / 8.0);
		else if (g_View.DebugView == 13)
			color = float3(selectedWeight * ProbeNormalization(weightSum, eligibleSupport, support));
		else if (g_View.DebugView == 14)
			color = float3(selectedMoments.x);
		else if (g_View.DebugView == 15)
			color = float3(selectedMoments.y);
		else
			color = float3(selectedMoments.z);
	}

	return float4(color, surface.Alpha);
}

// The ordinary path preserves the same arithmetic; optional MRT callers retain the already evaluated term.
public
float4 ShadeSurface(MaterialSurface material, float2 pixel, float viewDepth, bool screenAOEligible = true)
{
	float3 unused;
	return ShadeSurfaceWithSpecular(material, pixel, viewDepth, screenAOEligible, unused);
}

public struct ReflectionLightingTargets
{
	public float4 Color : SV_Target0;
	public float4 EnvironmentSpecular : SV_Target1;
};
