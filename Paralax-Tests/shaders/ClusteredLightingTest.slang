module ClusteredLightingTest;
import Common.SceneData;
import Common.ClusteredLighting;
import Common.LightData;

ConstantBuffer<ViewConstants> g_View : register(b0);
ByteAddressBuffer g_Data : register(t0);
StructuredBuffer<GPULight> g_Original : register(t1);
StructuredBuffer<float4> g_Positions : register(t2);
RWByteAddressBuffer g_Output : register(u0);

[shader("compute")]
[numthreads(1, 1, 1)]
void Lookup(uint3 id : SV_DispatchThreadID)
{
	float3 position = g_Positions[id.x].xyz;
	uint cell = ClusterLookup(g_View.ViewProjection, g_View.View, g_View.Clusters, position);
	uint offset = id.x * 80;
	uint selected[8] = {0, 0, 0, 0, 0, 0, 0, 0};
	ClusterLightIterator iterator = ClusterBeginLights(cell);
	uint index, previous = 0, count = 0, orderErrors = 0;
	while (ClusterNextLight(g_Data, g_View.Clusters.State.x, iterator, index))
	{
		if (count > 0 && index <= previous)
			++orderErrors;
		previous = index;
		++count;
		selected[index / 32] |= 1u << (index % 32);
	}
	g_Output.Store4(offset, uint4(cell, count, orderErrors, 0));
	for (uint word = 0; word < 8; ++word)
	{
		uint legacy = 0;
		for (uint bit = 0; bit < 32; ++bit)
		{
			uint index = word * 32 + bit;
			if (index >= g_View.Clusters.State.x)
				break;
			GPULight light = g_Original[index];
			float3 toLight = light.PositionRange.xyz - position;
			float distanceSquared = dot(toLight, toLight);
			if (!(distanceSquared > light.PositionRange.w * light.PositionRange.w))
				legacy |= 1u << bit;
		}
		g_Output.Store(offset + 16 + word * 4, selected[word]);
		g_Output.Store(offset + 48 + word * 4, legacy);
	}
}

[shader("compute")]
[numthreads(1, 1, 1)]
void CheckABI(uint3 id : SV_DispatchThreadID)
{
	GPULight raw = LoadRawLight(g_Data, id.x);
	GPULight structured = g_Original[id.x];
	uint offset = id.x * 128;
	g_Output.Store4(offset, asuint(raw.PositionRange));
	g_Output.Store4(offset + 16, asuint(raw.ColorIntensity));
	g_Output.Store4(offset + 32, asuint(raw.DirectionCos));
	g_Output.Store4(offset + 48, asuint(raw.Params));
	g_Output.Store4(offset + 64, asuint(structured.PositionRange));
	g_Output.Store4(offset + 80, asuint(structured.ColorIntensity));
	g_Output.Store4(offset + 96, asuint(structured.DirectionCos));
	g_Output.Store4(offset + 112, asuint(structured.Params));
}
