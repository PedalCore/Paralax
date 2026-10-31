// Draws the environment as the background where nothing else was rendered (depth == 0 with reversed Z).
import Common.SceneData;

ConstantBuffer<ViewConstants> g_View : register(b0, space0);
TextureCube<float4> g_RadianceEnvironment : register(t5, space0);
SamplerState g_LinearClamp : register(s1, space0);

struct SkyVertex
{
	float4 Position : SV_Position;
	float2 NDC : TEXCOORD0;
};

[shader("vertex")]
SkyVertex SkyVS(uint vertexID: SV_VertexID)
{
	float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
	SkyVertex output;
	output.NDC = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
	// Depth 0 = far plane in reversed Z, so the sky only covers pixels with no geometry (depth test GreaterOrEqual).
	output.Position = float4(output.NDC, 0.0, 1.0);
	return output;
}

float3 RotateY(float3 v, float angle)
{
	float s = sin(angle);
	float c = cos(angle);
	return float3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

[shader("fragment")]
float4 SkyPS(SkyVertex input) : SV_Target
{
	float4 world = mul(g_View.InverseViewProjection, float4(input.NDC, 0.5, 1.0));
	float3 direction = normalize(world.xyz / world.w - g_View.CameraPosition.xyz);
	float3 color = g_RadianceEnvironment.SampleLevel(g_LinearClamp, RotateY(direction, g_View.EnvironmentParams.z), 0.0).rgb;
	return float4(color * g_View.EnvironmentParams.x * g_View.EnvironmentParams.w, 1.0);
}
