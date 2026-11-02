// X-ray probe centres remain visible through walls for placement diagnostics; scene depth is untouched.
struct MarkerConstants
{
	float4x4 ViewProjection;
	float4 Origin;
	float4 Spacing;
	uint4 Grid;
	float4 Viewport;
};
[[vk::push_constant]] ConstantBuffer<MarkerConstants> g_Marker;
Texture2DArray<float4> g_Distance : register(t0);
struct MarkerVertex
{
	float4 Position : SV_Position;
	float2 Local : TEXCOORD0;
	nointerpolation float3 Color : COLOR0;
};
[shader("vertex")]
MarkerVertex MarkerVS(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
	const float2 corners[6] = {float2(-1,-1), float2(1,-1), float2(1,1), float2(-1,-1), float2(1,1), float2(-1,1)};
	uint3 cell = uint3(instance % g_Marker.Grid.x, (instance / g_Marker.Grid.x) % g_Marker.Grid.y, instance / (g_Marker.Grid.x * g_Marker.Grid.y));
	float3 position = g_Marker.Origin.xyz + float3(cell) * g_Marker.Spacing.xyz;
	bool selected = g_Marker.Viewport.z >= 0 && instance == uint(g_Marker.Viewport.z);
	MarkerVertex output;
	output.Position = mul(g_Marker.ViewProjection, float4(position, 1));
	output.Local = corners[vertex];
	output.Position.xy += output.Local * (selected ? 12.0 : 7.0) / g_Marker.Viewport.xy * output.Position.w;
	bool active = g_Distance.Load(int4(1, 1, instance, 0)).w >= 0.5;
	output.Color = selected ? float3(1, 0.8, 0) : active ? float3(0.05, 0.8, 0.1) : float3(0.9, 0.02, 0.02);
	return output;
}
[shader("fragment")]
float4 MarkerPS(MarkerVertex input) : SV_Target
{
	if (dot(input.Local, input.Local) > 1.0)
		discard;
	return float4(input.Color, 1);
}
