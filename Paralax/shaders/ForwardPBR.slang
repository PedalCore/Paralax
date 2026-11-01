// Forward PBR shading: metallic-roughness materials, parallax occlusion mapping, cascaded soft shadows for the sun,
// point/spot lights and image-based lighting. Writes linear HDR radiance.
import Common.SceneData;
import Common.PBR;

// Set 0: per view.
ConstantBuffer<ViewConstants> g_View : register(b0, space0);
StructuredBuffer<GPULight> g_Lights : register(t0, space0);
Texture2DArray<float> g_ShadowMap : register(t1, space0);
TextureCube<float4> g_SpecularEnvironment : register(t2, space0);
TextureCube<float4> g_DiffuseEnvironment : register(t3, space0);
Texture2D<float2> g_BRDFLut : register(t4, space0);
SamplerState g_LinearWrap : register(s0, space0);
SamplerState g_LinearClamp : register(s1, space0);
SamplerComparisonState g_ShadowSampler : register(s2, space0);

// Set 1: per material.
ConstantBuffer<MaterialConstants> g_Material : register(b0, space1);
Texture2D<float4> g_BaseColorTexture : register(t0, space1);
Texture2D<float4> g_MetallicRoughnessTexture : register(t1, space1);
Texture2D<float4> g_NormalTexture : register(t2, space1);
Texture2D<float4> g_OcclusionTexture : register(t3, space1);
Texture2D<float4> g_EmissiveTexture : register(t4, space1);
Texture2D<float4> g_HeightTexture : register(t5, space1);

[[vk::push_constant]] ConstantBuffer<DrawConstants> g_Draw;

struct VertexInput
{
	float3 Position : POSITION;
	float3 Normal : NORMAL;
	float4 Tangent : TANGENT;
	float2 UV : TEXCOORD0;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	float3 WorldPosition : POSITION0;
	float3 Normal : NORMAL;
	float4 Tangent : TANGENT;
	float2 UV : TEXCOORD0;
	float ViewDepth : TEXCOORD1;
};

float3 TransformNormal(float3 n)
{
	return float3(dot(g_Draw.NormalMatrix0.xyz, n), dot(g_Draw.NormalMatrix1.xyz, n), dot(g_Draw.NormalMatrix2.xyz, n));
}

[shader("vertex")]
VertexOutput MainVS(VertexInput input)
{
	VertexOutput output;
	float4 world = mul(g_Draw.World, float4(input.Position, 1.0));
	output.WorldPosition = world.xyz;
	output.Position = mul(g_View.ViewProjection, world);
	output.Normal = TransformNormal(input.Normal);
	// NormalMatrix0.w is the sign of the world matrix determinant: mirroring flips the bitangent.
	output.Tangent = float4(normalize(mul((float3x3)g_Draw.World, input.Tangent.xyz)), input.Tangent.w * g_Draw.NormalMatrix0.w);
	output.UV = input.UV;
	output.ViewDepth = -mul(g_View.View, world).z;
	return output;
}

bool HasTexture(uint bit)
{
	return (g_Material.TextureFlags & (1u << bit)) != 0;
}

// Steep parallax occlusion mapping with linear search plus interpolation between the bracketing layers.
// viewTS points from the surface towards the camera, in tangent space.
float2 ParallaxOcclusion(float2 uv, float3 viewTS)
{
	const float minLayers = 8.0;
	const float maxLayers = 32.0;
	float layers = lerp(maxLayers, minLayers, saturate(abs(viewTS.z)));
	float layerDepth = 1.0 / layers;
	// B points towards decreasing v (glTF convention), so marching away from the viewer moves +v by V.B.
	float2 shift = float2(viewTS.x, -viewTS.y) / max(viewTS.z, 0.05) * g_Material.HeightScale / layers;

	float2 dx = ddx(uv);
	float2 dy = ddy(uv);
	float currentDepth = 0.0;
	float2 currentUV = uv;
	float surfaceDepth = 1.0 - g_HeightTexture.SampleGrad(g_LinearWrap, currentUV, dx, dy).r;

	[loop]
	for (int i = 0; i < 32 && currentDepth < surfaceDepth; i++)
	{
		currentUV -= shift;
		currentDepth += layerDepth;
		surfaceDepth = 1.0 - g_HeightTexture.SampleGrad(g_LinearWrap, currentUV, dx, dy).r;
	}

	float2 previousUV = currentUV + shift;
	float after = surfaceDepth - currentDepth;
	float before = (1.0 - g_HeightTexture.SampleGrad(g_LinearWrap, previousUV, dx, dy).r) - (currentDepth - layerDepth);
	float weight = after / (after - before + 1e-6);
	return lerp(currentUV, previousUV, saturate(weight));
}

