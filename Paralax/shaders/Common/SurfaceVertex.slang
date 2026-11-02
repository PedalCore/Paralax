// Shared clip-space and tangent-frame construction for raster material paths.
module SurfaceVertex;
import Common.SceneBindings;
public
struct VertexInput
{
public
	float3 Position : POSITION;
public
	float3 Normal : NORMAL;
public
	float4 Tangent : TANGENT;
public
	float2 UV : TEXCOORD0;
};

public
struct VertexOutput
{
public
	float4 Position : SV_Position;
public
	float3 WorldPosition : POSITION0;
public
	float3 Normal : NORMAL;
public
	float4 Tangent : TANGENT;
public
	float2 UV : TEXCOORD0;
public
	float ViewDepth : TEXCOORD1;
};

float3 TransformNormal(float3 n)
{
	return float3(dot(g_Draw.NormalMatrix0.xyz, n), dot(g_Draw.NormalMatrix1.xyz, n), dot(g_Draw.NormalMatrix2.xyz, n));
}

public
VertexOutput TransformVertex(VertexInput input)
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
