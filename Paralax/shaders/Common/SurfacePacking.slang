// Three independent normals fit in two half-float attachments without substituting their semantics.
module SurfacePacking;
import Common.SurfaceContract;

public
float2 EncodeSurfaceNormal(float3 normal)
{
	float3 n = normal / (abs(normal.x) + abs(normal.y) + abs(normal.z));
	return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
}

public
float3 DecodeSurfaceNormal(float2 encoded)
{
	float3 n = float3(encoded, 1.0 - abs(encoded.x) - abs(encoded.y));
	float t = saturate(-n.z);
	n.xy += float2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
	return normalize(n);
}

public
struct SurfaceTargets
{
public
	float4 BaseMetal : SV_Target0;
public
	float4 Normals : SV_Target1;
public
	float4 ReceiverRoughAO : SV_Target2;
public
	float4 Emissive : SV_Target3;
};

public
SurfaceTargets PackSurface(MaterialSurface material, uint debugView)
{
	SurfaceTargets output;
	output.BaseMetal = float4(material.Surface.BaseColor, material.Surface.Metallic);
	output.Normals = float4(EncodeSurfaceNormal(material.Surface.Normal), EncodeSurfaceNormal(material.ShadowNormal));
	output.ReceiverRoughAO = float4(EncodeSurfaceNormal(material.ReceiverNormal), material.Surface.Roughness, material.Surface.Occlusion);
	// Lighting is added into the HDR attachment, avoiding a separate emissive texture and its bandwidth.
	output.Emissive = float4(debugView == 0 || debugView == 9 ? material.Surface.Emissive : float3(0.0), 1.0);
	return output;
}
