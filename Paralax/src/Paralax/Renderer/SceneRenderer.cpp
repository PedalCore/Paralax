#include "pxpch.h"
#include "Paralax/Renderer/SceneRenderer.h"

#include "Paralax/Asset/AssetManager.h"
#include "Paralax/Asset/MaterialAsset.h"
#include "Paralax/Asset/MeshAsset.h"
#include "Paralax/Asset/TextureAsset.h"
#include "Paralax/Renderer/GraphicsDevice.h"
#include "Paralax/Renderer/ShaderLibrary.h"
#include "Paralax/Scene/Entity.h"
#include "Paralax/Scene/ComponentRegistry.h"
#include "Paralax/Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/component_wise.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <bit>

namespace Paralax {

	std::span<const FieldInfo> SceneRendererSettings::GetFields()
	{
		using Reflection::FieldOptions;
		using Reflection::MakeField;
		static const std::vector<FieldInfo> s_Fields = {
			MakeField<&SceneRendererSettings::Path>("RenderPath", FieldOptions().Enum({"Forward", "Deferred", "Visibility"}).Describe("Opaque shading path; transparency remains forward. See render.stats for effective path and fallback reason.")),
			MakeField<&SceneRendererSettings::InternalRenderScale>("InternalRenderScale", FieldOptions().Range(0.5f, 2.0f).Describe("Scene raster scale: below 1 upscales; 1.5 or 2 supersamples with linear HDR area reduction. UI/text stay native. Failed scale retries after scale, output size or text-depth requirement changes.")),
			MakeField<&SceneRendererSettings::AntiAliasing>("AntiAliasing", FieldOptions().Enum({"Off", "Spatial", "Temporal", "FXAA"}).Describe("Temporal uses conservative motion history; FXAA filters native encoded color without history or jitter.")),
			MakeField<&SceneRendererSettings::AAHistoryWeight>("AAHistoryWeight", FieldOptions().Range(0.0f, 0.95f)),
			MakeField<&SceneRendererSettings::ClusteredLightingEnabled>("ClusteredLightingEnabled", FieldOptions().Describe("Opt-in conservative local-light cell masks; preserves light order and falls back when unavailable.")),
			MakeField<&SceneRendererSettings::ProfilingEnabled>("ProfilingEnabled", FieldOptions().Describe("Nonblocking GPU pass timestamps and separate CPU Render duration; profiling can add GPU overhead.")),
			MakeField<&SceneRendererSettings::SSAOEnabled>("SSAOEnabled", FieldOptions().Describe("Screen-space occlusion of diffuse environment, probe lighting and fill.")),
			MakeField<&SceneRendererSettings::SSAORadius>("SSAORadius", FieldOptions().Range(0.01f, 10.0f)),
			MakeField<&SceneRendererSettings::SSAOBias>("SSAOBias", FieldOptions().Range(0.0f, 0.25f)),
			MakeField<&SceneRendererSettings::SSAOStrength>("SSAOStrength", FieldOptions().Range(0.0f, 4.0f)),
			MakeField<&SceneRendererSettings::SSAOQuality>("SSAOQuality", FieldOptions().Enum({"Low", "Medium", "High"})),
			MakeField<&SceneRendererSettings::AmbientFillColor>("AmbientFillColor", FieldOptions().Range(0.0f, 1.0f)),
			MakeField<&SceneRendererSettings::AmbientFillIntensity>("AmbientFillIntensity", FieldOptions().Range(0.0f, 8.0f).Describe("Explicit artistic diffuse fill, independent of GI and environment intensity.")),
			MakeField<&SceneRendererSettings::SSREnabled>("SSREnabled", FieldOptions().Describe("Opaque screen-space reflections; misses retain environment specular. Geometric depth, no displaced POM intersections.")),
			MakeField<&SceneRendererSettings::SSRTemporalEnabled>("SSRTemporalEnabled", FieldOptions().Describe("Independent reflection history; moving geometry and lighting reset accumulation.")),
			MakeField<&SceneRendererSettings::SSRHistoryWeight>("SSRHistoryWeight", FieldOptions().Range(0.0f, 0.95f)),
			MakeField<&SceneRendererSettings::SSRMaxDistance>("SSRMaxDistance", FieldOptions().Range(0.1f, 200.0f)),
			MakeField<&SceneRendererSettings::SSRThickness>("SSRThickness", FieldOptions().Range(0.001f, 1.0f)),
			MakeField<&SceneRendererSettings::SSRMaxRoughness>("SSRMaxRoughness", FieldOptions().Range(0.05f, 1.0f)),
			MakeField<&SceneRendererSettings::SSRMaxSteps>("SSRMaxSteps", FieldOptions().Range(16, 256)),
			MakeField<&SceneRendererSettings::BloomEnabled>("BloomEnabled", FieldOptions().Describe("Soft-threshold scene-linear HDR bloom, before tonemapping.")),
			MakeField<&SceneRendererSettings::BloomThreshold>("BloomThreshold", FieldOptions().Range(0.0f, 64.0f)),
			MakeField<&SceneRendererSettings::BloomSoftKnee>("BloomSoftKnee", FieldOptions().Range(0.0f, 1.0f)),
			MakeField<&SceneRendererSettings::BloomIntensity>("BloomIntensity", FieldOptions().Range(0.0f, 2.0f)),
			MakeField<&SceneRendererSettings::BloomLevels>("BloomLevels", FieldOptions().Range(1, 6).Describe("Normalized multiscale spread; stops at one texel.")),
			MakeField<&SceneRendererSettings::Exposure>("Exposure", FieldOptions().Range(0.05f, 8.0f).Describe("Exposure multiplier applied before tonemapping.")),
			MakeField<&SceneRendererSettings::Tonemapper>("Tonemapper", FieldOptions().Enum({"AgX", "ACES", "None"})),
			MakeField<&SceneRendererSettings::Shadows>("Shadows", FieldOptions().Describe("Directional and local-light shadows; GI tracing remains independent.")),
			MakeField<&SceneRendererSettings::ShadowResolution>("ShadowResolution", FieldOptions().Enum({"Low", "Medium", "High"}).Describe("Shadow map size per cascade: 1024, 2048 or 4096.")),
			MakeField<&SceneRendererSettings::ShadowDistance>("ShadowDistance", FieldOptions().Range(5.0f, 500.0f).Describe("Distance covered by the shadow cascades, in meters.")),
			MakeField<&SceneRendererSettings::ShadowSoftness>("ShadowSoftness", FieldOptions().Range(0.0f, 0.5f).Describe("Penumbra size in meters.")),
			MakeField<&SceneRendererSettings::ShadowCascadeBlend>("ShadowCascadeBlend", FieldOptions().Range(0.0f, 0.3f).Describe("Fraction of each shadow cascade blended into the next; zero disables blending.")),
			MakeField<&SceneRendererSettings::LocalShadowResolution>("LocalShadowResolution", FieldOptions().Enum({"Low", "Medium", "High"}).Describe("Local shadow face size: 512, 1024 or 2048; depth memory is capped at 256 MiB.")),
			MakeField<&SceneRendererSettings::MaxShadowedLocalLights>("MaxShadowedLocalLights", FieldOptions().Range(0, 16).Describe("Maximum local shadow casters per view, subject to layer and memory budgets.")),
			MakeField<&SceneRendererSettings::LocalShadowSoftness>("LocalShadowSoftness", FieldOptions().Range(0.0f, 0.5f).Describe("Local-light shadow filter radius in metres, capped at eight receiver texels.")),
			MakeField<&SceneRendererSettings::DefaultEnvironmentIntensity>("SkyIntensity", FieldOptions().Range(0.0f, 3.0f).Describe("Environment intensity when the scene has no SkyLight.")),
			MakeField<&SceneRendererSettings::GIEnabled>("GIEnabled", FieldOptions().Describe("Enable diffuse probe lighting in scenes with a GI volume.")),
			MakeField<&SceneRendererSettings::GIFreeze>("GIFreeze", FieldOptions().Describe("Freeze probe history between explicitly queued updates; reset still refills once.")),
			MakeField<&SceneRendererSettings::GIVisibility>("GIVisibility", FieldOptions().Describe("Apply distance-moment visibility weights. Disable only for diagnosis; leaking is expected.")),
			MakeField<&SceneRendererSettings::GIBounceFeedback>("GIBounceFeedback", FieldOptions().Describe("Feed previous probe lighting into reflected hit lighting. Reset history when comparing.")),
			MakeField<&SceneRendererSettings::EnvironmentSpecular>("EnvironmentSpecular", FieldOptions().Describe("Include specular image-based lighting in the composite.")),
			MakeField<&SceneRendererSettings::GIDebugProbe>("GIDebugProbe", FieldOptions().Range(-1, 1727)),
			MakeField<&SceneRendererSettings::GIDebugScale>("GIDebugScale", FieldOptions().Range(0.0001f, 1000.0f)),
			MakeField<&SceneRendererSettings::GIShowProbes>("GIShowProbes", FieldOptions().Describe("Display diagnostic probe markers.")),
			MakeField<&SceneRendererSettings::GIRaysPerProbe>("GIRaysPerProbe", FieldOptions().Range(32, 256).Describe("Rays traced per updated probe; higher counts reduce noise at greater tracing cost.")),
			MakeField<&SceneRendererSettings::GIProbesPerFrame>("GIProbesPerFrame", FieldOptions().Range(1, 1728).Describe("Round-robin probe update budget after the initial full-volume fill.")),
			MakeField<&SceneRendererSettings::GIHysteresis>("GIHysteresis", FieldOptions().Range(0.0f, 0.99f).Describe("Previous lighting retained on each probe update.")),
			MakeField<&SceneRendererSettings::GINormalBias>("GINormalBias", FieldOptions().Range(0.0f, 0.05f).Describe("Receiver normal offset in metres.")),
			MakeField<&SceneRendererSettings::GIViewBias>("GIViewBias", FieldOptions().Range(0.0f, 0.5f).Describe("Receiver view offset as a fraction of probe spacing, limited to half the available view-ray distance.")),
			MakeField<&SceneRendererSettings::DebugView>("DebugView", FieldOptions().Enum({"Lit", "Normals", "BaseColor", "Indirect", "GICoverage", "DirectDiffuse", "DirectSpecular", "EnvironmentDiffuse", "EnvironmentSpecular", "Emissive", "IndirectDiffuse", "GIWeightSum", "GIValidNeighbours", "GIProbeWeight", "GIMeanDistance", "GISecondMoment", "GIVariance", "LocalShadow", "AmbientOcclusion", "AmbientFill", "Reflections"})),
		};
		return s_Fields;
	}

	namespace {

		nvrhi::VertexBufferBinding MeshVertexBinding(nvrhi::IBuffer* buffer, uint32_t vertexOffset)
		{
			// Express the mesh-local origin in the binding: large indexed base vertices fail on some Vulkan backends.
			// Keep indexed baseVertex zero in every mesh pass; vertex pulling retains its explicit element offset.
			return nvrhi::VertexBufferBinding().setBuffer(buffer).setOffset(uint64_t(vertexOffset) * sizeof(MeshVertex));
		}

		struct SceneIdentity
		{
		};
		Ref<const void> SceneToken(Scene& scene)
		{
			auto* identity = scene.GetRegistry().ctx().find<Ref<const SceneIdentity>>();
			if (!identity)
				identity = &scene.GetRegistry().ctx().emplace<Ref<const SceneIdentity>>(CreateRef<SceneIdentity>());
			return *identity;
		}

		uint64_t TemporalLightingKey(Scene& scene, AssetManager& assets, const SceneRendererSettings& settings)
		{
			uint64_t key = 1469598103934665603ull;
			auto append = [&](const std::string& value) {
				for (unsigned char character : value)
					key = (key ^ character) * 1099511628211ull;
			};
			auto compatibility = Reflection::ToJson(SceneRendererSettings::GetFields(), &settings);
			compatibility.erase("ProfilingEnabled");
			append(compatibility.dump());
			auto components = [&]<typename Component>(const char* name) {
				const auto* info = ComponentRegistry::Find(name);
				for (auto handle : scene.GetAllEntitiesWith<Component>())
				{
					const auto& component = scene.GetRegistry().get<Component>(handle);
					append(ComponentRegistry::ToJson(*info, &component).dump());
					const auto world = scene.GetWorldTransform(Entity(handle, &scene));
					for (int column = 0; column < 4; ++column)
						for (int row = 0; row < 4; ++row)
							key = (key ^ std::bit_cast<uint32_t>(world[column][row])) * 1099511628211ull;
					if constexpr (std::is_same_v<Component, SkyLightComponent>)
						if (auto texture = component.Environment.IsValid() ? assets.GetAsset<TextureAsset>(component.Environment) : nullptr)
							key = (key ^ texture->Version) * 1099511628211ull;
				}
			};
			components.template operator()<DirectionalLightComponent>("DirectionalLight");
			components.template operator()<PointLightComponent>("PointLight");
			components.template operator()<SpotLightComponent>("SpotLight");
			components.template operator()<SkyLightComponent>("SkyLight");
			components.template operator()<GIVolumeComponent>("GIVolume");
			return key;
		}

		double ProfilingTimeSeconds()
		{
			return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		constexpr nvrhi::Format s_HDRFormat = nvrhi::Format::RGBA16_FLOAT;
		constexpr nvrhi::Format s_DepthFormat = nvrhi::Format::D32;
		// Push constants are not descriptors on Vulkan (shaders use [[vk::push_constant]]); NVRHI still treats the slot
		// as a constant-buffer register, so keep it clear of real constant buffers.
		constexpr uint32_t s_PushConstantSlot = 15;

		// Frustum planes (ax + by + cz + d >= 0 inside) from a D3D-style (z in [0, 1]) view-projection matrix.
		std::array<glm::vec4, 6> ExtractFrustumPlanes(const glm::mat4& m)
		{
			const glm::vec4 row0(m[0][0], m[1][0], m[2][0], m[3][0]);
			const glm::vec4 row1(m[0][1], m[1][1], m[2][1], m[3][1]);
			const glm::vec4 row2(m[0][2], m[1][2], m[2][2], m[3][2]);
			const glm::vec4 row3(m[0][3], m[1][3], m[2][3], m[3][3]);
			std::array<glm::vec4, 6> planes = {row3 + row0, row3 - row0, row3 + row1, row3 - row1, row2, row3 - row2};
			for (glm::vec4& plane : planes)
				plane /= glm::length(glm::vec3(plane));
			return planes;
		}

		// skipPlane: index of a plane to ignore (planes[4] is the near plane; shadow casters behind it still cast).
		bool IsVisible(const std::array<glm::vec4, 6>& planes, const AABB& box, int skipPlane = -1)
		{
			if (!box.IsValid())
				return true;
			const glm::vec3 center = box.GetCenter();
			const glm::vec3 extents = box.GetExtents();
			for (int i = 0; i < 6; i++)
			{
				if (i == skipPlane)
					continue;
				const glm::vec4& plane = planes[static_cast<size_t>(i)];
				const float radius = glm::dot(extents, glm::abs(glm::vec3(plane)));
				if (glm::dot(glm::vec3(plane), center) + plane.w < -radius)
					return false;
			}
			return true;
		}

		GPU::DrawConstants MakeDrawConstants(const glm::mat4& world)
		{
			GPU::DrawConstants draw{};
			draw.World = world;
			const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(world)));
			// w: handedness of the transform (-1 when mirrored), applied to the bitangent in the shader.
			const float handedness = glm::determinant(glm::mat3(world)) < 0.0f ? -1.0f : 1.0f;
			draw.NormalMatrix0 = glm::vec4(normalMatrix[0][0], normalMatrix[1][0], normalMatrix[2][0], handedness);
			draw.NormalMatrix1 = glm::vec4(normalMatrix[0][1], normalMatrix[1][1], normalMatrix[2][1], 0.0f);
			draw.NormalMatrix2 = glm::vec4(normalMatrix[0][2], normalMatrix[1][2], normalMatrix[2][2], 0.0f);
			return draw;
		}