static const float2 PoissonDisk[16] = {
	float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725), float2(-0.09418410, -0.92938870), float2(0.34495938, 0.29387760),
	float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464), float2(-0.38277543, 0.27676845), float2(0.97484398, 0.75648379),
	float2(0.44323325, -0.97511554), float2(0.53742981, -0.47373420), float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
	float2(-0.24188840, 0.99706507), float2(-0.81409955, 0.91437590), float2(0.19984126, 0.78641367), float2(0.14383161, -0.14100790),
};

float InterleavedGradientNoise(float2 pixel)
{
	return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// Normal-offset biasing and a rotated 16-tap Poisson PCF, evaluated independently in each cascade's space.
float SampleSunCascade(uint cascade, float3 worldPosition, float3 normal, float3 receiverNormal, float2 pixel)
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

	// Shadow maps use standard Z (0 near the light) and NVRHI's comparison sampler tests ref < stored: lit when the
	// biased fragment depth is in front of the occluder.
	float depth = lightNDC.z - g_View.ShadowParams.y;
	float angle = InterleavedGradientNoise(pixel) * 6.28318530718;
	float2x2 rotation = float2x2(cos(angle), -sin(angle), sin(angle), cos(angle));
	float radius = max(g_View.ShadowParams.w / texelWorld, 1.0) * g_View.ShadowParams.x;

	// Compare each tap against the receiver plane at that UV, not the center depth. Otherwise a wide
	// world-space filter shadows its own sloping receiver and creates cascade-dependent speckling.
	float3 tangent = cross(receiverNormal, abs(receiverNormal.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0));
	float3 bitangent = cross(receiverNormal, tangent);
	float3 lightTangent = mul(g_View.CascadeViewProjection[cascade], float4(tangent, 0)).xyz;
	float3 lightBitangent = mul(g_View.CascadeViewProjection[cascade], float4(bitangent, 0)).xyz;
	float3 plane = cross(lightTangent, lightBitangent);
	float2 depthGradient = abs(plane.z) > 1e-4 * length(plane) ? float2(-2.0 * plane.x, 2.0 * plane.y) / plane.z : float2(0, 0);

	// Hardware PCF compares the four texel centers around each tap with a single reference depth.
	depth -= dot(abs(depthGradient), float2(0.5, 0.5) * g_View.ShadowParams.x);

	float lit = 0.0;
	[unroll]
	for (int i = 0; i < 16; i++)
	{
		float2 offset = mul(rotation, PoissonDisk[i]) * radius;
		lit += g_ShadowMap.SampleCmpLevelZero(g_ShadowSampler, float3(shadowUV + offset, float(cascade)), depth + dot(depthGradient, offset));
	}
	return lit / 16.0;
}

float SunShadow(float3 worldPosition, float3 normal, float3 receiverNormal, float viewDepth, float2 pixel)
{
	if (g_View.SunColor.w < 0.5 || viewDepth >= g_View.CascadeSplits[MaxCascades - 1])
		return 1.0;

	uint cascade = 0;
	while (cascade + 1 < MaxCascades && viewDepth >= g_View.CascadeSplits[cascade])
		cascade++;
	float shadow = SampleSunCascade(cascade, worldPosition, normal, receiverNormal, pixel);
	float start = cascade == 0 ? g_View.CascadeNear : g_View.CascadeSplits[cascade - 1];
	float end = g_View.CascadeSplits[cascade];
	float blendWidth = (end - start) * g_View.CascadeBlend;
	if (blendWidth > 0.0 && viewDepth > end - blendWidth)
	{
		// The last cascade fades out too, so the shadow distance never introduces another hard seam.
		float next = cascade + 1 < MaxCascades ? SampleSunCascade(cascade + 1, worldPosition, normal, receiverNormal, pixel) : 1.0;
		shadow = lerp(shadow, next, smoothstep(end - blendWidth, end, viewDepth));
	}
	return shadow;
}

