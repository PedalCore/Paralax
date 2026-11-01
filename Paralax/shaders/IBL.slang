// Environment lighting preparation (compute): procedural sky, equirectangular import, GGX specular prefiltering,
// diffuse irradiance convolution and the split-sum BRDF lookup table. Cubemaps are written as 6-layer arrays.
import Common.CubeMap;
import Common.PBR;

struct IBLConstants
{
	float4 SunDirection;   // xyz: direction towards the sun; w: sun disk intensity
	float4 SkyParams;      // x: sky intensity, y: roughness (prefilter), z: face size, w: sample count
	float4 SunColor;
	uint MipLevel;
	uint SourceMipCount;
	float SourceSize;
	float Padding;
};
[[vk::push_constant]] ConstantBuffer<IBLConstants> g_IBL;

RWTexture2DArray<float4> g_OutputCube : register(u0);
RWTexture2D<float2> g_OutputLut : register(u1);
TextureCube<float4> g_SourceCube : register(t0);
Texture2D<float4> g_SourceEquirect : register(t1);
SamplerState g_LinearClamp : register(s0);
SamplerState g_LinearWrapU : register(s1);

// A simple physically-inspired clear sky: Rayleigh-like gradient, horizon glow, ground and a sun disk.
float3 ProceduralSky(float3 direction)
{
	float3 sun = normalize(g_IBL.SunDirection.xyz);
	float height = direction.y;
	float sunAmount = saturate(dot(direction, sun));

	float3 zenith = float3(0.08, 0.28, 0.80);
	float3 horizon = float3(0.75, 0.88, 1.10);
	// Cool, low-albedo ground bounce keeps downward reflections neutral instead of tinting them brown.
	float3 ground = float3(0.12, 0.16, 0.22);

	float3 sky = lerp(horizon, zenith, pow(saturate(height), 0.45));
	// Warm scattering around the sun, stronger near the horizon.
	sky += g_IBL.SunColor.rgb * 0.35 * pow(sunAmount, 8.0) * (1.0 - 0.6 * saturate(height));
	// Below the horizon fade from the horizon colour to the ground smoothly (no hard dark band).
	float3 color = height >= 0.0 ? sky : lerp(horizon, ground, smoothstep(0.0, 0.45, -height));

	// Sun disk with soft edge (only above the horizon).
	float disk = smoothstep(0.9995, 0.99985, sunAmount) * step(0.0, height);
	color += g_IBL.SunColor.rgb * disk * g_IBL.SunDirection.w;
	return color * g_IBL.SkyParams.x;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ProceduralSkyCS(uint3 id: SV_DispatchThreadID)
{
	float size = g_IBL.SkyParams.z;
	if (id.x >= uint(size) || id.y >= uint(size))
		return;
	float3 direction = CubeTexelDirection(id.z, float2(id.xy), size);
	g_OutputCube[id] = float4(ProceduralSky(direction), 1.0);
}

// Builds mip N of a cube from mip N-1: sampling the previous level bilinearly at this texel's centre averages the 2x2
// texels beneath it, which preserves the energy of small bright features (the sun) across the chain.
[shader("compute")]
[numthreads(8, 8, 1)]
void DownsampleCubeCS(uint3 id: SV_DispatchThreadID)
{
	float size = g_IBL.SkyParams.z;
	if (id.x >= uint(size) || id.y >= uint(size))
		return;
	float3 direction = CubeTexelDirection(id.z, float2(id.xy), size);
	g_OutputCube[id] = g_SourceCube.SampleLevel(g_LinearClamp, direction, 0.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void EquirectToCubeCS(uint3 id: SV_DispatchThreadID)
{
	float size = g_IBL.SkyParams.z;
	if (id.x >= uint(size) || id.y >= uint(size))
		return;
	float3 direction = CubeTexelDirection(id.z, float2(id.xy), size);
	// Pick the equirect mip whose texel density matches this cube level (SourceSize = equirect width).
	float lod = max(0.0, log2(g_IBL.SourceSize / (4.0 * size)));
	float3 color = g_SourceEquirect.SampleLevel(g_LinearWrapU, DirectionToEquirect(direction), lod).rgb;
	// Clamp extreme values (sun pixels in HDRIs) to keep the prefiltered maps free of fireflies.
	g_OutputCube[id] = float4(min(color, float3(64.0)) * g_IBL.SkyParams.x, 1.0);
}

// GGX prefilter with mip-filtered importance sampling (Karis 2013, Colbert & Krivanek 2007).
[shader("compute")]
[numthreads(8, 8, 1)]
void PrefilterSpecularCS(uint3 id: SV_DispatchThreadID)
{
	float size = g_IBL.SkyParams.z;
	if (id.x >= uint(size) || id.y >= uint(size))
		return;

	float3 N = CubeTexelDirection(id.z, float2(id.xy), size);
	float roughness = g_IBL.SkyParams.y;
	if (roughness <= 0.0)
	{
		g_OutputCube[id] = g_SourceCube.SampleLevel(g_LinearClamp, N, 0.0);
		return;
	}

	uint sampleCount = uint(g_IBL.SkyParams.w);
	float texelSolidAngle = 4.0 * PI / (6.0 * g_IBL.SourceSize * g_IBL.SourceSize);
	float3 color = float3(0.0);
	float weight = 0.0;
	for (uint i = 0; i < sampleCount; i++)
	{
		float2 xi = Hammersley(i, sampleCount);
		float3 H = ImportanceSampleGGX(xi, N, roughness);
		float3 L = normalize(2.0 * dot(N, H) * H - N);
		float NdotL = dot(N, L);
		if (NdotL <= 0.0)
			continue;
		float NdotH = saturate(dot(N, H));
		float pdf = DistributionGGX(NdotH, roughness) * 0.25;
		float sampleSolidAngle = 1.0 / (float(sampleCount) * pdf + 1e-6);
		float mip = clamp(0.5 * log2(sampleSolidAngle / texelSolidAngle) + 1.0, 0.0, float(g_IBL.SourceMipCount - 1));
		color += g_SourceCube.SampleLevel(g_LinearClamp, L, mip).rgb * NdotL;
		weight += NdotL;
	}
	g_OutputCube[id] = float4(color / max(weight, 1e-6), 1.0);
}

// Cosine-weighted hemisphere convolution (diffuse irradiance / PI, i.e. ready to multiply by albedo).
[shader("compute")]
[numthreads(8, 8, 1)]
void IrradianceCS(uint3 id: SV_DispatchThreadID)
{
	float size = g_IBL.SkyParams.z;
	if (id.x >= uint(size) || id.y >= uint(size))
		return;

	float3 N = CubeTexelDirection(id.z, float2(id.xy), size);
	float3 up = abs(N.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
	float3 right = normalize(cross(up, N));
	up = cross(N, right);

	const uint samples = 1024;
	float3 irradiance = float3(0.0);
	for (uint i = 0; i < samples; i++)
	{
		float2 xi = Hammersley(i, samples);
		float phi = 2.0 * PI * xi.x;
		float cosTheta = sqrt(1.0 - xi.y);
		float sinTheta = sqrt(xi.y);
		float3 L = normalize(sinTheta * cos(phi) * right + sinTheta * sin(phi) * up + cosTheta * N);
		irradiance += g_SourceCube.SampleLevel(g_LinearClamp, L, 4.0).rgb;
	}
	// Cosine-weighted sampling: the estimator of (1/PI) * integral(L cos) is the plain average.
	g_OutputCube[id] = float4(irradiance / float(samples), 1.0);
}

float GeometrySmithIBL(float NdotV, float NdotL, float roughness)
{
	float k = roughness * roughness / 2.0;
	float gv = NdotV / (NdotV * (1.0 - k) + k);
	float gl = NdotL / (NdotL * (1.0 - k) + k);
	return gv * gl;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void BRDFLutCS(uint3 id: SV_DispatchThreadID)
{
	const float size = 128.0;
	if (id.x >= uint(size) || id.y >= uint(size))
		return;
	float NdotV = max((float(id.x) + 0.5) / size, 1e-3);
	float roughness = (float(id.y) + 0.5) / size;
	float3 V = float3(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
	float3 N = float3(0.0, 0.0, 1.0);

	const uint samples = 512;
	float a = 0.0;
	float b = 0.0;
	for (uint i = 0; i < samples; i++)
	{
		float3 H = ImportanceSampleGGX(Hammersley(i, samples), N, roughness);
		float3 L = normalize(2.0 * dot(V, H) * H - V);
		float NdotL = saturate(L.z);
		float NdotH = saturate(H.z);
		float VdotH = saturate(dot(V, H));
		if (NdotL > 0.0)
		{
			float G = GeometrySmithIBL(NdotV, NdotL, roughness);
			float Gv = G * VdotH / max(NdotH * NdotV, 1e-6);
			float Fc = pow(1.0 - VdotH, 5.0);
			a += (1.0 - Fc) * Gv;
			b += Fc * Gv;
		}
	}
	g_OutputLut[id.xy] = float2(a, b) / float(samples);
}