		uint64_t HashCombine(uint64_t seed, uint64_t value)
		{
			return seed ^ (value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2));
		}

	}

	std::optional<CameraView> CameraView::FromEntity(Scene& scene, Entity entity, float aspectRatio)
	{
		const auto* camera = entity.TryGetComponent<CameraComponent>();
		if (!camera)
			return std::nullopt;

		const glm::mat4 world = scene.GetWorldTransform(entity);
		CameraView view;
		view.Identity = static_cast<uint64_t>(entity.GetUUID());
		// Remove scale so it does not distort the view.
		glm::vec3 translation, scale;
		glm::quat rotation;
		if (!Math::DecomposeTransform(world, translation, rotation, scale))
			return std::nullopt;
		view.View = glm::inverse(Math::ComposeTransform(translation, rotation, glm::vec3(1.0f)));
		view.Projection = camera->Camera.GetProjection(aspectRatio);
		view.Position = translation;
		view.Orthographic = camera->Camera.Projection == SceneCamera::ProjectionType::Orthographic;
		view.Near = view.Orthographic ? camera->Camera.OrthographicNear : camera->Camera.PerspectiveNear;
		view.Far = view.Orthographic ? camera->Camera.OrthographicFar : camera->Camera.PerspectiveFar;
		view.VerticalFOV = camera->Camera.PerspectiveFOV;
		return view;
	}

	CameraView CameraView::LookAt(const glm::vec3& position, const glm::vec3& target, float verticalFOV, float aspectRatio, float nearPlane, float farPlane)
	{
		CameraView view;
		const glm::vec3 forward = glm::normalize(target - position);
		const glm::vec3 up = std::abs(forward.y) > 0.999f ? glm::vec3(0.0f, 0.0f, -1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
		view.View = glm::lookAtRH(position, target, up);
		view.Projection = glm::perspectiveRH_ZO(verticalFOV, aspectRatio, farPlane, nearPlane);
		view.Position = position;
		view.Near = nearPlane;
		view.Far = farPlane;
		view.VerticalFOV = verticalFOV;
		return view;
	}

	SceneRenderer::SceneRenderer(GraphicsDevice& device, AssetManager& assets, LocalShadowResourceFactory* shadowFactory, DeferredResourceFactory* deferredFactory, PostEffectResourceFactory* effectFactory, uint64_t effectBudgetCapacity)
		: m_GraphicsDevice(device)
		, m_Assets(assets)
		, m_Device(device.GetDevice())
		, m_Resources(device.GetDevice())
		, m_Environment(device.GetDevice())
		, m_Text(device.GetDevice())
		, m_ProbeGI(device.GetDevice(), device.GetInfo().Limits)
		, m_LocalShadowMaps(device.GetDevice(), device.GetInfo().Limits, shadowFactory)
		, m_Profiler(device.GetDevice(), device.GetInfo().GraphicsTimestampValidBits, device.GetInfo().TimestampPeriodNanoseconds)
		, m_EffectBudget(effectBudgetCapacity)
		, m_AmbientOcclusion(device, m_EffectBudget, effectFactory)
		, m_Bloom(device, m_EffectBudget, effectFactory)
		, m_RenderScale(device, m_EffectBudget, effectFactory)
		, m_TemporalAA(device, m_EffectBudget, effectFactory)
		, m_SpatialAA(device, m_EffectBudget, effectFactory)
		, m_FXAA(device, m_EffectBudget, effectFactory)
		, m_Clusters(device, m_EffectBudget, effectFactory)
		, m_Reflections(device, m_EffectBudget, effectFactory)
		, m_ReflectionHistory(device, m_EffectBudget, effectFactory)
		, m_EffectFactory(effectFactory)
		, m_Deferred(device, deferredFactory)
		, m_Visibility(device, deferredFactory)
		, m_ShadowFactory(shadowFactory)
	{
	}

	SceneRenderer::~SceneRenderer()
	{
		m_GraphicsDevice.WaitForIdle();
		InvalidateViewBindings();
		m_TonemapBindings = nullptr;
		m_CommandList = nullptr;
		m_Clusters.Release();
		m_ReflectionHistory.Release();
		m_Reflections.Release();
		m_AmbientOcclusion.Release();
		m_Bloom.Release();
		m_Text.InvalidateDepthFramebuffers(m_RenderScale.GetDepth());
		m_RenderScale.Release();
		m_TemporalAA.Release();
		m_SpatialAA.Release();
		m_FXAA.Release();
		m_Visibility.Prepare(false, nullptr, nullptr, nullptr, nullptr, nullptr);
		m_Deferred.Prepare(false, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
		m_Text.InvalidateDepthFramebuffers(m_DepthTarget);
		m_HDRFramebuffer = nullptr;
		m_HDRTarget = nullptr;
		m_DepthTarget = nullptr;
		m_SupersampleTargets.reset();
		m_GraphicsDevice.CollectGarbage();
		m_EffectBudget.CollectRetired();
	}

	bool SceneRenderer::Initialize()
	{
		m_CommandList = m_Device->createCommandList();

		nvrhi::SamplerDesc linear;
		linear.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap).setMaxAnisotropy(8.0f);
		m_LinearWrap = m_Device->createSampler(linear);
		nvrhi::SamplerDesc clamp;
		clamp.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		m_LinearClamp = m_Device->createSampler(clamp);
		nvrhi::SamplerDesc point;
		point.setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		m_PointClamp = m_Device->createSampler(point);
		nvrhi::SamplerDesc shadow;
		shadow.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Border).setBorderColor(nvrhi::Color(1.0f));
		shadow.setReductionType(nvrhi::SamplerReductionType::Comparison);
		m_ShadowSampler = m_Device->createSampler(shadow);
		shadow.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		m_LocalShadowSampler = m_Device->createSampler(shadow);

		m_ViewConstants = m_Device->createBuffer(nvrhi::BufferDesc()
													 .setByteSize(sizeof(GPU::ViewConstants))
													 .setIsConstantBuffer(true)
													 .setIsVolatile(true)
													 .setMaxVersions(16)
													 .setDebugName("ViewConstants"));
		m_LightBuffer = m_Device->createBuffer(nvrhi::BufferDesc()
												   .setByteSize(sizeof(GPU::Light) * GPU::MaxLights)
												   .setStructStride(sizeof(GPU::Light))
												   .setCanHaveRawViews(true)
												   .setInitialState(nvrhi::ResourceStates::ShaderResource)
												   .setKeepInitialState(true)
												   .setDebugName("Lights"));

		m_LocalShadowBuffer = m_Device->createBuffer(nvrhi::BufferDesc()
														 .setByteSize(sizeof(GPU::LocalShadowSlot) * 16)
														 .setStructStride(sizeof(GPU::LocalShadowSlot))
														 .setInitialState(nvrhi::ResourceStates::ShaderResource)
														 .setKeepInitialState(true)
														 .setDebugName("LocalShadowSlots"));
		if (!m_CommandList || !m_ViewConstants || !m_LightBuffer || !m_LocalShadowBuffer || !m_LocalShadowSampler)
			return false;

		m_CommandList->open();
		m_Resources.Initialize(m_CommandList);
		const bool environmentReady = m_Environment.Initialize(m_CommandList);
		const bool textReady = m_Text.Initialize(m_CommandList);
		const bool probeReady = m_ProbeGI.Initialize(m_CommandList);
		const bool shadowsReady = m_LocalShadowMaps.Initialize(m_CommandList);
		m_CommandList->close();
		m_Device->executeCommandList(m_CommandList);
		if (!environmentReady || !textReady || !probeReady || !shadowsReady)
			return false;

		if (!CreatePipelines())
			return false;

		m_Initialized = true;
		return true;
	}

	bool SceneRenderer::CreatePipelines()
	{
		m_ForwardVS = ShaderLibrary::CreateShader(m_Device, "ForwardPBR", "MainVS", nvrhi::ShaderType::Vertex);
		m_ForwardPS = ShaderLibrary::CreateShader(m_Device, "ForwardPBR", "MainPS", nvrhi::ShaderType::Pixel);
		m_ShadowVS = ShaderLibrary::CreateShader(m_Device, "Shadow", "ShadowVS", nvrhi::ShaderType::Vertex);
		m_LocalShadowVS = ShaderLibrary::CreateShader(m_Device, "Shadow", "LocalShadowVS", nvrhi::ShaderType::Vertex);
		m_ShadowPS = ShaderLibrary::CreateShader(m_Device, "Shadow", "ShadowPS", nvrhi::ShaderType::Pixel);
		m_LocalShadowPS = ShaderLibrary::CreateShader(m_Device, "Shadow", "LocalShadowPS", nvrhi::ShaderType::Pixel);
		m_LocalShadowMaskPS = ShaderLibrary::CreateShader(m_Device, "Shadow", "LocalShadowMaskPS", nvrhi::ShaderType::Pixel);
		m_ShadowMaskPS = ShaderLibrary::CreateShader(m_Device, "Shadow", "ShadowMaskPS", nvrhi::ShaderType::Pixel);
		m_SkyVS = ShaderLibrary::CreateShader(m_Device, "Sky", "SkyVS", nvrhi::ShaderType::Vertex);
		m_SkyPS = ShaderLibrary::CreateShader(m_Device, "Sky", "SkyPS", nvrhi::ShaderType::Pixel);
		m_TonemapVS = ShaderLibrary::CreateShader(m_Device, "Tonemap", "TonemapVS", nvrhi::ShaderType::Vertex);
		m_TonemapPS = ShaderLibrary::CreateShader(m_Device, "Tonemap", "TonemapPS", nvrhi::ShaderType::Pixel);
		if (!m_ForwardVS || !m_ForwardPS || !m_ShadowVS || !m_LocalShadowVS || !m_ShadowPS || !m_ShadowMaskPS || !m_LocalShadowPS || !m_LocalShadowMaskPS || !m_SkyVS || !m_SkyPS || !m_TonemapVS || !m_TonemapPS)
			return false;

		const nvrhi::VertexAttributeDesc attributes[] = {
			nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(offsetof(MeshVertex, Position)).setElementStride(sizeof(MeshVertex)),
			nvrhi::VertexAttributeDesc().setName("NORMAL").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(offsetof(MeshVertex, Normal)).setElementStride(sizeof(MeshVertex)),
			nvrhi::VertexAttributeDesc().setName("TANGENT").setFormat(nvrhi::Format::RGBA32_FLOAT).setOffset(offsetof(MeshVertex, Tangent)).setElementStride(sizeof(MeshVertex)),
			nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(MeshVertex, UV)).setElementStride(sizeof(MeshVertex)),
		};
		m_InputLayout = m_Device->createInputLayout(attributes, 4, m_ForwardVS);
		// The shadow pass only reads positions and UVs.
		const nvrhi::VertexAttributeDesc shadowAttributes[] = {attributes[0], attributes[3]};
		m_ShadowInputLayout = m_Device->createInputLayout(shadowAttributes, 2, m_ShadowVS);

		nvrhi::BindingLayoutDesc view;
		view.visibility = nvrhi::ShaderType::All;
		view.registerSpace = 0;
		view.registerSpaceIsDescriptorSet = true;
		view.bindings = {
			nvrhi::BindingLayoutItem::PushConstants(s_PushConstantSlot, sizeof(GPU::DrawConstants)),
			nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
			nvrhi::BindingLayoutItem::RawBuffer_SRV(0),
			nvrhi::BindingLayoutItem::Texture_SRV(1),
			nvrhi::BindingLayoutItem::Texture_SRV(2),
			nvrhi::BindingLayoutItem::Texture_SRV(3),
			nvrhi::BindingLayoutItem::Texture_SRV(4),
			nvrhi::BindingLayoutItem::Texture_SRV(5),
			nvrhi::BindingLayoutItem::Texture_SRV(6),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(7),
			nvrhi::BindingLayoutItem::Texture_SRV(8),
			nvrhi::BindingLayoutItem::Sampler(0),
			nvrhi::BindingLayoutItem::Sampler(1),
			nvrhi::BindingLayoutItem::Sampler(2),
			nvrhi::BindingLayoutItem::Sampler(3),
		};
		m_ViewLayout = m_Device->createBindingLayout(view);

		nvrhi::BindingLayoutDesc material;
		material.visibility = nvrhi::ShaderType::All;
		material.registerSpace = 1;
		material.registerSpaceIsDescriptorSet = true;
		material.bindings = {nvrhi::BindingLayoutItem::ConstantBuffer(0)};
		for (uint32_t slot = 0; slot < 6; slot++)
			material.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(slot));
		m_MaterialLayout = m_Device->createBindingLayout(material);

		nvrhi::BindingLayoutDesc shadowView;
		shadowView.visibility = nvrhi::ShaderType::All;
		shadowView.registerSpace = 0;
		shadowView.registerSpaceIsDescriptorSet = true;
		shadowView.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, sizeof(glm::mat4)), nvrhi::BindingLayoutItem::Sampler(0)};
		m_ShadowViewLayout = m_Device->createBindingLayout(shadowView);

		nvrhi::BindingLayoutDesc tonemap;
		tonemap.visibility = nvrhi::ShaderType::Pixel;
		tonemap.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, sizeof(GPU::TonemapConstants)), nvrhi::BindingLayoutItem::Texture_SRV(0),
							nvrhi::BindingLayoutItem::Texture_SRV(1), nvrhi::BindingLayoutItem::Sampler(0), nvrhi::BindingLayoutItem::Sampler(1)};
		m_TonemapLayout = m_Device->createBindingLayout(tonemap);

		nvrhi::BindingSetDesc shadowViewSet;
		shadowViewSet.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(glm::mat4)), nvrhi::BindingSetItem::Sampler(0, m_LinearWrap)};
		m_ShadowViewBindings = m_Device->createBindingSet(shadowViewSet, m_ShadowViewLayout);

		return m_InputLayout && m_ViewLayout && m_MaterialLayout && m_ShadowViewLayout && m_TonemapLayout && m_ShadowViewBindings;
	}

	bool SceneRenderer::EnsureTargets(uint32_t width, uint32_t height, bool supersampled)
	{
		m_TargetFailure.clear();
		const uint32_t limit = m_GraphicsDevice.GetInfo().Limits.MaxTextureDimension2D;
		if (!width || !height || width > limit || height > limit || (supersampled && (width > 8192 || height > 8192)))
		{
			m_TargetFailure = "scene raster dimensions exceed capacity";
			return false;
		}
		const bool sizeChanged = width != m_Width || height != m_Height || supersampled != bool(m_SupersampleTargets);
		const bool shadowChanged = m_ShadowMapSize != m_Settings.GetShadowMapSize();
		const uint32_t localLights = m_Settings.Shadows ? static_cast<uint32_t>(std::clamp(m_Settings.MaxShadowedLocalLights, 0, 16)) : 0;
		const bool localChanged = m_LocalShadowMaps.NeedsUpdate(m_Settings.GetLocalShadowMapSize(), localLights);
		if (!sizeChanged && !shadowChanged && !localChanged && m_HDRTarget && m_ShadowMap && m_LocalShadowPipeline && m_LocalShadowMaskPipeline)
			return true;

		m_GraphicsDevice.WaitForIdle();
		m_StaticLightingDirty = true;
		if (localChanged)
			m_LocalShadowMaps.Ensure(m_Settings.GetLocalShadowMapSize(), localLights);

		if (sizeChanged || !m_HDRTarget)
		{
			nvrhi::TextureDesc hdr;
			hdr.width = width;
			hdr.height = height;
			hdr.format = s_HDRFormat;
			hdr.isRenderTarget = true;
			hdr.debugName = "HDRColor";
			hdr.initialState = nvrhi::ResourceStates::RenderTarget;
			hdr.keepInitialState = true;
			hdr.setClearValue(nvrhi::Color(0.0f));
			// Stage the whole group before publishing it. Supersampling backing counts toward
			// the shared peak-live effect budget, including overlap with the previous group.
			auto textures = supersampled ? CreateScope<PostEffectTextures>(m_GraphicsDevice, m_EffectBudget, m_EffectFactory) : nullptr;
			nvrhi::TextureHandle nextHDR = textures ? nvrhi::TextureHandle(textures->Create(hdr)) : m_Device->createTexture(hdr);

			nvrhi::TextureDesc depth;
			depth.width = width;
			depth.height = height;
			depth.format = s_DepthFormat;
			depth.isRenderTarget = true;
			depth.debugName = "SceneDepth";
			depth.initialState = nvrhi::ResourceStates::DepthWrite;
			depth.keepInitialState = true;
			depth.setClearValue(nvrhi::Color(0.0f)); // Reversed Z: far plane = 0.
			nvrhi::TextureHandle nextDepth;
			if (nextHDR)
				nextDepth = textures ? nvrhi::TextureHandle(textures->Create(depth)) : m_Device->createTexture(depth);
			nvrhi::FramebufferHandle nextFramebuffer;
			if (nextHDR && nextDepth)
			{
				const auto desc = nvrhi::FramebufferDesc().addColorAttachment(nextHDR).setDepthAttachment(nextDepth);
				nextFramebuffer = supersampled && m_EffectFactory ? m_EffectFactory->CreateFramebuffer(m_Device, desc) : m_Device->createFramebuffer(desc);
			}
			if (textures)
				m_SupersampleAllocationAttempts += textures->GetAllocationAttempts();
			if (!nextFramebuffer)
			{
				m_TargetFailure = textures && (!nextHDR || !nextDepth) ? textures->GetFailureReason() : "scene target allocation failed";
				return false;
			}
			InvalidateViewBindings();
			m_TonemapBindings = nullptr;
			m_Text.InvalidateDepthFramebuffers(m_DepthTarget);
			m_Text.InvalidateDepthFramebuffers(m_RenderScale.GetDepth());
			m_RenderScale.Release();
			m_Visibility.Prepare(false, nullptr, nullptr, nullptr, nullptr, nullptr);
			m_Deferred.Prepare(false, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
			m_HDRFramebuffer = nullptr;
			m_HDRTarget = nullptr;
			m_DepthTarget = nullptr;
			m_SupersampleTargets.reset();
			m_SupersampleTargets = std::move(textures);
			m_HDRTarget = std::move(nextHDR);
			m_DepthTarget = std::move(nextDepth);
			m_HDRFramebuffer = std::move(nextFramebuffer);
			m_Width = width;
			m_Height = height;
			m_TonemapBindings = nullptr;
		}

		if (shadowChanged || !m_ShadowMap)
		{
			m_ShadowMapSize = m_Settings.GetShadowMapSize();
			nvrhi::TextureDesc shadow;
			shadow.width = m_ShadowMapSize;
			shadow.height = m_ShadowMapSize;
			shadow.arraySize = GPU::MaxCascades;
			shadow.dimension = nvrhi::TextureDimension::Texture2DArray;
			shadow.format = s_DepthFormat;
			shadow.isRenderTarget = true;
			shadow.debugName = "ShadowCascades";
			shadow.initialState = nvrhi::ResourceStates::ShaderResource;
			shadow.keepInitialState = true;
			shadow.setClearValue(nvrhi::Color(1.0f));
			m_ShadowMap = m_Device->createTexture(shadow);

			m_ShadowFramebuffers.clear();
			for (uint32_t cascade = 0; cascade < GPU::MaxCascades; cascade++)
			{
				nvrhi::FramebufferDesc desc;
				desc.setDepthAttachment(nvrhi::FramebufferAttachment().setTexture(m_ShadowMap).setArraySlice(cascade));
				m_ShadowFramebuffers.push_back(m_Device->createFramebuffer(desc));
			}
		}

		// Pipelines depend on framebuffer formats only, which never change; create them once.
		if (!m_OpaquePipeline)
		{
			nvrhi::GraphicsPipelineDesc forward;
			forward.inputLayout = m_InputLayout;
			forward.VS = m_ForwardVS;
			forward.PS = m_ForwardPS;
			forward.bindingLayouts = {m_ViewLayout, m_MaterialLayout, m_ProbeGI.GetLayout()};
			forward.renderState.rasterState.setFrontCounterClockwise(true).setCullBack();
			forward.renderState.depthStencilState.enableDepthTest().enableDepthWrite().setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual);
			m_OpaquePipeline = m_Device->createGraphicsPipeline(forward, m_HDRFramebuffer->getFramebufferInfo());

			// Mirrored (negative-determinant) transforms reverse the winding.
			nvrhi::GraphicsPipelineDesc mirrored = forward;
			mirrored.renderState.rasterState.setFrontCounterClockwise(false);
			m_OpaqueMirroredPipeline = m_Device->createGraphicsPipeline(mirrored, m_HDRFramebuffer->getFramebufferInfo());

			nvrhi::GraphicsPipelineDesc doubleSided = forward;
			doubleSided.renderState.rasterState.setCullNone();
			m_OpaqueDoubleSidedPipeline = m_Device->createGraphicsPipeline(doubleSided, m_HDRFramebuffer->getFramebufferInfo());

			nvrhi::GraphicsPipelineDesc blend = forward;
			blend.renderState.rasterState.setCullNone();
			blend.renderState.depthStencilState.disableDepthWrite();
			blend.renderState.blendState.targets[0].enableBlend().setSrcBlend(nvrhi::BlendFactor::SrcAlpha).setDestBlend(nvrhi::BlendFactor::InvSrcAlpha);
			m_BlendPipeline = m_Device->createGraphicsPipeline(blend, m_HDRFramebuffer->getFramebufferInfo());

			nvrhi::GraphicsPipelineDesc sky;
			sky.VS = m_SkyVS;
			sky.PS = m_SkyPS;
			sky.bindingLayouts = {m_ViewLayout};
			sky.renderState.rasterState.setCullNone();
			sky.renderState.depthStencilState.enableDepthTest().disableDepthWrite().setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual);
			m_SkyPipeline = m_Device->createGraphicsPipeline(sky, m_HDRFramebuffer->getFramebufferInfo());
		}

		if (!m_ShadowPipeline)
		{
			nvrhi::GraphicsPipelineDesc shadow;
			shadow.inputLayout = m_ShadowInputLayout;
			shadow.VS = m_ShadowVS;
			shadow.PS = m_ShadowPS;
			shadow.bindingLayouts = {m_ShadowViewLayout, m_MaterialLayout};
			shadow.renderState.rasterState.setCullNone();
			// Fragment depth preserves crossing triangles; the vertex shader only extends near coverage.
			shadow.renderState.depthStencilState.enableDepthTest().enableDepthWrite().setDepthFunc(nvrhi::ComparisonFunc::LessOrEqual);
			m_ShadowPipeline = m_Device->createGraphicsPipeline(shadow, m_ShadowFramebuffers[0]->getFramebufferInfo());

			nvrhi::GraphicsPipelineDesc masked = shadow;
			masked.PS = m_ShadowMaskPS;
			m_ShadowMaskPipeline = m_Device->createGraphicsPipeline(masked, m_ShadowFramebuffers[0]->getFramebufferInfo());
		}
		if (!m_LocalShadowPipeline || !m_LocalShadowMaskPipeline)
		{
			nvrhi::GraphicsPipelineDesc shadow;
			shadow.inputLayout = m_ShadowInputLayout;
			shadow.VS = m_LocalShadowVS;
			shadow.PS = m_LocalShadowPS;
			shadow.bindingLayouts = {m_ShadowViewLayout, m_MaterialLayout};
			shadow.renderState.rasterState.setCullNone();
			shadow.renderState.depthStencilState.enableDepthTest().enableDepthWrite().setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual);
			if (!m_LocalShadowPipeline)
				m_LocalShadowPipeline = m_ShadowFactory ? m_ShadowFactory->CreatePipeline(m_Device, shadow, m_LocalShadowMaps.GetFallbackFramebuffer()->getFramebufferInfo()) : m_Device->createGraphicsPipeline(shadow, m_LocalShadowMaps.GetFallbackFramebuffer()->getFramebufferInfo());
			shadow.PS = m_LocalShadowMaskPS;
			if (!m_LocalShadowMaskPipeline)
				m_LocalShadowMaskPipeline = m_ShadowFactory ? m_ShadowFactory->CreatePipeline(m_Device, shadow, m_LocalShadowMaps.GetFallbackFramebuffer()->getFramebufferInfo()) : m_Device->createGraphicsPipeline(shadow, m_LocalShadowMaps.GetFallbackFramebuffer()->getFramebufferInfo());
			const bool failed = !m_LocalShadowPipeline || !m_LocalShadowMaskPipeline;
			if (failed && !m_ReportedLocalPipelineFailure)
				PX_CORE_ERROR("Local shadow pipeline creation failed; retrying on the next frame");
			m_ReportedLocalPipelineFailure = failed;
		}
		return true;
	}

	nvrhi::IGraphicsPipeline* SceneRenderer::GetTonemapPipeline(nvrhi::IFramebuffer* output)
	{
		const nvrhi::FramebufferInfo& info = output->getFramebufferInfo();
		uint64_t key = HashCombine(static_cast<uint64_t>(info.depthFormat), (static_cast<uint64_t>(info.sampleCount) << 8) | info.sampleQuality);
		for (nvrhi::Format format : info.colorFormats)
			key = HashCombine(key, static_cast<uint64_t>(format));
		auto it = m_TonemapPipelines.find(key);
		if (it != m_TonemapPipelines.end())
			return it->second;

		nvrhi::GraphicsPipelineDesc desc;
		desc.VS = m_TonemapVS;
		desc.PS = m_TonemapPS;
		desc.bindingLayouts = {m_TonemapLayout};
		desc.renderState.rasterState.setCullNone();
		desc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
		nvrhi::GraphicsPipelineHandle pipeline = m_Device->createGraphicsPipeline(desc, info);
		m_TonemapPipelines[key] = pipeline;
		return pipeline;
	}

	void SceneRenderer::RenderProbeMarkers(const CameraView& camera, nvrhi::IFramebuffer* output)
	{
		if (!m_Settings.GIShowProbes || !GetGIState().Available || !m_ProbeGI.HasHistory())
			return;
		struct MarkerConstants
		{
			glm::mat4 ViewProjection;
			glm::vec4 Origin, Spacing;
			glm::uvec4 Grid;
			glm::vec4 Viewport;
		};
		static_assert(sizeof(MarkerConstants) == 128);
		if (!m_ProbeMarkerLayout)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::All;
			layout.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, sizeof(MarkerConstants)), nvrhi::BindingLayoutItem::Texture_SRV(0)};
			m_ProbeMarkerLayout = m_Device->createBindingLayout(layout);
		}
		if (!m_ProbeMarkerVS)
			m_ProbeMarkerVS = ShaderLibrary::CreateShader(m_Device, "ProbeMarkers", "MarkerVS", nvrhi::ShaderType::Vertex);
		if (!m_ProbeMarkerPS)
			m_ProbeMarkerPS = ShaderLibrary::CreateShader(m_Device, "ProbeMarkers", "MarkerPS", nvrhi::ShaderType::Pixel);
		if (!m_ProbeMarkerLayout || !m_ProbeMarkerVS || !m_ProbeMarkerPS)
			return;
		const auto& info = output->getFramebufferInfo();
		uint64_t key = HashCombine(static_cast<uint64_t>(info.depthFormat), (static_cast<uint64_t>(info.sampleCount) << 8) | info.sampleQuality);
		for (auto format : info.colorFormats)
			key = HashCombine(key, static_cast<uint64_t>(format));
		auto& pipeline = m_ProbeMarkerPipelines[key];
		if (!pipeline)
		{
			nvrhi::GraphicsPipelineDesc desc;
			desc.VS = m_ProbeMarkerVS;
			desc.PS = m_ProbeMarkerPS;
			desc.bindingLayouts = {m_ProbeMarkerLayout};
			desc.renderState.rasterState.setCullNone();
			desc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
			pipeline = m_Device->createGraphicsPipeline(desc, info);
		}
		nvrhi::IBindingSet* binding = nullptr;
		for (const auto& candidate : m_ProbeMarkerBindings)
			if (candidate && candidate->getDesc()->bindings[1].resourceHandle == m_ProbeGI.GetDistance())
				binding = candidate;
		if (!binding)
		{
			nvrhi::BindingSetDesc set;
			set.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(MarkerConstants)), nvrhi::BindingSetItem::Texture_SRV(0, m_ProbeGI.GetDistance())};
			auto& slot = m_ProbeMarkerBindings[m_NextProbeMarkerBinding++ % 2];
			slot = m_Device->createBindingSet(set, m_ProbeMarkerLayout);
			binding = slot;
		}
		if (!pipeline || !binding)
			return;
		const auto& probes = m_ProbeGI.GetConstants();
		MarkerConstants constants{camera.Projection * camera.View, probes.Origin, probes.Spacing, probes.Grid, {static_cast<float>(info.width), static_cast<float>(info.height), probes.Diagnostics.x, 0.0f}};
		nvrhi::GraphicsState state;
		state.pipeline = pipeline;
		state.framebuffer = output;
		state.bindings = {binding};
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(info.width), static_cast<float>(info.height)));
		m_CommandList->setGraphicsState(state);
		m_CommandList->setPushConstants(&constants, sizeof(constants));
		m_CommandList->draw(nvrhi::DrawArguments().setVertexCount(6).setInstanceCount(probes.Grid.w));
	}

	nvrhi::ITexture* SceneRenderer::ResolveTexture(AssetHandle handle, nvrhi::ITexture* fallback)
	{
		if (!handle.IsValid())
			return fallback;
		Ref<TextureAsset> texture = m_Assets.GetAsset<TextureAsset>(handle);
		if (!texture)
			return fallback;
		nvrhi::ITexture* gpu = m_Resources.GetTexture(*texture, m_CommandList);
		return gpu ? gpu : fallback;
	}

	SceneRenderer::MaterialEntry* SceneRenderer::GetMaterial(AssetHandle handle)
	{
		// Each material is validated (asset and texture versions) at most once per frame.
		const AssetHandle key = handle.IsValid() ? handle : AssetHandle(AssetManager::BuiltinDefaultMaterial);
		if (auto it = m_Materials.find(key); it != m_Materials.end() && it->second.LastValidatedFrame == m_FrameIndex && it->second.BindingSet)
			return &it->second;

		Ref<MaterialAsset> material = m_Assets.GetAsset<MaterialAsset>(handle.IsValid() ? handle : AssetHandle(AssetManager::BuiltinDefaultMaterial));
		if (!material)
			material = m_Assets.GetAsset<MaterialAsset>(AssetHandle(AssetManager::BuiltinDefaultMaterial));

		const AssetHandle textures[6] = {material->BaseColorTexture, material->MetallicRoughnessTexture, material->NormalTexture,
										 material->OcclusionTexture, material->EmissiveTexture, material->HeightTexture};
		uint64_t textureHash = 0;
		for (AssetHandle texture : textures)
		{
			const Ref<Asset> asset = texture.IsValid() && m_Assets.IsLoaded(texture) ? m_Assets.GetAsset(texture) : nullptr;
			textureHash = HashCombine(textureHash, HashCombine(static_cast<uint64_t>(texture), asset ? asset->Version : 0));
		}

		MaterialEntry& entry = m_Materials[material->Handle];
		entry.LastValidatedFrame = m_FrameIndex;
		if (entry.BindingSet && entry.Version == material->Version && entry.TextureVersionHash == textureHash)
			return &entry;

		nvrhi::ITexture* white = m_Resources.GetWhiteTexture();
		nvrhi::ITexture* bound[6] = {
			ResolveTexture(textures[0], white),
			ResolveTexture(textures[1], white),
			ResolveTexture(textures[2], m_Resources.GetFlatNormalTexture()),
			ResolveTexture(textures[3], white),
			ResolveTexture(textures[4], white),
			ResolveTexture(textures[5], white),
		};

		GPU::MaterialConstants constants{};
		constants.BaseColorFactor = material->BaseColorFactor;
		constants.EmissiveFactor = glm::vec4(material->EmissiveFactor * material->EmissiveStrength, 0.0f);
		constants.MetallicFactor = material->MetallicFactor;
		constants.RoughnessFactor = material->RoughnessFactor;
		constants.NormalScale = material->NormalScale;
		constants.OcclusionStrength = material->OcclusionStrength;
		constants.AlphaCutoff = material->AlphaCutoff;
		constants.AlphaMode = static_cast<uint32_t>(material->Alpha);
		constants.HeightScale = material->HeightScale;
		for (uint32_t i = 0; i < 6; i++)
		{
			const bool real = textures[i].IsValid() && bound[i] != white && bound[i] != m_Resources.GetFlatNormalTexture();
			if (real)
				constants.TextureFlags |= 1u << i;
		}

		if (!entry.Constants)
		{
			entry.Constants = m_Device->createBuffer(nvrhi::BufferDesc()
														 .setByteSize(sizeof(GPU::MaterialConstants))
														 .setIsConstantBuffer(true)
														 .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
														 .setKeepInitialState(true)
														 .setDebugName("MaterialConstants"));
		}
		m_CommandList->writeBuffer(entry.Constants, &constants, sizeof(constants));

		nvrhi::BindingSetDesc set;
		set.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, entry.Constants)};
		for (uint32_t slot = 0; slot < 6; slot++)
			set.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(slot, bound[slot]));
		entry.BindingSet = m_Device->createBindingSet(set, m_MaterialLayout);
		entry.Version = material->Version;
		entry.TextureVersionHash = textureHash;
		entry.Blend = material->Alpha == AlphaMode::Blend;
		entry.AlphaMask = material->Alpha == AlphaMode::Mask;
		entry.DoubleSided = material->DoubleSided;
		// Color metadata does not constrain authored values. Preserve out-of-range and HDR materials through
		// the Forward oracle instead of letting compact normalized/half attachments silently clip them.
		entry.CompactSurface = true;
		for (int channel = 0; channel < 3; ++channel)
			entry.CompactSurface &= std::isfinite(constants.BaseColorFactor[channel]) && constants.BaseColorFactor[channel] >= 0.0f && constants.BaseColorFactor[channel] <= 1.0f &&
									std::isfinite(constants.EmissiveFactor[channel]) && std::abs(constants.EmissiveFactor[channel]) <= 65504.0f;
		for (uint32_t slot : {0u, 3u, 4u})
			entry.CompactSurface &= bound[slot]->getDesc().format != nvrhi::Format::RGBA32_FLOAT;
		return &entry;
	}

	void SceneRenderer::CollectDraws(Scene& scene, const CameraView& camera)
	{
		m_Draws.clear();
		glm::mat4 cullingProjection = camera.Projection;
		if (m_Settings.AntiAliasing == AntiAliasingMode::Temporal && m_Settings.DebugView == RenderDebugView::None && m_Width && m_Height)
		{
			// One internal pixel covers the bounded half-pixel jitter without losing edge geometry.
			for (int column = 0; column < 4; ++column)
			{
				cullingProjection[column][0] /= 1 + 2.0f / static_cast<float>(m_Width);
				cullingProjection[column][1] /= 1 + 2.0f / static_cast<float>(m_Height);
			}
		}
		const glm::mat4 viewProjection = cullingProjection * camera.View;
		const auto planes = ExtractFrustumPlanes(viewProjection);

		auto view = scene.GetAllEntitiesWith<MeshComponent>();
		for (entt::entity handle : view)
		{
			Entity entity(handle, &scene);
			const MeshComponent& component = view.get<MeshComponent>(handle);
			Ref<MeshAsset> mesh = m_Assets.GetAsset<MeshAsset>(component.Mesh);
			if (!mesh)
				continue;
			const GPUMesh* gpuMesh = m_Resources.GetMesh(*mesh, m_CommandList);
			if (!gpuMesh)
				continue;

			const glm::mat4 world = scene.GetWorldTransform(entity);
			uint32_t ordinal = 0;
			for (const Submesh& submesh : mesh->Submeshes)
			{
				const glm::mat4 submeshWorld = world * submesh.Transform;
				const AABB worldBounds = submesh.Bounds.Transformed(world);

				AssetHandle materialHandle = 0;
				if (submesh.MaterialIndex < component.Materials.size() && component.Materials[submesh.MaterialIndex].IsValid())
					materialHandle = component.Materials[submesh.MaterialIndex];
				else if (submesh.MaterialIndex < mesh->Materials.size())
					materialHandle = mesh->Materials[submesh.MaterialIndex];
				MaterialEntry* material = GetMaterial(materialHandle);

				DrawItem item;
				item.EntityID = static_cast<uint64_t>(entity.GetUUID());
				item.MeshID = static_cast<uint64_t>(component.Mesh);
				item.TemporalVersion = HashCombine(HashCombine(mesh->Version, static_cast<uint64_t>(materialHandle)), HashCombine(material->Version, material->TextureVersionHash));
				item.SubmeshOrdinal = ordinal++;
				item.Mesh = gpuMesh;
				item.IndexOffset = submesh.IndexOffset;
				item.IndexCount = submesh.IndexCount;
				item.VertexOffset = submesh.VertexOffset;
				item.World = submeshWorld;
				item.Material = material->BindingSet;
				item.Blend = material->Blend;
				item.DoubleSided = material->DoubleSided;
				item.AlphaMask = material->AlphaMask;
				item.CompactSurface = material->CompactSurface;
				item.CastShadows = component.CastShadows && !material->Blend;
				item.ViewDepth = -(camera.View * glm::vec4(worldBounds.IsValid() ? worldBounds.GetCenter() : glm::vec3(submeshWorld[3]), 1.0f)).z;

				item.Bounds = worldBounds;
				item.Mirrored = glm::determinant(glm::mat3(submeshWorld)) < 0.0f;

				// Off-screen submeshes may still cast shadows; keep them (with their material) but don't shade them.
				item.Visible = IsVisible(planes, worldBounds);
				if (!item.Visible)
				{
					m_Stats.CulledSubmeshes++;
					if (!item.CastShadows)
						continue;
				}
				m_Draws.push_back(item);
				if (item.Visible)
					m_Stats.Triangles += item.IndexCount / 3;
			}
		}

		// Back-to-front for blended surfaces; front-to-back for opaque (early depth rejection).
		std::stable_sort(m_Draws.begin(), m_Draws.end(), [](const DrawItem& a, const DrawItem& b) {
			if (a.Blend != b.Blend)
				return !a.Blend;
			return a.Blend ? a.ViewDepth > b.ViewDepth : a.ViewDepth < b.ViewDepth;
		});
	}

	void SceneRenderer::UpdateEnvironment(Scene& scene, const glm::vec3& sunDirection, const glm::vec3& sunColor)
	{
		AssetHandle environment = 0;
		float intensity = m_Settings.DefaultEnvironmentIntensity;
		m_EnvironmentRotation = 0.0f;
		m_ShowSky = true;
		// The first sky light defines the environment.
		auto skyLights = scene.GetAllEntitiesWith<SkyLightComponent>();
		if (auto first = skyLights.begin(); first != skyLights.end())
		{
			const entt::entity handle = *first;
			const SkyLightComponent& sky = scene.GetRegistry().get<SkyLightComponent>(handle);
			environment = sky.Environment;
			intensity = sky.Intensity;
			m_EnvironmentRotation = glm::radians(sky.RotationDegrees);
			m_ShowSky = sky.ShowBackground;
		}
		m_EnvironmentIntensity = intensity;

		Ref<TextureAsset> equirect = environment.IsValid() ? m_Assets.GetAsset<TextureAsset>(environment) : nullptr;
		uint64_t key = 0;
		if (equirect)
		{
			key = HashCombine(static_cast<uint64_t>(environment), equirect->Version);
		}
		else
		{
			// Quantized sun parameters: the procedural sky is rebuilt only when they change noticeably.
			const glm::ivec3 direction = glm::ivec3(glm::round(sunDirection * 256.0f));
			const glm::ivec3 color = glm::ivec3(glm::round(sunColor * 64.0f));
			key = 0x5C1E5C1Eull;
			for (int component : {direction.x, direction.y, direction.z, color.x, color.y, color.z})
				key = HashCombine(key, static_cast<uint64_t>(static_cast<int64_t>(component)));
		}
		if (!m_StaticLightingDirty && key == m_EnvironmentKey && m_EnvironmentMaps.Specular)
			return;

		if (equirect)
		{
			nvrhi::ITexture* source = m_Resources.GetTexture(*equirect, m_CommandList);
			m_Environment.GenerateFromEquirect(m_CommandList, m_EnvironmentMaps, source, 1.0f);
		}
		else
		{
			// Without a sun the sky keeps its daylight gradient but shows no sun disk.
			const bool hasSun = glm::length(sunDirection) > 0.0f;
			const glm::vec3 direction = hasSun ? sunDirection : glm::normalize(glm::vec3(-0.3f, -1.0f, -0.4f));
			const glm::vec3 skyTint = hasSun ? sunColor / std::max(glm::compMax(sunColor), 1e-3f) : glm::vec3(1.0f);
			m_Environment.GenerateProceduralSky(m_CommandList, m_EnvironmentMaps, direction, skyTint, 1.0f, hasSun ? 40.0f : 0.0f);
		}
		m_EnvironmentKey = key;
		m_StaticLightingDirty = false;
	}

	void SceneRenderer::ComputeCascades(const CameraView& camera, const glm::vec3& sunDirection, GPU::ViewConstants& view)
	{
		const float nearPlane = std::max(camera.Near, 0.01f);
		const float farPlane = std::min(camera.Far, m_Settings.ShadowDistance);
		constexpr float lambda = 0.75f;

		// Frustum corners at the near and far planes in world space (reversed Z: near = 1, far = 0).
		const glm::mat4 inverseViewProjection = glm::inverse(camera.Projection * camera.View);
		std::array<glm::vec3, 4> nearCorners, farCorners;
		const glm::vec2 ndc[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
		for (int i = 0; i < 4; i++)
		{
			const glm::vec4 n = inverseViewProjection * glm::vec4(ndc[i], 1.0f, 1.0f);
			const glm::vec4 f = inverseViewProjection * glm::vec4(ndc[i], 0.0f, 1.0f);
			nearCorners[i] = glm::vec3(n) / n.w;
			farCorners[i] = glm::vec3(f) / f.w;
		}
		const float frustumDepth = std::max(camera.Far - camera.Near, 1e-3f);

		view.CascadeBlend = std::isfinite(m_Settings.ShadowCascadeBlend) ? std::clamp(m_Settings.ShadowCascadeBlend, 0.0f, 0.3f) : 0.0f;
		view.CascadeNear = nearPlane;
		float previousSplit = nearPlane;
		float previousStart = nearPlane;
		for (uint32_t cascade = 0; cascade < GPU::MaxCascades; cascade++)
		{
			const float p = static_cast<float>(cascade + 1) / static_cast<float>(GPU::MaxCascades);
			const float logarithmic = nearPlane * std::pow(farPlane / nearPlane, p);
			const float uniform = nearPlane + (farPlane - nearPlane) * p;
			const float split = lambda * logarithmic + (1.0f - lambda) * uniform;

			// Slice corners by interpolating along the frustum edges (linear in view depth for both projections).
			glm::vec3 corners[8];
			// The next cascade must cover the preceding transition band, including its off-screen casters.
			const float overlapStart = previousSplit - (previousSplit - previousStart) * view.CascadeBlend;
			const float t0 = (overlapStart - camera.Near) / frustumDepth;
			const float t1 = (split - camera.Near) / frustumDepth;
			for (int i = 0; i < 4; i++)
			{
				corners[i] = glm::mix(nearCorners[i], farCorners[i], t0);
				corners[i + 4] = glm::mix(nearCorners[i], farCorners[i], t1);
			}

			glm::vec3 center(0.0f);
			for (const glm::vec3& corner : corners)
				center += corner;
			center /= 8.0f;
			float radius = 0.0f;
			for (const glm::vec3& corner : corners)
				radius = std::max(radius, glm::length(corner - center));
			radius = std::ceil(radius * 16.0f) / 16.0f; // Stable size: avoids shimmering as the camera rotates.

			const glm::vec3 up = std::abs(sunDirection.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
			const float casterExtension = 100.0f;
			const glm::mat4 lightView = glm::lookAtRH(center - sunDirection * (radius + casterExtension), center, up);
			glm::mat4 lightProjection = glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + casterExtension);

			// Snap the projection to whole shadow-map texels so static geometry does not shimmer.
			const glm::mat4 shadowMatrix = lightProjection * lightView;
			glm::vec4 origin = shadowMatrix * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
			origin *= static_cast<float>(m_ShadowMapSize) / 2.0f;
			const glm::vec4 rounded = glm::round(origin);
			glm::vec4 offset = (rounded - origin) * 2.0f / static_cast<float>(m_ShadowMapSize);
			lightProjection[3][0] += offset.x;
			lightProjection[3][1] += offset.y;

			view.CascadeViewProjection[cascade] = lightProjection * lightView;
			view.CascadeTexelWorldSize[static_cast<int>(cascade)] = 2.0f * radius / static_cast<float>(m_ShadowMapSize);
			view.CascadeSplits[static_cast<int>(cascade)] = split;
			previousStart = previousSplit;
			previousSplit = split;
		}

		view.ShadowParams = glm::vec4(1.0f / static_cast<float>(m_ShadowMapSize), 0.0002f, 1.5f, m_Settings.ShadowSoftness);
	}

	void SceneRenderer::RenderShadows(const GPU::ViewConstants& view)
	{
		for (uint32_t cascade = 0; cascade < GPU::MaxCascades; cascade++)
		{
			nvrhi::IFramebuffer* framebuffer = m_ShadowFramebuffers[cascade];
			m_CommandList->clearDepthStencilTexture(m_ShadowMap, nvrhi::TextureSubresourceSet(0, 1, cascade, 1), true, 1.0f, false, 0);

			const auto planes = ExtractFrustumPlanes(view.CascadeViewProjection[cascade]);
			for (const DrawItem& draw : m_Draws)
			{
				// Near plane ignored: casters between the light and the cascade still cast (depth clamping).
				if (!draw.CastShadows || !IsVisible(planes, draw.Bounds, 4))
					continue;
				nvrhi::IBindingSet* material = draw.Material;

				nvrhi::GraphicsState state;
				state.pipeline = draw.AlphaMask ? m_ShadowMaskPipeline : m_ShadowPipeline;
				state.framebuffer = framebuffer;
				state.bindings = {m_ShadowViewBindings, material};
				state.vertexBuffers = {MeshVertexBinding(draw.Mesh->VertexBuffer, draw.VertexOffset)};
				state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(draw.Mesh->IndexBuffer).setFormat(nvrhi::Format::R32_UINT);
				state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(m_ShadowMapSize), static_cast<float>(m_ShadowMapSize)));
				m_CommandList->setGraphicsState(state);

				const glm::mat4 worldLightViewProjection = view.CascadeViewProjection[cascade] * draw.World;
				m_CommandList->setPushConstants(&worldLightViewProjection, sizeof(worldLightViewProjection));
				m_CommandList->drawIndexed(nvrhi::DrawArguments().setVertexCount(draw.IndexCount).setStartIndexLocation(draw.IndexOffset).setStartVertexLocation(0));
				m_Stats.ShadowDrawCalls++;
			}
		}
	}

	void SceneRenderer::CollectLights(Scene& scene, const CameraView& camera, LocalShadowHistory* history)
	{
		const auto identity = SceneToken(scene);
		m_Stats.ShadowedLocalLightIDs.clear();
		std::vector<GPU::Light> lights;
		std::vector<LocalShadowCandidate> candidates;
		const auto cameraPlanes = ExtractFrustumPlanes(camera.Projection * camera.View);
		auto finite = [](glm::vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
		auto collect = [&](auto entities, bool spot) {
			for (entt::entity handle : entities)
			{
				Entity entity(handle, &scene);
				const glm::mat4 world = scene.GetWorldTransform(entity);
				GPU::Light light{};
				bool casts = false;
				float outerAngle = 0.0f;
				if (spot)
				{
					const auto& component = entity.GetComponent<SpotLightComponent>();
					outerAngle = component.OuterConeAngle;
					const glm::vec3 direction = -glm::vec3(world[2]);
					const float directionLength = glm::length(direction);
					if (!finite(direction) || !std::isfinite(directionLength) || directionLength < 1e-8f ||
						!std::isfinite(outerAngle) || outerAngle <= 0 || outerAngle > glm::radians(89.0f) ||
						!std::isfinite(component.InnerConeAngle) || component.InnerConeAngle < 0 || component.InnerConeAngle > glm::radians(89.0f) ||
						!std::isfinite(component.Intensity) || component.Intensity < 0 || !finite(component.Color) || glm::any(glm::lessThan(component.Color, glm::vec3(0))))
						continue;
					light.PositionRange = {glm::vec3(world[3]), component.Range};
					light.ColorIntensity = {component.Color * component.Intensity, 1.0f};
					light.DirectionCos = {direction / directionLength, std::cos(outerAngle)};
					light.Params.x = std::cos(std::clamp(component.InnerConeAngle, 0.0f, outerAngle));
					casts = component.CastShadows;
				}
				else
				{
					const auto& component = entity.GetComponent<PointLightComponent>();
					if (!std::isfinite(component.Intensity) || component.Intensity < 0 || !finite(component.Color) || glm::any(glm::lessThan(component.Color, glm::vec3(0))))
						continue;
					light.PositionRange = {glm::vec3(world[3]), component.Range};
					light.ColorIntensity = {component.Color * component.Intensity, 0.0f};
					casts = component.CastShadows;
				}
				if (!finite(glm::vec3(light.ColorIntensity)) || glm::compMax(glm::vec3(light.ColorIntensity)) <= 0)
					continue;
				light.Params.z = casts ? 1.0f : 0.0f;
				light.Params.w = 0.05f;
				const glm::vec3 position(light.PositionRange);
				const float range = light.PositionRange.w;
				const bool visible = IsVisible(cameraPlanes, AABB{position - glm::vec3(range), position + glm::vec3(range)});
				candidates.push_back({uint64_t(entity.GetUUID()), position, range, spot && outerAngle <= glm::radians(75.0f) ? 1u : 6u, casts, visible, spot ? 1u : 0u});
				lights.push_back(light);
			}
		};
		collect(scene.GetAllEntitiesWith<PointLightComponent>(), false);
		collect(scene.GetAllEntitiesWith<SpotLightComponent>(), true);
		const uint32_t maxLights = m_Settings.Shadows ? static_cast<uint32_t>(std::clamp(m_Settings.MaxShadowedLocalLights, 0, 16)) : 0;
		const uint32_t capacity = m_LocalShadowPipeline && m_LocalShadowMaskPipeline ? m_LocalShadowMaps.GetLayers() : 0;
		const auto selection = SelectLocalShadows(candidates, camera.Position, maxLights, capacity, identity, history);
		m_LocalShadowSlots.clear();
		const float inverseResolution = 1.0f / static_cast<float>(m_Settings.GetLocalShadowMapSize());
		constexpr glm::vec3 directions[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
		for (const auto& assignment : selection.Shadows)
		{
			GPU::Light& light = lights[assignment.Candidate];
			light.Params.y = static_cast<float>(m_LocalShadowSlots.size() + 1);
			GPU::LocalShadowSlot slot{};
			slot.PositionRange = light.PositionRange;
			// A one-texel guard contains hardware PCF at cube seams; each world-space tap chooses its own face.
			// Spots need the entire eight-texel filter plus the hardware footprint around the true cone.
			const float tangent = assignment.Layers == 6 ? 1.0f + 2.0f * inverseResolution : std::sqrt(std::max(1.0f - light.DirectionCos.w * light.DirectionCos.w, 1e-12f)) / light.DirectionCos.w * (1.0f + 24.0f * inverseResolution);
			slot.Atlas = {static_cast<float>(assignment.FirstLayer), static_cast<float>(assignment.Layers), inverseResolution, tangent};
			slot.Bias = {light.Params.w, light.PositionRange.w, std::isfinite(m_Settings.LocalShadowSoftness) ? std::clamp(m_Settings.LocalShadowSoftness, 0.0f, 0.5f) : 0.0f, 1.0f};
			const glm::mat4 projection = glm::perspectiveRH_ZO(2.0f * std::atan(tangent), 1.0f, light.PositionRange.w, light.Params.w);
			const glm::vec3 position(light.PositionRange);
			for (uint32_t face = 0; face < assignment.Layers; ++face)
			{
				const glm::vec3 direction = assignment.Layers == 6 ? directions[face] : glm::vec3(light.DirectionCos);
				const glm::vec3 up = std::abs(direction.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
				slot.ViewProjection[face] = projection * glm::lookAtRH(position, position + direction, up);
			}
			m_LocalShadowSlots.push_back(slot);
			m_Stats.ShadowedLocalLightIDs.push_back(candidates[assignment.Candidate].ID);
		}
		m_Lights.clear();
		for (uint32_t index : selection.DirectLights)
			m_Lights.push_back(lights[index]);
		m_Stats.Lights = static_cast<uint32_t>(m_Lights.size());
		m_Stats.RequestedLocalShadows = selection.Requested;
		m_Stats.ShadowedLocalLights = static_cast<uint32_t>(selection.Shadows.size());
		m_Stats.LocalShadowLayers = selection.LayersUsed;
		m_Stats.LocalShadowCapacity = capacity;
		m_Stats.LocalShadowAllocationAttempts = m_LocalShadowMaps.GetAllocationAttempts();
		const bool capacityExceeded = maxLights > 0 && capacity > 0 && selection.Shadows.size() < std::min(maxLights, selection.Requested);
		if (capacityExceeded && !m_ReportedLocalCapacity)
			PX_CORE_WARN("Local shadow layer budget cannot fit all requested lights; see render.stats for the selected set");
		m_ReportedLocalCapacity = capacityExceeded;
	}

	void SceneRenderer::RenderLocalShadows()
	{
		for (const auto& slot : m_LocalShadowSlots)
			for (uint32_t face = 0; face < static_cast<uint32_t>(slot.Atlas.y); ++face)
			{
				const uint32_t layer = static_cast<uint32_t>(slot.Atlas.x) + face;
				m_CommandList->clearDepthStencilTexture(m_LocalShadowMaps.GetTexture(), nvrhi::TextureSubresourceSet(0, 1, layer, 1), true, 0.0f, false, 0);
				const auto planes = ExtractFrustumPlanes(slot.ViewProjection[face]);
				for (const DrawItem& draw : m_Draws)
				{
					// Reversed-Z near plane is index 5. Depth clamping retains casters closer than 5 cm;
					// the side planes still reject geometry behind the light.
					const glm::vec3 nearest = glm::clamp(glm::vec3(slot.PositionRange), draw.Bounds.Min, draw.Bounds.Max);
					const glm::vec3 delta = nearest - glm::vec3(slot.PositionRange);
					if (!draw.CastShadows || (draw.Bounds.IsValid() && glm::dot(delta, delta) > slot.PositionRange.w * slot.PositionRange.w) || !IsVisible(planes, draw.Bounds, 5))
						continue;
					nvrhi::GraphicsState state;
					state.pipeline = draw.AlphaMask ? m_LocalShadowMaskPipeline : m_LocalShadowPipeline;
					state.framebuffer = m_LocalShadowMaps.GetFramebuffer(layer);
					state.bindings = {m_ShadowViewBindings, draw.Material};
					state.vertexBuffers = {MeshVertexBinding(draw.Mesh->VertexBuffer, draw.VertexOffset)};
					state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(draw.Mesh->IndexBuffer).setFormat(nvrhi::Format::R32_UINT);
					const float size = static_cast<float>(m_LocalShadowMaps.GetResolution());
					state.viewport.addViewportAndScissorRect(nvrhi::Viewport(size, size));
					m_CommandList->setGraphicsState(state);
					const glm::mat4 matrix = slot.ViewProjection[face] * draw.World;
					m_CommandList->setPushConstants(&matrix, sizeof(matrix));
					m_CommandList->drawIndexed(nvrhi::DrawArguments().setVertexCount(draw.IndexCount).setStartIndexLocation(draw.IndexOffset).setStartVertexLocation(0));
					++m_Stats.ShadowDrawCalls;
					++m_Stats.LocalShadowDrawCalls;
				}
			}
	}

	bool SceneRenderer::PrepareVisibilityDraws()
	{
		std::vector<DrawItem*> ordered;
		for (DrawItem& draw : m_Draws)
			if (draw.Visible && !draw.Blend)
				ordered.push_back(&draw);
		std::sort(ordered.begin(), ordered.end(), [](const DrawItem* a, const DrawItem* b) {
			return a->EntityID != b->EntityID ? a->EntityID < b->EntityID : a->SubmeshOrdinal < b->SubmeshOrdinal;
		});
		uint64_t triangles = 0;
		for (DrawItem* draw : ordered)
		{
			if (draw->IndexCount % 3 != 0 || uint64_t(draw->IndexOffset) + draw->IndexCount > draw->Mesh->IndexBuffer->getDesc().byteSize / sizeof(uint32_t) ||
				!VisibilitySurfaces::FitsTriangleIDs(triangles, draw->IndexCount / 3))
			{
				m_Stats.PathFallbackReason = "triangle identifier or index capacity exceeded";
				m_Visibility.EndPreparation(false);
				return false;
			}
			draw->TriangleBase = static_cast<uint32_t>(triangles);
			triangles += draw->IndexCount / 3;
			if (!m_Visibility.PrepareMesh(*draw->Mesh))
			{
				m_Stats.PathFallbackReason = m_Visibility.GetFallbackReason();
				m_Visibility.EndPreparation(false);
				return false;
			}
		}
		m_Visibility.EndPreparation(true);
		m_Stats.PathFallbackReason.clear();
		return true;
	}

	void SceneRenderer::RenderVisibilityOpaque()
	{
		m_Visibility.Clear(m_CommandList);
		for (bool resolve : {false, true})
			for (const DrawItem& draw : m_Draws)
			{
				if (!draw.Visible || draw.Blend)
					continue;
				nvrhi::GraphicsState state;
				state.pipeline = resolve ? m_Visibility.GetResolvePipeline() : m_Visibility.GetPipeline(draw.DoubleSided, draw.Mirrored);
				state.framebuffer = resolve ? m_Deferred.GetFramebuffer() : m_Visibility.GetFramebuffer();
				state.bindings = {m_ActiveViewBindings, draw.Material, m_ProbeGI.GetBindings(), m_Visibility.GetBindings(*draw.Mesh, resolve)};
				state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(m_Width), float(m_Height)));
				m_CommandList->setGraphicsState(state);
				GPU::DrawConstants constants = MakeDrawConstants(draw.World);
				constants.Geometry = {draw.IndexOffset, draw.VertexOffset, draw.TriangleBase, draw.IndexCount / 3 | (draw.Mirrored && !draw.DoubleSided ? 0x80000000u : 0u)};
				m_CommandList->setPushConstants(&constants, sizeof(constants));
				m_CommandList->draw(nvrhi::DrawArguments().setVertexCount(resolve ? 3 : draw.IndexCount));
				++m_Stats.DrawCalls;
				m_Stats.VisibilityResolveDrawCalls += resolve ? 1 : 0;
			}
	}

	void SceneRenderer::RenderForward(const GPU::ViewConstants& view, ReflectionHistory* reflectionHistory)
	{
		m_CommandList->clearTextureFloat(m_HDRTarget, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
		m_CommandList->clearDepthStencilTexture(m_DepthTarget, nvrhi::AllSubresources, true, 0.0f, false, 0);

		nvrhi::Viewport viewport(static_cast<float>(m_Width), static_cast<float>(m_Height));
		auto drawItems = [&](bool blendPass, bool guidePass = false, bool reflectionGuide = false) {
			for (const DrawItem& draw : m_Draws)
			{
				if (!draw.Visible || draw.Blend != blendPass)
					continue;

				nvrhi::GraphicsState state;
				state.pipeline = blendPass          ? m_BlendPipeline
								 : draw.DoubleSided ? m_OpaqueDoubleSidedPipeline
								 : draw.Mirrored    ? m_OpaqueMirroredPipeline
													: m_OpaquePipeline;
				if (!blendPass && m_Stats.EffectivePath != RenderPath::Forward)
					state.pipeline = m_Deferred.GetMaterialPipeline(draw.DoubleSided, draw.Mirrored);
				state.framebuffer = !blendPass && m_Stats.EffectivePath != RenderPath::Forward ? m_Deferred.GetFramebuffer() : m_HDRFramebuffer.Get();
				if (!blendPass && m_Reflections.IsReady() && m_Stats.EffectivePath == RenderPath::Forward)
				{
					state.pipeline = m_Reflections.GetOpaquePipeline(draw.DoubleSided, draw.Mirrored);
					state.framebuffer = m_Reflections.GetOpaqueFramebuffer();
				}
				if (reflectionGuide)
				{
					state.pipeline = m_Reflections.GetGuidePipeline(draw.DoubleSided, draw.Mirrored);
					state.framebuffer = m_Reflections.GetGuideFramebuffer();
				}
				if (guidePass)
				{
					state.pipeline = m_AmbientOcclusion.GetGuidePipeline(draw.DoubleSided, draw.Mirrored);
					state.framebuffer = m_AmbientOcclusion.GetGuideFramebuffer();
				}
				state.bindings = {m_ActiveViewBindings, draw.Material, m_ProbeGI.GetBindings()};
				state.vertexBuffers = {MeshVertexBinding(draw.Mesh->VertexBuffer, draw.VertexOffset)};
				state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(draw.Mesh->IndexBuffer).setFormat(nvrhi::Format::R32_UINT);
				state.viewport.addViewportAndScissorRect(viewport);
				m_CommandList->setGraphicsState(state);

				const GPU::DrawConstants constants = MakeDrawConstants(draw.World);
				m_CommandList->setPushConstants(&constants, sizeof(constants));
				m_CommandList->drawIndexed(nvrhi::DrawArguments().setVertexCount(draw.IndexCount).setStartIndexLocation(draw.IndexOffset).setStartVertexLocation(0));
				m_Stats.DrawCalls++;
			}
		};

		auto renderAO = [&]() {
			if (!m_AmbientOcclusion.IsReady())
				return;
			auto finite = [](float value, float fallback, float minimum, float maximum) { return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback; };
			m_Profiler.BeginSection(m_CommandList, RenderTimingSection::AmbientOcclusion);
			m_AmbientOcclusion.Render(m_CommandList, view.Projection, std::abs(view.Projection[3][3]) > 0.5f,
									  finite(m_Settings.SSAORadius, 0.5f, 0.01f, 10.0f), finite(m_Settings.SSAOBias, 0.02f, 0.0f, 0.25f),
									  finite(m_Settings.SSAOStrength, 1.0f, 0.0f, 4.0f), 8u << std::clamp(static_cast<int>(m_Settings.SSAOQuality), 0, 2));
			m_Profiler.EndSection(m_CommandList, RenderTimingSection::AmbientOcclusion);
			m_Stats.SSAOPasses += 5;
			m_Stats.DrawCalls += 5;
		};
		if (m_Reflections.IsReady())
			m_Reflections.ClearGuides(m_CommandList);
		if (m_AmbientOcclusion.NeedsPrepass())
		{
			m_AmbientOcclusion.ClearGuide(m_CommandList);
			drawItems(false, true);
			++m_Stats.SSAOPasses;
			renderAO();
			// Replay original opaque depth ordering, including coplanar GreaterOrEqual draws.
			m_CommandList->clearDepthStencilTexture(m_DepthTarget, nvrhi::AllSubresources, true, 0.0f, false, 0);
		}
		if (m_Stats.EffectivePath != RenderPath::Forward)
			m_Deferred.ClearCoverage(m_CommandList);
		if (m_Stats.EffectivePath == RenderPath::Visibility)
			RenderVisibilityOpaque();
		else
			drawItems(false);
		if (m_Stats.EffectivePath != RenderPath::Forward)
		{
			renderAO();
			m_Deferred.Resolve(m_CommandList, m_ActiveViewBindings, m_ProbeGI.GetBindings(),
							   m_Reflections.IsReady() ? m_Reflections.GetOpaquePipeline(false, false) : nullptr,
							   m_Reflections.IsReady() ? m_Reflections.GetOpaqueFramebuffer() : nullptr);
			++m_Stats.DrawCalls;
			++m_Stats.SurfaceResolveDrawCalls;
		}

		// Raw lighting diagnostics describe surfaces; preserve the sky in the original material debug views.
		if (view.DebugView < static_cast<uint32_t>(RenderDebugView::DirectDiffuse))
		{
			nvrhi::GraphicsState state;
			state.pipeline = m_SkyPipeline;
			state.framebuffer = m_HDRFramebuffer;
			state.bindings = {m_ActiveViewBindings};
			state.viewport.addViewportAndScissorRect(viewport);
			m_CommandList->setGraphicsState(state);
			const GPU::DrawConstants unused{};
			m_CommandList->setPushConstants(&unused, sizeof(unused));
			m_CommandList->draw(nvrhi::DrawArguments().setVertexCount(3));
		}

		if (m_Reflections.IsReady())
		{
			m_Profiler.BeginSection(m_CommandList, RenderTimingSection::Reflections);
			if (m_Reflections.NeedsGuides())
				drawItems(false, false, true);
			const ReflectionSettings settings{m_Settings.SSRMaxDistance, m_Settings.SSRThickness, m_Settings.SSRMaxRoughness,
											  static_cast<uint32_t>(std::clamp(m_Settings.SSRMaxSteps, 16, 256))};
			m_Reflections.Render(m_CommandList, settings, m_Settings.DebugView == RenderDebugView::Reflections, reflectionHistory, view.ViewProjection, m_Settings.SSRHistoryWeight, &m_Profiler);
			m_Profiler.EndSection(m_CommandList, RenderTimingSection::Reflections);
			m_Stats.SSRPasses = m_Reflections.GetHierarchyLevels() + 2 + (reflectionHistory ? 1u : 0u);
			m_Stats.DrawCalls += m_Stats.SSRPasses;
		}
		if (m_Settings.DebugView == RenderDebugView::Reflections && !m_Reflections.IsReady())
			m_CommandList->clearTextureFloat(m_HDRTarget, nvrhi::AllSubresources, nvrhi::Color(0, 0, 0, 1));
		if (m_Settings.DebugView != RenderDebugView::Reflections)
			drawItems(true);
		(void)view;
	}

	void SceneRenderer::RenderTemporalGuides(TemporalAA& history)
	{
		for (bool reactive : {false, true})
			for (const auto& draw : m_Draws)
			{
				if (!draw.Visible || draw.Blend != reactive)
					continue;
				nvrhi::GraphicsState state;
				state.pipeline = reactive ? history.GetReactivePipeline() : history.GetGuidePipeline(draw.DoubleSided, draw.Mirrored);
				state.framebuffer = reactive ? history.GetReactiveFramebuffer() : history.GetGuideFramebuffer();
				state.bindings = {m_ActiveViewBindings, draw.Material, m_ProbeGI.GetBindings(), history.GetDrawBindings()};
				state.vertexBuffers = {MeshVertexBinding(draw.Mesh->VertexBuffer, draw.VertexOffset)};
				state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(draw.Mesh->IndexBuffer).setFormat(nvrhi::Format::R32_UINT);
				state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(m_Width), float(m_Height)));
				m_CommandList->setGraphicsState(state);
				auto constants = MakeDrawConstants(draw.World);
				constants.Geometry.x = draw.TemporalIndex;
				m_CommandList->setPushConstants(&constants, sizeof(constants));
				m_CommandList->drawIndexed(nvrhi::DrawArguments().setVertexCount(draw.IndexCount).setStartIndexLocation(draw.IndexOffset).setStartVertexLocation(0));
				++m_Stats.DrawCalls;
			}
	}

	GIState SceneRenderer::GetGIState() const
	{
		const auto& parameters = m_ProbeGI.GetConstants();
		GIState state;
		state.Enabled = m_Settings.GIEnabled;
		state.Supported = m_ProbeGI.IsSupported();
		state.Frozen = m_Settings.GIFreeze;
		state.HistoryValid = m_ProbeGI.HasHistory();
		state.Available = state.Enabled && state.Supported && parameters.Origin.w > 0.0f;
		state.Updates = m_ProbeGI.GetUpdateCount();
		state.Queued = m_QueuedGIUpdates;
		state.Origin = glm::vec3(parameters.Origin);
		state.Spacing = parameters.Spacing.x;
		state.Counts = glm::uvec3(parameters.Grid);
		state.Frame = m_FrameIndex;
		return state;
	}

	bool SceneRenderer::ResetGIHistory()
	{
		if (!GetGIState().Available)
			return false;
		m_QueuedGIUpdates = 0;
		m_ProbeGI.ResetHistory();
		++m_TemporalResetGeneration;
		return true;
	}

	bool SceneRenderer::QueueGIUpdates(uint32_t frames)
	{
		if (!GetGIState().Available || !m_Settings.GIFreeze || frames == 0 || frames > 1024 || m_QueuedGIUpdates > 1024 - frames)
			return false;
		m_QueuedGIUpdates += frames;
		return true;
	}

	Scope<TemporalAA> SceneRenderer::CreateTemporalHistory()
	{
		return CreateScope<TemporalAA>(m_GraphicsDevice, m_EffectBudget, m_EffectFactory);
	}

	Scope<ReflectionHistory> SceneRenderer::CreateReflectionHistory()
	{
		return CreateScope<ReflectionHistory>(m_GraphicsDevice, m_EffectBudget, m_EffectFactory);
	}

	void SceneRenderer::ResetTemporalHistories(const char* reason)
	{
		m_TemporalAA.GetState().Reset(reason);
		m_ReflectionHistory.GetState().Reset(reason);
	}

	void SceneRenderer::InvalidateViewBindings()
	{
		m_ActiveViewBindings = nullptr;
		m_Clusters.InvalidateView();
		m_ViewBindings = nullptr;
	}

	void SceneRenderer::Render(Scene& scene, const CameraView& camera, nvrhi::IFramebuffer* output, LocalShadowHistory* shadowHistory, TemporalAA* temporalHistory, ReflectionHistory* reflectionHistory)
	{
		PX_CORE_VERIFY(m_Initialized, "SceneRenderer::Render called before a successful Initialize");
		PX_CORE_VERIFY(output, "SceneRenderer::Render requires an output framebuffer");

		const nvrhi::FramebufferInfoEx& outputInfo = output->getFramebufferInfo();
		if (outputInfo.width == 0 || outputInfo.height == 0)
			return;

		TemporalAA& aa = temporalHistory ? *temporalHistory : m_TemporalAA;
		ReflectionHistory* reflections = reflectionHistory ? reflectionHistory : temporalHistory ? nullptr
																								 : &m_ReflectionHistory;
		if (reflections != &m_ReflectionHistory)
			m_ReflectionHistory.DetachInputs();
		auto& failedScale = aa.m_FailedScale;
		const double cpuStart = m_Settings.ProfilingEnabled ? ProfilingTimeSeconds() : 0;
		m_Stats = SceneRenderStats();
		m_FrameIndex++;
		m_Stats.RenderFrame = m_FrameIndex;
		const float requestedScale = std::isfinite(m_Settings.InternalRenderScale) ? std::clamp(m_Settings.InternalRenderScale, 0.5f, 2.0f) : 1.0f;
		const glm::uvec2 outputSize(outputInfo.width, outputInfo.height);
		const auto requestedSize = RenderScale::CalculateDimensions(outputInfo.width, outputInfo.height, requestedScale);
		bool worldText = false;
		for (auto entity : scene.GetAllEntitiesWith<TextComponent>())
		{
			const auto& text = scene.GetRegistry().get<TextComponent>(entity);
			worldText |= !text.ScreenSpace && !text.Text.empty();
		}
		if (failedScale && (failedScale->Output != outputSize || failedScale->Scale != requestedScale || failedScale->WorldText != worldText))
			failedScale.reset();
		const bool cachedFailure = bool(failedScale);
		glm::uvec2 internalSize = cachedFailure ? outputSize : requestedSize;
		m_ActiveViewBindings = nullptr;
		if (!m_Settings.ClusteredLightingEnabled || m_Width != internalSize.x || m_Height != internalSize.y ||
			(internalSize.x > outputSize.x || internalSize.y > outputSize.y) != bool(m_SupersampleTargets))
			m_Clusters.Release();
		const bool fxaaRequested = m_Settings.AntiAliasing == AntiAliasingMode::FXAA && m_Settings.DebugView == RenderDebugView::None;
		const bool temporalRequested = m_Settings.AntiAliasing == AntiAliasingMode::Temporal && m_Settings.DebugView == RenderDebugView::None;
		const bool reflectionRequested = m_Settings.SSREnabled &&
										 (m_Settings.DebugView == RenderDebugView::None || m_Settings.DebugView == RenderDebugView::Reflections);
		uint64_t reflectionPlan = HashCombine(HashCombine(requestedSize.x, requestedSize.y), HashCombine(outputSize.x, outputSize.y));
		reflectionPlan = HashCombine(reflectionPlan, HashCombine(static_cast<uint64_t>(m_Settings.Path), static_cast<uint64_t>(m_Settings.AntiAliasing)));
		reflectionPlan = HashCombine(reflectionPlan, HashCombine(worldText ? 1u : 0u, m_Settings.SSAOEnabled ? 1u : 0u));
		const bool reflectionTransition = reflectionPlan != m_ReflectionPlanKey;
		const bool newReflectionRequest = reflectionRequested && !m_ReflectionWasRequested;
		m_ReflectionWasRequested = reflectionRequested;
		const bool reflectionHistoryRequested = reflectionRequested && m_Settings.SSRTemporalEnabled && reflections;
		if (reflections && (!reflectionHistoryRequested || reflections->m_ResourcePlan != reflectionPlan || (reflections->IsReady() && !reflections->MatchesSize(internalSize))))
			reflections->Release();
		if (reflections)
			reflections->m_ResourcePlan = reflectionPlan;
		const bool changedAAOwner = m_ReflectionAAOwner != aa.m_OwnerToken;
		m_ReflectionAAOwner = aa.m_OwnerToken;
		if (!reflectionRequested || reflectionTransition || (temporalRequested && changedAAOwner))
		{
			// Old reflection scratch and failed-input references must not starve a new scale/AA request.
			if (reflections)
				reflections->DetachInputs();
			m_Reflections.Release();
			m_EffectBudget.CollectRetired();
		}
		m_ReflectionPlanKey = reflectionPlan;
		const bool bloomRequested = m_Settings.BloomEnabled && std::isfinite(m_Settings.BloomIntensity) && m_Settings.BloomIntensity > 0 && m_Settings.DebugView == RenderDebugView::None;
		const auto bloomLevels = static_cast<uint32_t>(std::clamp(m_Settings.BloomLevels, 1, 6));
		if (m_Bloom.IsReady() && (!bloomRequested || !m_Bloom.MatchesShape(internalSize.x, internalSize.y, bloomLevels) || (reflectionRequested && (newReflectionRequest || reflectionTransition || changedAAOwner))))
		{
			// Disabled or incompatible pyramids must not consume a new AA request's budget.
			m_TonemapBindings = nullptr;
			m_Bloom.Release();
			m_EffectBudget.CollectRetired();
		}
		if (aa.IsReady() && (!temporalRequested || !aa.MatchesSize(internalSize.x, internalSize.y)))
		{
			m_TonemapBindings = nullptr;
			m_Bloom.Release();
			aa.Release();
		}
		auto* expectedCompositeSource = temporalRequested && aa.IsReady() ? aa.GetCompletedOutput() : m_HDRTarget.Get();
		if (m_TonemapBindings && m_TonemapBindings->getDesc()->bindings[1].resourceHandle != expectedCompositeSource)
		{
			// Compositor descriptors may retain a destroyed capture owner's last history output.
			// Release those references before another view's budget decisions.
			m_TonemapBindings = nullptr;
			m_Bloom.Release();
			m_EffectBudget.CollectRetired();
		}

		if (m_Width != internalSize.x || m_Height != internalSize.y)
		{
			// Retire incompatible effects before the native-depth budget decision. Otherwise an
			// affordable final configuration can fail only because its old pyramid is still live.
			InvalidateViewBindings();
			m_TonemapBindings = nullptr;
			m_AmbientOcclusion.Release();
			m_Bloom.Release();
			m_EffectBudget.CollectRetired();
		}
		// Retire disabled AA and depth from the previous raster before reserving supersampling.
		// Otherwise an affordable transition can become a cached budget failure.
		if (m_SpatialAA.IsReady() && (m_Settings.AntiAliasing == AntiAliasingMode::Off || fxaaRequested || m_Settings.DebugView != RenderDebugView::None ||
									  m_SpatialAA.GetFramebuffer()->getFramebufferInfo().width != outputInfo.width ||
									  m_SpatialAA.GetFramebuffer()->getFramebufferInfo().height != outputInfo.height))
			m_SpatialAA.Release();
		if (!fxaaRequested || (m_FXAA.IsReady() && !m_FXAA.MatchesOutput(output)))
			m_FXAA.Release();
		if (m_RenderScale.IsReady() && (!worldText || internalSize == outputSize || m_Width != internalSize.x || m_Height != internalSize.y ||
										!m_RenderScale.MatchesInputs(m_DepthTarget, outputInfo.width, outputInfo.height)))
		{
			m_Text.InvalidateDepthFramebuffers(m_RenderScale.GetDepth());
			m_RenderScale.Release();
		}
		m_EffectBudget.CollectRetired();
		if (!EnsureTargets(internalSize.x, internalSize.y, internalSize.x > outputSize.x || internalSize.y > outputSize.y))
		{
			failedScale = TemporalAA::ScaleFailure{outputSize, requestedScale, worldText, m_TargetFailure};
			internalSize = outputSize;
			if (!EnsureTargets(outputInfo.width, outputInfo.height))
			{
				PX_CORE_ERROR("Native scene targets unavailable: {}", m_TargetFailure);
				return;
			}
		}
		// A native-depth fallback may retry target/shadow preparation. Stage history until
		// the final capacity is known so one render never advances its retention twice.
		std::optional<LocalShadowHistory> stagedShadowHistory;
		if (shadowHistory)
			stagedShadowHistory = *shadowHistory;
		CollectLights(scene, camera, stagedShadowHistory ? &*stagedShadowHistory : nullptr);
		if (m_Lights.empty())
			m_Clusters.Release();
		const bool nativeDepthNeeded = worldText && internalSize != outputSize;
		if (m_RenderScale.IsReady() && (!nativeDepthNeeded || !m_RenderScale.MatchesInputs(m_DepthTarget, outputInfo.width, outputInfo.height)))
			m_Text.InvalidateDepthFramebuffers(m_RenderScale.GetDepth());
		const auto nativeDepthBackingAttempts = m_RenderScale.GetBackingAttempts();
		const bool nativeDepthReady = m_RenderScale.Prepare(nativeDepthNeeded, m_DepthTarget, outputInfo.width, outputInfo.height);
		if (nativeDepthNeeded && !nativeDepthReady)
		{
			failedScale = TemporalAA::ScaleFailure{outputSize, requestedScale, worldText, m_RenderScale.GetFallbackReason()};
			// One fallback decision before effect preparation/jitter; cache it to avoid resize oscillation.
			if (!EnsureTargets(outputInfo.width, outputInfo.height))
				return;
		}
		const uint32_t finalShadowCapacity = m_LocalShadowPipeline && m_LocalShadowMaskPipeline ? m_LocalShadowMaps.GetLayers() : 0;
		if (finalShadowCapacity != m_Stats.LocalShadowCapacity)
			CollectLights(scene, camera, shadowHistory);
		else if (shadowHistory)
			*shadowHistory = std::move(*stagedShadowHistory);
		m_StaticLightingDirty |= nativeDepthBackingAttempts != m_RenderScale.GetBackingAttempts();
		m_Stats.RequestedRenderScale = requestedScale;
		m_Stats.EffectiveRenderScale = failedScale ? 1.0f : requestedScale;
		m_Stats.ScaleFallbackReason = failedScale ? failedScale->Reason : "";
		m_Stats.InternalWidth = m_Width;
		m_Stats.InternalHeight = m_Height;
		m_Stats.OutputWidth = outputInfo.width;
		m_Stats.OutputHeight = outputInfo.height;
		const uint64_t surfaceGeneration = m_Deferred.GetGeneration();
		const uint64_t visibilityGeneration = m_Visibility.GetGeneration();
		m_Stats.RequestedPath = m_Settings.Path;
		const bool requestedSurface = m_Settings.Path == RenderPath::Deferred || m_Settings.Path == RenderPath::Visibility;
		const bool deferred = m_Deferred.Prepare(requestedSurface, m_HDRTarget, m_DepthTarget, m_InputLayout, m_ViewLayout, m_MaterialLayout, m_ProbeGI.GetLayout());
		const bool visibility = m_Visibility.Prepare(deferred && m_Settings.Path == RenderPath::Visibility, m_DepthTarget, m_Deferred.GetFramebuffer(), m_ViewLayout, m_MaterialLayout, m_ProbeGI.GetLayout());
		m_Stats.EffectivePath = visibility ? RenderPath::Visibility : deferred && m_Settings.Path == RenderPath::Deferred ? RenderPath::Deferred
																														  : RenderPath::Forward;
		m_Stats.PathFallbackReason = !requestedSurface && m_Settings.Path != RenderPath::Forward ? "invalid render path" : !deferred && requestedSurface ? m_Deferred.GetFallbackReason()
																																						 : m_Visibility.GetFallbackReason();

		m_CommandList->open();
		RenderTimingContext timing;
		if (m_Settings.ProfilingEnabled)
		{
			timing.Frame = m_FrameIndex;
			timing.InternalWidth = m_Width;
			timing.InternalHeight = m_Height;
			timing.OutputWidth = outputInfo.width;
			timing.OutputHeight = outputInfo.height;
			timing.RequestedPath = static_cast<uint32_t>(m_Settings.Path);
			timing.Settings = Reflection::ToJson(SceneRendererSettings::GetFields(), &m_Settings).dump();
		}
		m_Profiler.BeginFrame(m_Settings.ProfilingEnabled, m_CommandList, timing, ProfilingTimeSeconds());
		// Mesh uploads and portable resolve descriptors must precede any static lighting generation too.
		CollectDraws(scene, camera);
		const bool incompatibleMaterial = requestedSurface && std::any_of(m_Draws.begin(), m_Draws.end(), [](const DrawItem& draw) { return draw.Visible && !draw.Blend && !draw.CompactSurface; });
		if (incompatibleMaterial)
		{
			m_Stats.EffectivePath = RenderPath::Forward;
			m_Stats.PathFallbackReason = "material exceeds compact surface range";
			if (!m_ReportedSurfaceRange)
				PX_CORE_WARN("Material exceeds compact surface range; using Forward to preserve authored values");
		}
		m_ReportedSurfaceRange = incompatibleMaterial;
		if (visibility && incompatibleMaterial)
			m_Visibility.EndPreparation(false);
		if (visibility && !incompatibleMaterial && !PrepareVisibilityDraws())
			m_Stats.EffectivePath = RenderPath::Forward;
		m_StaticLightingDirty |= surfaceGeneration != m_Deferred.GetGeneration() || visibilityGeneration != m_Visibility.GetGeneration();
		m_Stats.SurfaceOwnedBytes = m_Deferred.GetOwnedBytes();
		m_Stats.SurfaceAllocationAttempts = m_Deferred.GetAllocationAttempts();
		m_Stats.VisibilityOwnedBytes = m_Visibility.GetOwnedBytes();
		m_Stats.VisibilityGeometryBytes = m_Visibility.GetGeometryBytes();
		m_Stats.VisibilityAllocationAttempts = m_Visibility.GetAllocationAttempts();

		auto* previousAO = m_AmbientOcclusion.GetTexture();
		const uint64_t aoBackingAttempts = m_AmbientOcclusion.GetBackingAttempts();
		const bool aoRequested = m_Settings.SSAOEnabled && std::isfinite(m_Settings.SSAOStrength) && m_Settings.SSAOStrength > 0;
		// Bindings can retain retired AO textures. Clear before replacement; stable frames reuse them.
		if (m_AmbientOcclusion.IsReady() && (!aoRequested || !m_AmbientOcclusion.MatchesInputs(m_DepthTarget, m_Stats.EffectivePath == RenderPath::Forward ? nullptr : m_Deferred.GetReceiverGuide())))
			InvalidateViewBindings();
		m_EffectBudget.CollectRetired();
		m_Stats.SSAOActive = m_AmbientOcclusion.Prepare(aoRequested, m_DepthTarget,
														m_Stats.EffectivePath == RenderPath::Forward ? nullptr : m_Deferred.GetReceiverGuide(), m_Resources.GetWhiteTexture(),
														m_ViewConstants, m_InputLayout, m_ViewLayout, m_MaterialLayout, m_ProbeGI.GetLayout());
		if (previousAO != m_AmbientOcclusion.GetTexture())
			InvalidateViewBindings();
		// Unsupported preflight does not allocate backing and must not regenerate static lighting.
		m_StaticLightingDirty |= aoBackingAttempts != m_AmbientOcclusion.GetBackingAttempts();
		m_Stats.SSAOFallbackReason = m_AmbientOcclusion.GetFallbackReason();
		m_Stats.RequestedAA = m_Settings.AntiAliasing;
		if (aa.IsReady() && (!temporalRequested || !aa.MatchesSize(m_Width, m_Height)))
		{
			m_TonemapBindings = nullptr;
			m_Bloom.Release();
			aa.Release();
		}
		const auto temporalBacking = aa.GetBackingAttempts();
		std::vector<TemporalDrawSnapshot> temporalDraws;
		std::vector<TemporalGPURecord> temporalRecords;
		bool temporalActive = false, temporalOpaque = false, temporalBlend = false;
		if (temporalRequested)
		{
			const auto count = std::count_if(m_Draws.begin(), m_Draws.end(), [](const DrawItem& draw) { return draw.Visible; });
			if (count > TemporalState::MaxDraws)
			{
				if (aa.IsReady())
				{
					m_TonemapBindings = nullptr;
					m_Bloom.Release();
					aa.Release();
				}
				aa.m_MemoryFailure.reset();
				aa.GetState().Abort("temporal draw capacity exceeded");
				m_Stats.AAFallbackReason = "temporal draw capacity exceeded";
			}
			else
			{
				const glm::uvec2 size(m_Width, m_Height);
				const uint32_t drawCount = static_cast<uint32_t>(count);
				const bool cachedMemoryFailure = aa.m_MemoryFailure && aa.m_MemoryFailure->InternalSize == size &&
												 aa.m_MemoryFailure->OutputSize == outputSize && aa.m_MemoryFailure->DrawCount == drawCount &&
												 aa.m_MemoryFailure->OtherBytes == m_EffectBudget.GetUsedBytes() - m_SpatialAA.GetOwnedBytes();
				auto prepare = [&]() { return aa.Prepare(m_HDRTarget, m_DepthTarget, drawCount, m_InputLayout, m_ViewLayout, m_MaterialLayout, m_ProbeGI.GetLayout()); };
				bool prepared = !cachedMemoryFailure && prepare();
				if (!prepared && !cachedMemoryFailure && std::strcmp(aa.GetFallbackReason(), "renderer effect memory budget exhausted") == 0 && m_SpatialAA.IsReady())
				{
					// A transition may fit after retiring Spatial. Try once; an unchanged failed request
					// retains its working fallback instead of allocating it and stalling every frame.
					m_SpatialAA.Release();
					prepared = prepare();
				}
				if (!prepared)
				{
					m_Stats.AAFallbackReason = cachedMemoryFailure ? "renderer effect memory budget exhausted" : aa.GetFallbackReason();
					if (m_Stats.AAFallbackReason == "renderer effect memory budget exhausted")
					{
						// Spatial is replaceable during the bounded retry. Record the other live
						// allocations at the failure, never the budget after later effects retire.
						aa.m_MemoryFailure = TemporalAA::MemoryFailure{size, outputSize, drawCount, m_EffectBudget.GetUsedBytes() - m_SpatialAA.GetOwnedBytes()};
					}
					else
						aa.m_MemoryFailure.reset();
				}
				else
				{
					aa.m_MemoryFailure.reset();
					temporalDraws.reserve(static_cast<size_t>(count));
					for (auto& draw : m_Draws)
						if (draw.Visible)
						{
							temporalOpaque |= !draw.Blend;
							temporalBlend |= draw.Blend;
							draw.TemporalIndex = static_cast<uint32_t>(temporalDraws.size());
							temporalDraws.push_back({{draw.EntityID, draw.MeshID, draw.SubmeshOrdinal}, draw.World, draw.TemporalVersion, false});
						}
					TemporalFrameSnapshot snapshot;
					snapshot.Scene = SceneToken(scene);
					snapshot.CameraIdentity = camera.Identity;
					snapshot.View = camera.View;
					snapshot.Projection = camera.Projection;
					snapshot.InternalSize = {m_Width, m_Height};
					snapshot.OutputSize = outputSize;
					snapshot.Settings = HashCombine(TemporalLightingKey(scene, m_Assets, m_Settings), HashCombine(static_cast<uint64_t>(m_Stats.EffectivePath), m_TemporalResetGeneration));
					temporalActive = aa.GetState().Stage(snapshot, temporalDraws);
					if (temporalActive)
					{
						const auto& previous = aa.GetState().CanUseHistory() ? aa.GetState().GetPreviousFrame() : snapshot;
						for (const auto& draw : aa.GetState().GetDrawRecords())
							temporalRecords.push_back({previous.Projection * previous.View * draw.PreviousWorld, previous.View * draw.PreviousWorld, {draw.ID, draw.Reactive ? 1u : 0u, 0, 0}});
					}
					else
						m_Stats.AAFallbackReason = aa.GetState().GetResetReason();
				}
			}
		}
		else
		{
			aa.m_MemoryFailure.reset();
			aa.GetState().Reset(m_Settings.DebugView == RenderDebugView::None ? "AA disabled" : "diagnostic view");
		}
		m_StaticLightingDirty |= temporalBacking != aa.GetBackingAttempts();
		const auto spatialBacking = m_SpatialAA.GetBackingAttempts();
		const bool spatialRequested = !temporalActive && m_Settings.DebugView == RenderDebugView::None &&
									  (m_Settings.AntiAliasing == AntiAliasingMode::Spatial || temporalRequested);
		const bool spatialActive = m_SpatialAA.Prepare(spatialRequested, output);
		m_StaticLightingDirty |= spatialBacking != m_SpatialAA.GetBackingAttempts();
		if (spatialRequested && !spatialActive)
		{
			if (!m_Stats.AAFallbackReason.empty())
				m_Stats.AAFallbackReason += "; ";
			m_Stats.AAFallbackReason += m_SpatialAA.GetFallbackReason();
		}
		const auto fxaaBacking = m_FXAA.GetBackingAttempts();
		const bool fxaaActive = m_FXAA.Prepare(fxaaRequested, output);
		m_StaticLightingDirty |= fxaaBacking != m_FXAA.GetBackingAttempts();
		if (fxaaRequested && !fxaaActive)
			m_Stats.AAFallbackReason = m_FXAA.GetFallbackReason();
		m_Stats.EffectiveAA = temporalActive ? AntiAliasingMode::Temporal : spatialActive ? AntiAliasingMode::Spatial
																		: fxaaActive      ? AntiAliasingMode::FXAA
																						  : AntiAliasingMode::Off;
		aa.m_EffectiveMode = m_Stats.EffectiveAA;
		aa.m_RenderFallback = m_Stats.AAFallbackReason;
		m_Stats.AAResetReason = aa.GetState().GetResetReason();
		m_Stats.AASampleIndex = aa.GetState().GetSampleIndex();
		const auto rasterProjection = temporalActive ? TemporalState::JitterProjection(camera.Projection, {m_Width, m_Height}, aa.GetState().GetJitter()) : camera.Projection;
		nvrhi::ITexture* sceneColor = temporalActive ? aa.GetOutput() : m_HDRTarget.Get();

		const auto reflectionBacking = m_Reflections.GetBackingAttempts();
		m_Stats.SSRActive = m_Reflections.Prepare(reflectionRequested, m_HDRTarget, m_DepthTarget,
												  m_Stats.EffectivePath == RenderPath::Forward ? std::array<nvrhi::ITexture*, 3>{} : m_Deferred.GetMaterialGuides(),
												  m_ViewConstants, m_Environment.GetBRDFLut(), m_InputLayout, m_ViewLayout, m_MaterialLayout, m_ProbeGI.GetLayout(),
												  m_Stats.EffectivePath == RenderPath::Forward ? nullptr : m_Deferred.GetSurfaceLayout());
		m_StaticLightingDirty |= reflectionBacking != m_Reflections.GetBackingAttempts();
		m_Stats.SSRFallbackReason = m_Reflections.GetFallbackReason();
		m_Stats.SSROwnedBytes = m_Reflections.GetOwnedBytes();
		m_Stats.SSRHierarchyLevels = m_Reflections.GetHierarchyLevels();
		if (reflections && !m_Stats.SSRActive)
			reflections->Release();
		const auto reflectionHistoryBacking = reflections ? reflections->GetBackingAttempts() : 0;
		bool reflectionHistoryActive = reflectionHistoryRequested && m_Stats.SSRActive && reflections->Prepare(m_Reflections.GetHistoryInputs());
		if (reflections && !reflectionHistoryActive)
			reflections->GetState().Reset("reflection history unavailable or disabled");
		m_StaticLightingDirty |= reflections && reflectionHistoryBacking != reflections->GetBackingAttempts();
		if (reflections)
			m_Stats.SSROwnedBytes += reflections->GetOwnedBytes();

		auto* previousBloom = m_Bloom.GetTexture();
		const uint64_t bloomBackingAttempts = m_Bloom.GetBackingAttempts();
		if (m_Bloom.IsReady() && (!bloomRequested || !m_Bloom.MatchesInputs(sceneColor, bloomLevels)))
			m_TonemapBindings = nullptr;
		m_Stats.BloomActive = m_Bloom.Prepare(bloomRequested, sceneColor, m_Resources.GetWhiteTexture(), bloomLevels);
		if (previousBloom != m_Bloom.GetTexture())
			m_TonemapBindings = nullptr;
		m_StaticLightingDirty |= bloomBackingAttempts != m_Bloom.GetBackingAttempts();
		m_Stats.BloomFallbackReason = m_Bloom.GetFallbackReason();
		m_Stats.EffectOwnedBytes = m_EffectBudget.GetUsedBytes();
		m_Stats.EffectPeakBytes = m_EffectBudget.GetPeakBytes();
		m_Stats.EffectAllocationAttempts = m_AmbientOcclusion.GetAllocationAttempts() + m_Bloom.GetAllocationAttempts() + m_RenderScale.GetAllocationAttempts() + aa.GetAllocationAttempts() + m_SpatialAA.GetAllocationAttempts() + m_FXAA.GetAllocationAttempts() + m_SupersampleAllocationAttempts + m_Reflections.GetAllocationAttempts() + (reflections ? reflections->GetAllocationAttempts() : 0);

		m_Stats.ClusteredLightingRequested = m_Settings.ClusteredLightingEnabled;
		std::optional<ClusteredLightingGrid> clusterGrid;
		bool clusterPrepared = false;
		const auto clusterBacking = m_Clusters.GetBackingAttempts();
		if (m_Settings.ClusteredLightingEnabled && !m_Lights.empty())
		{
			clusterGrid = ClusteredLightingGrid::Create(m_Width, m_Height, static_cast<uint32_t>(m_Lights.size()), camera.Near, camera.Far,
														camera.View, rasterProjection, m_GraphicsDevice.GetInfo().Limits.MaxStorageBufferBytes);
			if (clusterGrid)
			{
				clusterPrepared = m_Clusters.Prepare(*clusterGrid, m_ViewConstants);
				m_Stats.ClusterGrid = {clusterGrid->GetConstants().Dimensions.z, clusterGrid->GetConstants().Dimensions.w, ClusteredLightingGrid::DepthSlices};
				m_Stats.ClusterCells = clusterGrid->GetCellCount();
				m_Stats.ClusterLogicalBytes = clusterGrid->GetBufferBytes();
				m_Stats.ClusteredLightingFallbackReason = m_Clusters.GetFallbackReason();
			}
			else
				m_Stats.ClusteredLightingFallbackReason = "unsupported clustered camera or grid";
		}
		else if (m_Settings.ClusteredLightingEnabled)
			m_Stats.ClusteredLightingFallbackReason = "no admitted local lights";
		m_StaticLightingDirty |= clusterBacking != m_Clusters.GetBackingAttempts();

		if (m_StaticLightingDirty)
		{
			// The BRDF LUT and environment maps are written rarely (the LUT once, the maps when their inputs change) and
			// read every frame. On the macOS CI device (an Apple paravirtual GPU through MoltenVK) they have
			// intermittently come back all zero in the
			// first frame after a large render-target allocation, with no GPU command touching them in between, and
			// the loss was never seen when they were generated after the allocation (renderer issue R1). Regenerating
			// them after every target allocation costs a few compute dispatches and removes the dependence on
			// allocation order. UpdateEnvironment regenerates the maps while this flag is set.
			m_Environment.GenerateBRDFLut(m_CommandList);
			m_Stats.StaticLightingGenerations++;
		}

		// Sun (first directional light) and point/spot lights.
		glm::vec3 sunDirection(0.0f);
		glm::vec3 sunColor(0.0f);
		bool hasSun = false;
		bool sunShadows = false;
		// The first directional light is the sun.
		auto directionalLights = scene.GetAllEntitiesWith<DirectionalLightComponent>();
		if (auto first = directionalLights.begin(); first != directionalLights.end())
		{
			const entt::entity handle = *first;
			Entity entity(handle, &scene);
			const auto& light = entity.GetComponent<DirectionalLightComponent>();
			sunDirection = glm::normalize(glm::vec3(scene.GetWorldTransform(entity) * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
			sunColor = light.Color * light.Intensity;
			hasSun = true;
			sunShadows = light.CastShadows && m_Settings.Shadows;
		}

		if (!m_Lights.empty())
			m_CommandList->writeBuffer(m_LightBuffer, m_Lights.data(), m_Lights.size() * sizeof(GPU::Light));
		if (!m_LocalShadowSlots.empty())
			m_CommandList->writeBuffer(m_LocalShadowBuffer, m_LocalShadowSlots.data(), m_LocalShadowSlots.size() * sizeof(GPU::LocalShadowSlot));

		UpdateEnvironment(scene, hasSun ? sunDirection : glm::vec3(0.0f), sunColor);

		const std::array<nvrhi::IResource*, 6> viewTextures{m_ShadowMap, m_EnvironmentMaps.Specular, m_EnvironmentMaps.Diffuse, m_Environment.GetBRDFLut(), m_EnvironmentMaps.Radiance, m_LocalShadowMaps.GetTexture()};
		bool bindingsChanged = !m_ViewBindings;
		if (m_ViewBindings)
			for (size_t i = 0; i < viewTextures.size(); ++i)
				bindingsChanged |= m_ViewBindings->getDesc()->bindings[3 + i].resourceHandle != viewTextures[i];
		if (bindingsChanged)
		{
			InvalidateViewBindings();
			nvrhi::BindingSetDesc set;
			set.bindings = {
				nvrhi::BindingSetItem::PushConstants(s_PushConstantSlot, sizeof(GPU::DrawConstants)),
				nvrhi::BindingSetItem::ConstantBuffer(0, m_ViewConstants),
				nvrhi::BindingSetItem::RawBuffer_SRV(0, m_LightBuffer),
				nvrhi::BindingSetItem::Texture_SRV(1, m_ShadowMap),
				nvrhi::BindingSetItem::Texture_SRV(2, m_EnvironmentMaps.Specular),
				nvrhi::BindingSetItem::Texture_SRV(3, m_EnvironmentMaps.Diffuse),
				nvrhi::BindingSetItem::Texture_SRV(4, m_Environment.GetBRDFLut()),
				nvrhi::BindingSetItem::Texture_SRV(5, m_EnvironmentMaps.Radiance),
				nvrhi::BindingSetItem::Texture_SRV(6, m_LocalShadowMaps.GetTexture()),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(7, m_LocalShadowBuffer),
				nvrhi::BindingSetItem::Texture_SRV(8, m_AmbientOcclusion.IsReady() ? m_AmbientOcclusion.GetTexture() : m_Resources.GetWhiteTexture()),
				nvrhi::BindingSetItem::Sampler(0, m_LinearWrap),
				nvrhi::BindingSetItem::Sampler(1, m_LinearClamp),
				nvrhi::BindingSetItem::Sampler(2, m_ShadowSampler),
				nvrhi::BindingSetItem::Sampler(3, m_LocalShadowSampler),
			};
			m_ViewBindings = m_Device->createBindingSet(set, m_ViewLayout);
		}

		GPU::ViewConstants view{};
		view.AmbientFill = glm::vec4(glm::clamp(m_Settings.AmbientFillColor, glm::vec3(0), glm::vec3(1)),
									 std::isfinite(m_Settings.AmbientFillIntensity) ? std::clamp(m_Settings.AmbientFillIntensity, 0.0f, 8.0f) : 0.0f);
		if (!std::isfinite(view.AmbientFill.x) || !std::isfinite(view.AmbientFill.y) || !std::isfinite(view.AmbientFill.z))
			view.AmbientFill = glm::vec4(0);
		view.ScreenEffects.x = m_Stats.SSAOActive ? 1.0f : 0.0f;
		view.View = camera.View;
		view.Projection = rasterProjection;
		view.ViewProjection = rasterProjection * camera.View;
		view.InverseViewProjection = glm::inverse(view.ViewProjection);
		view.CameraPosition = glm::vec4(camera.Position, 1.0f);
		view.SunDirection = glm::vec4(hasSun ? sunDirection : glm::vec3(0.0f, -1.0f, 0.0f), hasSun ? 1.0f : 0.0f);
		view.SunColor = glm::vec4(sunColor, sunShadows ? 1.0f : 0.0f);
		view.EnvironmentParams = glm::vec4(m_EnvironmentIntensity, static_cast<float>(m_EnvironmentMaps.SpecularMipCount), m_EnvironmentRotation, m_ShowSky ? 1.0f : 0.0f);
		view.ViewportSize = glm::vec2(static_cast<float>(m_Width), static_cast<float>(m_Height));
		view.Exposure = m_Settings.Exposure;
		view.LightCount = static_cast<uint32_t>(m_Lights.size());
		view.DebugView = m_Settings.DebugView == RenderDebugView::Reflections ? 0u : static_cast<uint32_t>(m_Settings.DebugView);
		view.Diagnostics = {m_Settings.EnvironmentSpecular ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
		if (sunShadows)
			ComputeCascades(camera, sunDirection, view);
		m_ActiveViewBindings = m_ViewBindings;
		if (clusterPrepared)
		{
			if (auto* clustered = m_Clusters.FinalizeView(m_ViewBindings, m_ViewLayout, view.Clusters))
			{
				m_ActiveViewBindings = clustered;
				m_Stats.ClusteredLightingActive = true;
			}
			m_Stats.ClusteredLightingFallbackReason = m_Clusters.GetFallbackReason();
		}
		m_CommandList->writeBuffer(m_ViewConstants, &view, sizeof(view));
		if (m_Stats.ClusteredLightingActive)
		{
			m_Profiler.BeginSection(m_CommandList, RenderTimingSection::ClusterBuild);
			const bool dispatched = m_Clusters.Dispatch(m_CommandList, m_Lights);
			m_Profiler.EndSection(m_CommandList, RenderTimingSection::ClusterBuild);
			if (dispatched)
			{
				m_Stats.ClusterDispatches = 1;
				m_Stats.ClusterPrefixUploadBytes = m_Lights.size() * sizeof(GPU::Light);
				m_Stats.ClusterLightTestsUpperBound = uint64_t(m_Stats.ClusterCells) * m_Lights.size();
			}
			else
			{
				view.Clusters.State.y = 0;
				m_ActiveViewBindings = m_ViewBindings;
				m_Stats.ClusteredLightingActive = false;
				m_Stats.ClusteredLightingFallbackReason = "cluster dispatch input mismatch";
				m_CommandList->writeBuffer(m_ViewConstants, &view, sizeof(view));
			}
		}
		m_Stats.ClusterOwnedBytes = m_Clusters.GetOwnedBytes();
		m_Stats.ClusterAllocationAttempts = m_Clusters.GetAllocationAttempts();
		m_Stats.ClusterBackingAttempts = m_Clusters.GetBackingAttempts();
		m_Stats.EffectOwnedBytes = m_EffectBudget.GetUsedBytes();
		m_Stats.EffectPeakBytes = m_EffectBudget.GetPeakBytes();
		m_Stats.EffectAllocationAttempts += m_Clusters.GetAllocationAttempts();
		if (m_Stats.ClusteredLightingFallbackReason != m_ReportedClusterFailure)
		{
			m_ReportedClusterFailure = m_Stats.ClusteredLightingFallbackReason;
			if (!m_ReportedClusterFailure.empty() && !m_Lights.empty())
				PX_CORE_WARN("Clustered lighting unavailable: {}", m_ReportedClusterFailure);
		}

		Entity volume;
		uint32_t enabledVolumes = 0;
		if (m_Settings.GIEnabled)
			for (auto handle : scene.GetAllEntitiesWith<GIVolumeComponent>())
			{
				Entity candidate(handle, &scene);
				if (candidate.GetComponent<GIVolumeComponent>().Enabled)
				{
					++enabledVolumes;
					if (!volume || uint64_t(candidate.GetUUID()) < uint64_t(volume.GetUUID()))
						volume = candidate;
				}
			}
		if (enabledVolumes <= 1)
			m_ReportedMultipleGIVolumes = false;
		if (enabledVolumes > 1 && !m_ReportedMultipleGIVolumes)
		{
			PX_CORE_WARN("Only one GI volume is supported; using the enabled volume with the lowest UUID");
			m_ReportedMultipleGIVolumes = true;
		}
		if (volume)
		{
			const auto& component = volume.GetComponent<GIVolumeComponent>();
			ProbeGISpecification specification;
			specification.Origin = glm::vec3(scene.GetWorldTransform(volume)[3]);
			specification.Spacing = component.ProbeSpacing;
			specification.Counts = {static_cast<uint32_t>(component.ProbeCountX), static_cast<uint32_t>(component.ProbeCountY), static_cast<uint32_t>(component.ProbeCountZ)};
			specification.RaysPerProbe = static_cast<uint32_t>(m_Settings.GIRaysPerProbe);
			specification.ProbesPerFrame = static_cast<uint32_t>(m_Settings.GIProbesPerFrame);
			specification.Hysteresis = m_Settings.GIHysteresis;
			specification.NormalBias = m_Settings.GINormalBias;
			specification.ViewBias = m_Settings.GIViewBias;
			specification.FreezeUpdates = m_Settings.GIFreeze && m_QueuedGIUpdates == 0;
			specification.VisibilityWeighting = m_Settings.GIVisibility;
			specification.BounceFeedback = m_Settings.GIBounceFeedback;
			specification.DebugProbe = m_Settings.GIDebugProbe;
			if (!m_ProbeGI.Update(scene, m_Assets, specification, m_CommandList, m_ViewConstants, m_LightBuffer, m_EnvironmentMaps.Radiance, view.LightCount))
			{
				m_ProbeGI.Disable(m_CommandList);
				m_QueuedGIUpdates = 0;
				if (!m_ReportedGIFailure && m_ProbeGI.IsSupported() && !m_ProbeGI.HasReportedUpdateFailure())
					PX_CORE_ERROR("Probe GI update failed: invalid volume, scene data or unavailable GPU resources");
				m_ReportedGIFailure = true;
			}
			else
			{
				m_ReportedGIFailure = false;
				if (m_QueuedGIUpdates > 0)
					--m_QueuedGIUpdates;
			}
		}
		else
		{
			m_QueuedGIUpdates = 0;
			m_ProbeGI.Disable(m_CommandList);
		}

		m_Profiler.BeginSection(m_CommandList, RenderTimingSection::Shadows);
		if (sunShadows)
			RenderShadows(view);
		RenderLocalShadows();
		m_Profiler.EndSection(m_CommandList, RenderTimingSection::Shadows);
		if (reflectionHistoryActive)
		{
			TemporalFrameSnapshot snapshot;
			snapshot.Scene = SceneToken(scene);
			snapshot.CameraIdentity = camera.Identity;
			snapshot.View = camera.View;
			snapshot.Projection = camera.Projection;
			snapshot.InternalSize = {m_Width, m_Height};
			snapshot.OutputSize = outputSize;
			uint64_t signature = HashCombine(TemporalLightingKey(scene, m_Assets, m_Settings), HashCombine(static_cast<uint64_t>(m_Stats.EffectivePath), m_TemporalResetGeneration));
			signature = HashCombine(signature, HashCombine(m_ProbeGI.GetUpdateCount(), static_cast<uint64_t>(m_Stats.EffectiveAA)));
			for (const auto& draw : m_Draws)
			{
				signature = HashCombine(signature, HashCombine(draw.EntityID, HashCombine(draw.MeshID, draw.SubmeshOrdinal)));
				signature = HashCombine(signature, HashCombine(draw.TemporalVersion, (draw.Blend ? 1u : 0u) | (draw.CastShadows ? 2u : 0u)));
				for (int column = 0; column < 4; ++column)
					for (int row = 0; row < 4; ++row)
						signature = HashCombine(signature, std::bit_cast<uint32_t>(draw.World[column][row]));
			}
			snapshot.Settings = signature;
			reflectionHistoryActive = reflections->GetState().Stage(snapshot, {});
		}
		m_Stats.SSRHistoryActive = reflectionHistoryActive;
		m_Stats.SSRHistoryEligible = reflectionHistoryActive && reflections->GetState().CanUseHistory();
		m_Stats.SSRHistoryReason = !reflections ? "private AA view without reflection history" : !reflections->GetFallbackReason().empty() ? reflections->GetFallbackReason()
																																		   : reflections->GetState().GetResetReason();
		m_Profiler.BeginSection(m_CommandList, RenderTimingSection::Scene);
		RenderForward(view, reflectionHistoryActive ? reflections : nullptr);
		m_Profiler.EndSection(m_CommandList, RenderTimingSection::Scene);

		if (temporalActive)
		{
			m_Profiler.BeginSection(m_CommandList, RenderTimingSection::AntiAliasing);
			aa.UploadAndClear(m_CommandList, temporalRecords);
			RenderTemporalGuides(aa);
			aa.Resolve(m_CommandList, glm::inverse(rasterProjection), std::isfinite(m_Settings.AAHistoryWeight) ? std::clamp(m_Settings.AAHistoryWeight, 0.0f, 0.95f) : 0.9f);
			m_Profiler.EndSection(m_CommandList, RenderTimingSection::AntiAliasing);
			m_Stats.AAPasses = 1u + (temporalOpaque ? 1u : 0u) + (temporalBlend ? 1u : 0u);
			++m_Stats.DrawCalls;
		}

		if (m_Bloom.IsReady())
		{
			m_Profiler.BeginSection(m_CommandList, RenderTimingSection::Bloom);
			m_Bloom.Render(m_CommandList, std::isfinite(m_Settings.BloomThreshold) ? std::clamp(m_Settings.BloomThreshold, 0.0f, 64.0f) : 1.0f,
						   std::isfinite(m_Settings.BloomSoftKnee) ? std::clamp(m_Settings.BloomSoftKnee, 0.0f, 1.0f) : 0.5f);
			m_Stats.BloomPasses = m_Bloom.GetPassCount();
			m_Stats.DrawCalls += m_Stats.BloomPasses;
			m_Profiler.EndSection(m_CommandList, RenderTimingSection::Bloom);
		}
		m_Profiler.BeginSection(m_CommandList, RenderTimingSection::Tonemap);
		// Tone map into the output, or native undithered linear LDR for spatial filtering.
		if (m_TonemapBindings && m_TonemapBindings->getDesc()->bindings[1].resourceHandle != sceneColor)
			m_TonemapBindings = nullptr;
		if (!m_TonemapBindings)
		{
			nvrhi::BindingSetDesc set;
			set.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(GPU::TonemapConstants)), nvrhi::BindingSetItem::Texture_SRV(0, sceneColor),
							nvrhi::BindingSetItem::Texture_SRV(1, m_Bloom.IsReady() ? m_Bloom.GetTexture() : m_Resources.GetWhiteTexture()),
							nvrhi::BindingSetItem::Sampler(0, m_PointClamp), nvrhi::BindingSetItem::Sampler(1, m_LinearClamp)};
			m_TonemapBindings = m_Device->createBindingSet(set, m_TonemapLayout);
		}
		nvrhi::GraphicsState tonemap;
		auto* tonemapOutput = spatialActive ? m_SpatialAA.GetFramebuffer() : fxaaActive ? m_FXAA.GetFramebuffer()
																						: output;
		tonemap.pipeline = GetTonemapPipeline(tonemapOutput);
		tonemap.framebuffer = tonemapOutput;
		tonemap.bindings = {m_TonemapBindings};
		tonemap.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(outputInfo.width), static_cast<float>(outputInfo.height)));
		m_CommandList->setGraphicsState(tonemap);
		GPU::TonemapConstants tonemapConstants{};
		tonemapConstants.Upscale = (m_Width > outputInfo.width || m_Height > outputInfo.height) ? 2.0f : (m_Width != outputInfo.width || m_Height != outputInfo.height) ? 1.0f
																																										: 0.0f;
		tonemapConstants.Dimensions = {m_Width, m_Height, outputInfo.width, outputInfo.height};
		tonemapConstants.BloomIntensity = m_Bloom.IsReady() ? std::clamp(m_Settings.BloomIntensity, 0.0f, 2.0f) : 0.0f;
		const bool rawDiagnostic = static_cast<uint32_t>(m_Settings.DebugView) >= 3;
		tonemapConstants.Exposure = (m_Settings.DebugView == RenderDebugView::LocalShadow || m_Settings.DebugView == RenderDebugView::AmbientOcclusion) ? 1.0f : (rawDiagnostic ? m_Settings.GIDebugScale : m_Settings.Exposure);
		tonemapConstants.Operator = rawDiagnostic ? 3u : static_cast<uint32_t>(m_Settings.Tonemapper);
		if (spatialActive)
			tonemapConstants.Operator |= 0x100u;
		if (fxaaActive)
			tonemapConstants.Operator |= 0x200u;
		m_CommandList->setPushConstants(&tonemapConstants, sizeof(tonemapConstants));
		m_CommandList->draw(nvrhi::DrawArguments().setVertexCount(3));

		m_Profiler.EndSection(m_CommandList, RenderTimingSection::Tonemap);
		if (spatialActive || fxaaActive)
		{
			m_Profiler.BeginSection(m_CommandList, RenderTimingSection::AntiAliasing);
			if (fxaaActive)
				m_FXAA.Render(m_CommandList, output);
			else
				m_SpatialAA.Render(m_CommandList, output);
			++m_Stats.AAPasses;
			++m_Stats.DrawCalls;
			m_Profiler.EndSection(m_CommandList, RenderTimingSection::AntiAliasing);
		}
		m_Profiler.BeginSection(m_CommandList, RenderTimingSection::Overlays);
		RenderProbeMarkers(camera, output);

		// Text goes over the tone-mapped image, so exposure never changes it; world-space text still respects depth.
		if (m_RenderScale.IsReady())
		{
			m_RenderScale.RenderDepth(m_CommandList);
			m_Stats.NativeDepthPasses = 1;
			++m_Stats.DrawCalls;
		}
		m_Stats.TextGlyphs = m_Text.Render(m_CommandList, scene, rasterProjection * camera.View, output,
										   m_RenderScale.IsReady() ? m_RenderScale.GetDepth() : m_DepthTarget.Get());
		m_Profiler.EndSection(m_CommandList, RenderTimingSection::Overlays);
		m_Profiler.EndFrame(m_CommandList, static_cast<uint32_t>(m_Stats.EffectivePath), m_Stats.SSAOActive, m_Stats.BloomActive, static_cast<uint32_t>(m_Stats.EffectiveAA), m_Stats.SSRActive, m_Stats.SSRHistoryActive, m_Stats.ClusteredLightingActive);
		m_CommandList->close();
		const double submittedAt = ProfilingTimeSeconds();
		m_Device->executeCommandList(m_CommandList);
		m_Profiler.Submitted(submittedAt);
		if (m_Stats.SSRActive)
			m_Reflections.CommitInspectionFrame(view, m_FrameIndex, static_cast<uint32_t>(m_Stats.EffectivePath));
		if (temporalActive)
			aa.Commit();
		if (reflectionHistoryActive)
			reflections->Commit();
		m_Stats.SSRHistoryValid = reflections && reflections->GetState().IsValid();
		m_Stats.SSRHistoryCompletedFrames = reflections ? reflections->GetState().GetCompletedFrames() : 0;
		if (reflections && reflections != &m_ReflectionHistory)
			reflections->DetachInputs();
		aa.m_LastPasses = m_Stats.AAPasses;
		m_Stats.AAHistoryValid = aa.GetState().IsValid();
		m_Stats.AACompletedFrames = aa.GetState().GetCompletedFrames();
		m_Stats.ProfilingEnabled = m_Settings.ProfilingEnabled;
		if (m_Settings.ProfilingEnabled)
		{
			m_Stats.CPURenderMilliseconds = (ProfilingTimeSeconds() - cpuStart) * 1000;
			m_Stats.ProfilingDevice = m_GraphicsDevice.GetInfo();
		}
		m_Stats.GPUTiming = m_Profiler.GetLatest();
		m_Stats.GPUProfilingStatus = m_Profiler.GetStatus();
		m_Stats.GPUProfilingPending = m_Profiler.GetPendingCount();
		m_Stats.GPUProfilingSkipped = m_Profiler.GetSkippedCount();
		m_Stats.GPUProfilingInvalid = m_Profiler.GetInvalidCount();
		m_Stats.GPUProfilingLastResult = m_Profiler.GetLastResultStatus();
	}

}
