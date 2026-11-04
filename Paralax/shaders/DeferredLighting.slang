import Common.SceneBindings;
import Common.PBR;
import Common.SurfaceContract;
import Common.SurfacePacking;
import Common.SurfaceLighting;

Texture2D<float4> g_BaseMetal : register(t0, space1);
Texture2D<float4> g_Normals : register(t1, space1);
Texture2D<float4> g_ReceiverRoughAO : register(t2, space1);
Texture2D<float> g_Depth : register(t3, space1);

[shader("vertex")] float4 ResolveVS(uint vertexID : SV_VertexID)
	: SV_Position
{
	float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
	return float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}

MaterialSurface LoadMaterial(float4 pixel)
{
	int3 location = int3(int2(pixel.xy), 0);
	float4 receiver = g_ReceiverRoughAO.Load(location);
	if (receiver.z == 0.0)
		discard;
	float depth = g_Depth.Load(location);
	float2 ndc = pixel.xy / g_View.ViewportSize * float2(2.0, -2.0) + float2(-1.0, 1.0);
	float4 world = mul(g_View.InverseViewProjection, float4(ndc, depth, 1.0));
	float3 position = world.xyz / world.w;
	float4 baseMetal = g_BaseMetal.Load(location);
	float4 normals = g_Normals.Load(location);
	MaterialSurface material;
	material.Surface.Position = position;
	material.Surface.View = normalize(g_View.CameraPosition.xyz - position);
	material.Surface.Normal = DecodeSurfaceNormal(normals.xy);
	material.Surface.BaseColor = baseMetal.rgb;
	material.Surface.Metallic = baseMetal.a;
	material.Surface.Roughness = receiver.z;
	material.Surface.Occlusion = receiver.w;
	material.Surface.Emissive = float3(0.0);
	material.Surface.Alpha = 1.0;
	material.ShadowNormal = DecodeSurfaceNormal(normals.zw);
	material.ReceiverNormal = DecodeSurfaceNormal(receiver.xy);
	return material;
}

[shader("fragment")] float4 ResolvePS(float4 pixel : SV_Position) : SV_Target
{
	MaterialSurface material = LoadMaterial(pixel);
	return ShadeSurface(material, pixel.xy, -mul(g_View.View, float4(material.Surface.Position, 1.0)).z);
}

[shader("fragment")] ReflectionLightingTargets ReflectionPS(float4 pixel : SV_Position)
{
	MaterialSurface material = LoadMaterial(pixel);
	ReflectionLightingTargets output;
	float3 specular;
	output.Color = ShadeSurfaceWithSpecular(material, pixel.xy, -mul(g_View.View, float4(material.Surface.Position, 1.0)).z, true, specular);
	output.EnvironmentSpecular = float4(specular, 1);
	return output;
}