float3 RotateY(float3 v, float angle)
{
	float s = sin(angle);
	float c = cos(angle);
	return float3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

float3 EvaluateEnvironment(SurfaceData surface)
{
	float intensity = g_View.EnvironmentParams.x;
	if (intensity <= 0.0)
		return float3(0.0);

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

	// Multiple-scattering energy compensation (Fdez-Aguera 2019): rough metals keep their brightness.
	float3 singleScatter = F0 * brdf.x + brdf.y;
	float3 energyCompensation = 1.0 + F0 * (1.0 / max(brdf.x + brdf.y, 1e-4) - 1.0);
	float3 specular = prefiltered * singleScatter * energyCompensation;
	// Specular occlusion (Lagarde & de Rousiers 2014) from ambient occlusion.
	float specularOcclusion = saturate(pow(NdotV + surface.Occlusion, exp2(-16.0 * surface.Roughness - 1.0)) - 1.0 + surface.Occlusion);
	float3 kd = (1.0 - F) * (1.0 - surface.Metallic);
	float3 diffuse = kd * irradiance * surface.BaseColor;
	return (diffuse * surface.Occlusion + specular * specularOcclusion) * intensity;
}

[shader("fragment")]
float4 MainPS(VertexOutput input, bool isFrontFace: SV_IsFrontFace) : SV_Target
{
	float3 N = normalize(input.Normal);
	if (!isFrontFace)
		N = -N;
	// Derivatives must be evaluated before discard or cascade selection; interpolated shading normals do not
	// describe the rasterized triangle's receiver plane (notably on smooth spheres and normal-mapped meshes).
	float3 receiverNormal = cross(ddx(input.WorldPosition), ddy(input.WorldPosition));
	float receiverLength = length(receiverNormal);
	receiverNormal = receiverLength > 1e-12 ? receiverNormal / receiverLength : N;
	// Keep the geometric shading normal for shadow bias before material normal mapping changes N.
	float3 shadowNormal = N;
	float3 T = normalize(input.Tangent.xyz - N * dot(N, input.Tangent.xyz));
	float3 B = cross(N, T) * input.Tangent.w;
	float3 V = normalize(g_View.CameraPosition.xyz - input.WorldPosition);

	float2 uv = input.UV;
	if (HasTexture(5))
	{
		float3 viewTS = float3(dot(V, T), dot(V, B), dot(V, N));
		uv = ParallaxOcclusion(uv, viewTS);
	}

	float4 baseColor = g_Material.BaseColorFactor;
	if (HasTexture(0))
		baseColor *= g_BaseColorTexture.Sample(g_LinearWrap, uv);

	if (g_Material.AlphaMode == 1 && baseColor.a < g_Material.AlphaCutoff)
		discard;

	float metallic = g_Material.MetallicFactor;
	float roughness = g_Material.RoughnessFactor;
	if (HasTexture(1))
	{
		float4 mr = g_MetallicRoughnessTexture.Sample(g_LinearWrap, uv);
		roughness *= mr.g;
		metallic *= mr.b;
	}

	if (HasTexture(2))
	{
		float3 tangentNormal = g_NormalTexture.Sample(g_LinearWrap, uv).xyz * 2.0 - 1.0;
		tangentNormal.xy *= g_Material.NormalScale;
		N = normalize(tangentNormal.x * T + tangentNormal.y * B + tangentNormal.z * N);
	}

	float occlusion = 1.0;
	if (HasTexture(3))
		occlusion = lerp(1.0, g_OcclusionTexture.Sample(g_LinearWrap, uv).r, g_Material.OcclusionStrength);

	float3 emissive = g_Material.EmissiveFactor.rgb;
	if (HasTexture(4))
		emissive *= g_EmissiveTexture.Sample(g_LinearWrap, uv).rgb;

	SurfaceData surface;
	surface.Position = input.WorldPosition;
	surface.Normal = N;
	surface.View = V;
	surface.BaseColor = baseColor.rgb;
	surface.Metallic = saturate(metallic);
	surface.Roughness = clamp(roughness, 0.045, 1.0);
	surface.Occlusion = occlusion;
	surface.Emissive = emissive;
	surface.Alpha = baseColor.a;

	float3 color = surface.Emissive;

	if (g_View.SunDirection.w > 0.5)
	{
		float shadow = SunShadow(surface.Position, shadowNormal, receiverNormal, input.ViewDepth, input.Position.xy);
		color += EvaluateLight(surface, -g_View.SunDirection.xyz, g_View.SunColor.rgb * shadow);
	}

	for (uint i = 0; i < g_View.LightCount; i++)
	{
		GPULight light = g_Lights[i];
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
		color += EvaluateLight(surface, L, light.ColorIntensity.rgb * attenuation);
	}

	color += EvaluateEnvironment(surface);

	if (g_View.DebugView == 1)
		color = N * 0.5 + 0.5;
	else if (g_View.DebugView == 2)
		color = surface.BaseColor;

	return float4(color, g_Material.AlphaMode == 2 ? surface.Alpha : 1.0);
}
