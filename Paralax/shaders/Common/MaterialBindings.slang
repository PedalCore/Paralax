// Ordinary per-material descriptors shared by raster and portable visibility resolve.
module MaterialBindings;
import Common.SceneData;

// Set 1: per material.
public
ConstantBuffer<MaterialConstants> g_Material : register(b0, space1);
public
Texture2D<float4> g_BaseColorTexture : register(t0, space1);
public
Texture2D<float4> g_MetallicRoughnessTexture : register(t1, space1);
public
Texture2D<float4> g_NormalTexture : register(t2, space1);
public
Texture2D<float4> g_OcclusionTexture : register(t3, space1);
public
Texture2D<float4> g_EmissiveTexture : register(t4, space1);
public
Texture2D<float4> g_HeightTexture : register(t5, space1);
