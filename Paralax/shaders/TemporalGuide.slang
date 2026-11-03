import Common.SurfaceVertex;
import Common.SurfaceMaterial;
import Common.SurfacePacking;
import Common.SceneBindings;
import Common.SurfaceContract;

struct PreviousDraw
{
	float4x4 Clip;
	float4x4 View;
	uint4 Identity;
};
StructuredBuffer<PreviousDraw> g_Previous : register(t0, space3);

struct MotionVertex
{
	float4 Position : SV_Position;
	float3 WorldPosition : POSITION0;
	float3 Normal : NORMAL;
	float4 Tangent : TANGENT;
	float2 UV : TEXCOORD0;
	float ViewDepth : TEXCOORD1;
	float4 PreviousClip : TEXCOORD2;
	float4 PreviousView : TEXCOORD3;
	nointerpolation uint2 Identity : TEXCOORD4;
};

[shader("vertex")] MotionVertex MotionVS(VertexInput input) {
	VertexOutput current = TransformVertex(input);
	PreviousDraw previous = g_Previous[g_Draw.Geometry.x];
	MotionVertex output;
	output.Position = current.Position;
	output.WorldPosition = current.WorldPosition;
	output.Normal = current.Normal;
	output.Tangent = current.Tangent;
	output.UV = current.UV;
	output.ViewDepth = current.ViewDepth;
	output.PreviousClip = mul(previous.Clip, float4(input.Position, 1));
	output.PreviousView = mul(previous.View, float4(input.Position, 1));
	output.Identity = previous.Identity.xy;
	return output;
}

VertexOutput MaterialVertex(MotionVertex input)
{
	VertexOutput result;
	result.Position = input.Position;
	result.WorldPosition = input.WorldPosition;
	result.Normal = input.Normal;
	result.Tangent = input.Tangent;
	result.UV = input.UV;
	result.ViewDepth = input.ViewDepth;
	return result;
}

struct MotionTargets
{
	float2 Normal : SV_Target0;
	uint ID : SV_Target1;
	float2 PreviousUV : SV_Target2;
	float PreviousDepth : SV_Target3;
};

[shader("fragment")] MotionTargets MotionPS(MotionVertex input, bool front : SV_IsFrontFace) {
	MaterialSurface material = EvaluateMaterial(MaterialVertex(input), front);
	MotionTargets result;
	result.Normal = EncodeSurfaceNormal(material.ReceiverNormal);
	bool valid = all(isfinite(input.PreviousClip)) && all(isfinite(input.PreviousView)) && input.PreviousClip.w > 1e-6;
	float3 previousNDC = valid ? input.PreviousClip.xyz / input.PreviousClip.w : float3(0);
	valid = valid && all(abs(previousNDC.xy) <= 1) && previousNDC.z >= 0 && previousNDC.z <= 1;
	result.ID = input.Identity.x | ((input.Identity.y != 0 || !valid) ? 0x80000000u : 0);
	result.PreviousUV = float2(previousNDC.x * 0.5 + 0.5, 0.5 - previousNDC.y * 0.5);
	result.PreviousDepth = valid ? -input.PreviousView.z : 0;
	return result;
}

	[shader("fragment")] float ReactivePS(MotionVertex input, bool front : SV_IsFrontFace)
	: SV_Target
{
	MaterialSurface material = EvaluateMaterial(MaterialVertex(input), front);
	if (material.Surface.Alpha <= 0)
		discard;
	return 1;
}
