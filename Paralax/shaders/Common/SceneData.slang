// Data layouts shared between the C++ SceneRenderer (Paralax/Renderer/SceneRendererTypes.h) and shaders.
// Keep both in sync: sizes are verified with static_asserts on the C++ side.
module SceneData;

public
static const uint MaxCascades = 4;

public
struct ViewConstants
{
public
	float4x4 ViewProjection;
public
	float4x4 View;
public
	float4x4 Projection;
public
	float4x4 InverseViewProjection;
public
	float4 CameraPosition; // xyz; w unused
public
	float4 SunDirection; // xyz: direction light travels (normalized); w: 1 if a sun exists
public
	float4 SunColor; // rgb * intensity; w: 1 if the sun casts shadows
public
	float4x4 CascadeViewProjection[MaxCascades];
public
	float4 CascadeSplits; // view-space far distance of each cascade
public
	float4 CascadeTexelWorldSize; // world-space size of one shadow-map texel per cascade
public
	float4 ShadowParams; // x: 1/shadow map size, y: depth bias, z: normal offset (texels), w: softness (world units)
public
	float4 EnvironmentParams; // x: IBL intensity, y: prefiltered mip count, z: sky rotation (radians), w: show sky
public
	float2 ViewportSize;
public
	float Exposure;
public
	uint LightCount;
public
	float Time;
public
	uint DebugView;
public
	float CascadeBlend; // Fraction of each cascade blended into the next (last fades to unshadowed).
public
	float CascadeNear; // First cascade start in view space.
public
	float4 Diagnostics; // x: include environment specular
public
	float4 AmbientFill; // rgb color, w intensity
public
	float4 ScreenEffects; // x: screen AO enabled
};

// Point and spot lights. Type: 0 = point, 1 = spot.
public
struct GPULight
{
public
	float4 PositionRange; // xyz position, w range
public
	float4 ColorIntensity; // rgb color * intensity, w type
public
	float4 DirectionCos; // xyz direction the light points to, w cos(outer cone)
public
	float4 Params; // x cos(inner cone), y shadow slot + 1 (0 unshadowed), z requested, w near
};

public
struct LocalShadowSlot
{
public
	float4x4 ViewProjection[6];
public
	float4 Atlas; // first layer, face count, inverse resolution, tan(half FOV)
public
	float4 Bias; // near, far, filter radius in metres, normal offset in texels
public
	float4 PositionRange;
};

public
struct MaterialConstants
{
public
	float4 BaseColorFactor;
public
	float4 EmissiveFactor; // rgb * strength
public
	float MetallicFactor;
public
	float RoughnessFactor;
public
	float NormalScale;
public
	float OcclusionStrength;
public
	float AlphaCutoff;
public
	uint AlphaMode; // 0 opaque, 1 mask, 2 blend
public
	float HeightScale;
public
	uint TextureFlags; // bit 0 base color, 1 metal/rough, 2 normal, 3 occlusion, 4 emissive, 5 height
};

public
struct DrawConstants
{
public
	float4x4 World;
public
	float4 NormalMatrix0; // rows of the 3x3 inverse-transpose world matrix
public
	float4 NormalMatrix1;
public
	float4 NormalMatrix2;
public
	uint4 Geometry; // index offset, vertex offset, global triangle base, count | clockwise-front bit31
};
