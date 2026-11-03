// Shared descriptor contract for material evaluation and surface lighting.
module SceneBindings;
import Common.SceneData;
import Common.PBR;
import Common.ProbeGI;

// Set 0: per view.
public
ConstantBuffer<ViewConstants> g_View : register(b0, space0);
public
StructuredBuffer<GPULight> g_Lights : register(t0, space0);
public
Texture2DArray<float> g_ShadowMap : register(t1, space0);
public
TextureCube<float4> g_SpecularEnvironment : register(t2, space0);
public
TextureCube<float4> g_DiffuseEnvironment : register(t3, space0);
public
Texture2D<float2> g_BRDFLut : register(t4, space0);
public
Texture2DArray<float> g_LocalShadowMap : register(t6, space0);
public
StructuredBuffer<LocalShadowSlot> g_LocalShadowSlots : register(t7, space0);
public
Texture2D<float> g_ScreenAO : register(t8, space0);
public
SamplerComparisonState g_LocalShadowSampler : register(s3, space0);
public
SamplerState g_LinearWrap : register(s0, space0);
public
SamplerState g_LinearClamp : register(s1, space0);
public
SamplerComparisonState g_ShadowSampler : register(s2, space0);

public
ConstantBuffer<ProbeGIConstants> g_Probe : register(b0, space2);
public
Texture2DArray<float4> g_ProbeIrradiance : register(t0, space2);
public
Texture2DArray<float4> g_ProbeDistance : register(t1, space2);

public [[vk::push_constant]] ConstantBuffer<DrawConstants> g_Draw;
