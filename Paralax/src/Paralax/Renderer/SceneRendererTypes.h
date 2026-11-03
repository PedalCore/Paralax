#pragma once

// C++ mirrors of the GPU data layouts in Paralax/shaders/Common/SceneData.slang. Keep both in sync.

#include <glm/glm.hpp>

#include <cstdint>

namespace Paralax::GPU {

	inline constexpr uint32_t MaxCascades = 4;
	inline constexpr uint32_t MaxLights = 256;

	struct ViewConstants
	{
		glm::mat4 ViewProjection;
		glm::mat4 View;
		glm::mat4 Projection;
		glm::mat4 InverseViewProjection;
		glm::vec4 CameraPosition;
		glm::vec4 SunDirection;
		glm::vec4 SunColor;
		glm::mat4 CascadeViewProjection[MaxCascades];
		glm::vec4 CascadeSplits;
		glm::vec4 CascadeTexelWorldSize;
		glm::vec4 ShadowParams;
		glm::vec4 EnvironmentParams;
		glm::vec2 ViewportSize;
		float Exposure;
		uint32_t LightCount;
		float Time;
		uint32_t DebugView;
		float CascadeBlend;
		float CascadeNear;
		glm::vec4 Diagnostics;   // x: include environment specular
		glm::vec4 AmbientFill;   // rgb color, w intensity
		glm::vec4 ScreenEffects; // x: screen AO enabled
	};
	static_assert(sizeof(ViewConstants) == 4 * 64 + 3 * 16 + MaxCascades * 64 + 4 * 16 + 80, "ViewConstants layout mismatch");

	struct Light
	{
		glm::vec4 PositionRange;
		glm::vec4 ColorIntensity;
		glm::vec4 DirectionCos;
		glm::vec4 Params;
	};
	static_assert(sizeof(Light) == 64);

	// Keeping the exact raster matrices also makes face orientation identical during sampling.
	struct LocalShadowSlot
	{
		glm::mat4 ViewProjection[6];
		glm::vec4 Atlas; // first layer, face count, inverse resolution, tan(half FOV)
		glm::vec4 Bias;  // near, far, filter radius in metres, normal offset in texels
		glm::vec4 PositionRange;
	};
	static_assert(sizeof(LocalShadowSlot) == 432);

	struct MaterialConstants
	{
		glm::vec4 BaseColorFactor;
		glm::vec4 EmissiveFactor;
		float MetallicFactor;
		float RoughnessFactor;
		float NormalScale;
		float OcclusionStrength;
		float AlphaCutoff;
		uint32_t AlphaMode;
		float HeightScale;
		uint32_t TextureFlags;
	};
	static_assert(sizeof(MaterialConstants) == 64);

	struct DrawConstants
	{
		glm::mat4 World;
		glm::vec4 NormalMatrix0;
		glm::vec4 NormalMatrix1;
		glm::vec4 NormalMatrix2;
		glm::uvec4 Geometry; // index offset, vertex offset, global triangle base, count | clockwise-front bit31
	};
	static_assert(sizeof(DrawConstants) == 128, "Must fit the 128-byte push constant minimum");

	struct IBLConstants
	{
		glm::vec4 SunDirection;
		glm::vec4 SkyParams;
		glm::vec4 SunColor;
		uint32_t MipLevel;
		uint32_t SourceMipCount;
		float SourceSize;
		float Padding;
	};
	static_assert(sizeof(IBLConstants) == 64);

	struct TonemapConstants
	{
		float Exposure;
		uint32_t Operator;
		float BloomIntensity;
		float Upscale;           // 0 native, 1 bilinear enlargement, 2 normalized area reduction.
		glm::uvec4 Dimensions{}; // Source xy, output zw; used by area reduction.
	};

}
