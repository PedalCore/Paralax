module ClusteredLightingBuild;
import Common.ClusteredLighting;

import Common.SceneData;
ConstantBuffer<ViewConstants> g_Build : register(b0);
RWByteAddressBuffer g_Data : register(u0);

[shader("compute")]
[numthreads(8, 1, 1)]
void BuildMasks(uint3 cell : SV_GroupID, uint word : SV_GroupIndex)
{
	uint mask = 0;
	for (uint bit = 0; bit < 32; ++bit)
	{
		uint index = word * 32 + bit;
		if (index < g_Build.Clusters.State.x && ClusterLightMayIntersect(g_Build.ViewProjection, g_Build.View, g_Build.Clusters, cell, asfloat(g_Data.Load4(index * 64))))
			mask |= 1u << bit;
	}
	uint index = (cell.z * g_Build.Clusters.Dimensions.w + cell.y) * g_Build.Clusters.Dimensions.z + cell.x;
	g_Data.Store(ClusterPrefixBytes + (index * 8 + word) * 4, mask);
}
