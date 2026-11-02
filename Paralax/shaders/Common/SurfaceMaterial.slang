// Material sampling is shared by Forward and deferred geometry. POM remains UV-only.
module SurfaceMaterial;
import Common.SceneBindings;
import Common.MaterialBindings;
import Common.SurfaceVertex;
import Common.PBR;

import Common.SurfaceContract;

bool HasTexture(uint bit)
{
	return (g_Material.TextureFlags & (1u << bit)) != 0;
}

// Steep parallax occlusion mapping with linear search plus interpolation between the bracketing layers.
// viewTS points from the surface towards the camera, in tangent space.
float2 ParallaxOcclusion(float2 uv, float3 viewTS, float2 dx, float2 dy)
{
	const float minLayers = 8.0;
	const float maxLayers = 32.0;
	float layers = lerp(maxLayers, minLayers, saturate(abs(viewTS.z)));
	float layerDepth = 1.0 / layers;
	// B points towards decreasing v (glTF convention), so marching away from the viewer moves +v by V.B.
	float2 shift = float2(viewTS.x, -viewTS.y) / max(viewTS.z, 0.05) * g_Material.HeightScale / layers;

	float currentDepth = 0.0;
	float2 currentUV = uv;
	float surfaceDepth = 1.0 - g_HeightTexture.SampleGrad(g_LinearWrap, currentUV, dx, dy).r;

	[loop] for (int i = 0; i < 32 && currentDepth < surfaceDepth; i++)
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

public
MaterialSurface EvaluateMaterialWithFootprint(VertexOutput input, bool isFrontFace, float2 uvDx, float2 uvDy, float3 positionDx, float3 positionDy, bool applyMask)
{
	float3 N = normalize(input.Normal);
	if (!isFrontFace)
		N = -N;
	// Derivatives must be evaluated before discard or cascade selection; interpolated shading normals do not
	// describe the rasterized triangle's receiver plane (notably on smooth spheres and normal-mapped meshes).
	float3 receiverNormal = cross(positionDx, positionDy);
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
		if (!all(isfinite(uvDx)) || !all(isfinite(uvDy)))
		{
			uvDx = float2(65536.0, 0.0);
			uvDy = float2(0.0, 65536.0);
		}
		uv = ParallaxOcclusion(uv, viewTS, uvDx, uvDy);
		if (!all(isfinite(uv)))
			uv = all(isfinite(input.UV)) ? input.UV : input.Position.xy * 65536.0;
	}

	float4 baseColor = g_Material.BaseColorFactor;
	if (HasTexture(0))
		baseColor *= g_BaseColorTexture.Sample(g_LinearWrap, uv);

	if (applyMask && g_Material.AlphaMode == 1 && baseColor.a < g_Material.AlphaCutoff)
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

	MaterialSurface result;
	result.Surface = surface;
	result.Surface.Alpha = g_Material.AlphaMode == 2 ? surface.Alpha : 1.0;
	result.ShadowNormal = shadowNormal;
	result.ReceiverNormal = receiverNormal;
	return result;
}

public
MaterialSurface EvaluateMaterial(VertexOutput input, bool isFrontFace)
{
	return EvaluateMaterialWithFootprint(input, isFrontFace, ddx(input.UV), ddy(input.UV), ddx(input.WorldPosition), ddy(input.WorldPosition), true);
}
