// Physically based shading: GGX microfacet specular with Smith height-correlated visibility and Schlick Fresnel,
// Lambertian diffuse. All colors are linear.
module PBR;

public static const float PI = 3.14159265358979;

public float DistributionGGX(float NdotH, float roughness)
{
	float a = roughness * roughness;
	float a2 = a * a;
	float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
	return a2 / max(PI * d * d, 1e-7);
}

public float VisibilitySmithGGXCorrelated(float NdotV, float NdotL, float roughness)
{
	float a = roughness * roughness;
	float a2 = a * a;
	float ggxV = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
	float ggxL = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
	return 0.5 / max(ggxV + ggxL, 1e-7);
}

public float3 FresnelSchlick(float cosTheta, float3 F0)
{
	return F0 + (1.0 - F0) * pow(1.0 - saturate(cosTheta), 5.0);
}

public float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
	return F0 + (max(float3(1.0 - roughness), F0) - F0) * pow(1.0 - saturate(cosTheta), 5.0);
}

public struct SurfaceData
{
	public float3 Position;
	public float3 Normal;
	public float3 View;      // from surface to camera, normalized
	public float3 BaseColor;
	public float Metallic;
	public float Roughness;  // perceptual, clamped away from 0
	public float Occlusion;
	public float3 Emissive;
	public float Alpha;
};

public struct LightContribution
{
	public float3 Diffuse;
	public float3 Specular;
};

// Separate physical terms are also used by renderer diagnostics.
public LightContribution EvaluateLightTerms(SurfaceData surface, float3 L, float3 radiance)
{
	LightContribution result = {};
	float3 N = surface.Normal;
	float3 V = surface.View;
	float3 H = normalize(V + L);
	float NdotL = saturate(dot(N, L));
	if (NdotL <= 0.0)
		return result;
	float NdotV = max(dot(N, V), 1e-4);
	float NdotH = saturate(dot(N, H));
	float VdotH = saturate(dot(V, H));

	float3 F0 = lerp(float3(0.04), surface.BaseColor, surface.Metallic);
	float3 F = FresnelSchlick(VdotH, F0);
	float D = DistributionGGX(NdotH, surface.Roughness);
	float Vis = VisibilitySmithGGXCorrelated(NdotV, NdotL, surface.Roughness);

	float3 specular = D * Vis * F;
	float3 kd = (1.0 - F) * (1.0 - surface.Metallic);
	float3 diffuse = kd * surface.BaseColor / PI;
	result.Diffuse = diffuse * radiance * NdotL;
	result.Specular = specular * radiance * NdotL;
	return result;
}

// Smooth distance falloff that reaches exactly zero at the light's range (inverse-square in between).
public float DistanceAttenuation(float distanceSquared, float range)
{
	float factor = distanceSquared / max(range * range, 1e-4);
	float smoothFactor = saturate(1.0 - factor * factor);
	return smoothFactor * smoothFactor / max(distanceSquared, 1e-4);
}

// Shared split-sum material weighting; incident scene radiance is independent of environment intensity.
public float3 EvaluateIndirectSpecular(float3 radiance, float3 f0, float2 brdf, float nDotV, float roughness, float occlusion)
{
	float3 singleScatter = f0 * brdf.x + brdf.y;
	float3 energyCompensation = 1.0 + f0 * (1.0 / max(brdf.x + brdf.y, 1e-4) - 1.0);
	float3 specular = radiance * singleScatter * energyCompensation;
	float specularOcclusion = saturate(pow(nDotV + occlusion, exp2(-16.0 * roughness - 1.0)) - 1.0 + occlusion);
	return specular * specularOcclusion;
}
