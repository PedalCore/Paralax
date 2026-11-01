#include "pxpch.h"
#include "Paralax/Renderer/SceneRenderer.h"

#include "Paralax/Asset/AssetManager.h"
#include "Paralax/Asset/MaterialAsset.h"
#include "Paralax/Asset/MeshAsset.h"
#include "Paralax/Asset/TextureAsset.h"
#include "Paralax/Renderer/GraphicsDevice.h"
#include "Paralax/Renderer/ShaderLibrary.h"
#include "Paralax/Scene/Entity.h"
#include "Paralax/Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/component_wise.hpp>

#include <algorithm>
#include <array>

namespace Paralax {

	std::span<const FieldInfo> SceneRendererSettings::GetFields()
	{
		using Reflection::FieldOptions;
		using Reflection::MakeField;
		static const std::vector<FieldInfo> s_Fields = {
			MakeField<&SceneRendererSettings::Exposure>("Exposure", FieldOptions().Range(0.05f, 8.0f).Describe("Exposure multiplier applied before tonemapping.")),
			MakeField<&SceneRendererSettings::Tonemapper>("Tonemapper", FieldOptions().Enum({"AgX", "ACES", "None"})),
			MakeField<&SceneRendererSettings::Shadows>("Shadows", FieldOptions().Describe("Directional light shadows.")),
			MakeField<&SceneRendererSettings::ShadowResolution>("ShadowResolution", FieldOptions().Enum({"Low", "Medium", "High"}).Describe("Shadow map size per cascade: 1024, 2048 or 4096.")),
			MakeField<&SceneRendererSettings::ShadowDistance>("ShadowDistance", FieldOptions().Range(5.0f, 500.0f).Describe("Distance covered by the shadow cascades, in meters.")),
			MakeField<&SceneRendererSettings::ShadowSoftness>("ShadowSoftness", FieldOptions().Range(0.0f, 0.5f).Describe("Penumbra size in meters.")),
			MakeField<&SceneRendererSettings::ShadowCascadeBlend>("ShadowCascadeBlend", FieldOptions().Range(0.0f, 0.3f).Describe("Fraction of each shadow cascade blended into the next; zero disables blending.")),
			MakeField<&SceneRendererSettings::DefaultEnvironmentIntensity>("SkyIntensity", FieldOptions().Range(0.0f, 3.0f).Describe("Environment intensity when the scene has no SkyLight.")),
			MakeField<&SceneRendererSettings::GIEnabled>("GIEnabled", FieldOptions().Describe("Enable diffuse probe lighting in scenes with a GI volume.")),
			MakeField<&SceneRendererSettings::GIRaysPerProbe>("GIRaysPerProbe", FieldOptions().Range(32, 256)),
			MakeField<&SceneRendererSettings::GIProbesPerFrame>("GIProbesPerFrame", FieldOptions().Range(1, 1728)),
			MakeField<&SceneRendererSettings::GIHysteresis>("GIHysteresis", FieldOptions().Range(0.0f, 0.99f).Describe("Previous lighting retained on each probe update.")),
			MakeField<&SceneRendererSettings::GINormalBias>("GINormalBias", FieldOptions().Range(0.0f, 0.05f)),
			MakeField<&SceneRendererSettings::GIViewBias>("GIViewBias", FieldOptions().Range(0.0f, 0.05f)),
			MakeField<&SceneRendererSettings::DebugView>("DebugView", FieldOptions().Enum({"Lit", "Normals", "BaseColor", "Indirect", "GICoverage"})),
		};
		return s_Fields;
	}

	namespace {

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

	SceneRenderer::SceneRenderer(GraphicsDevice& device, AssetManager& assets)
		: m_GraphicsDevice(device)
		, m_Assets(assets)
		, m_Device(device.GetDevice())
		, m_Resources(device.GetDevice())
		, m_Environment(device.GetDevice())
		, m_Text(device.GetDevice())
		, m_ProbeGI(device.GetDevice(), device.GetInfo().Limits)
	{
	}

	SceneRenderer::~SceneRenderer()
	{
		m_GraphicsDevice.WaitForIdle();
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

		m_ViewConstants = m_Device->createBuffer(nvrhi::BufferDesc()
													 .setByteSize(sizeof(GPU::ViewConstants))
													 .setIsConstantBuffer(true)
													 .setIsVolatile(true)
													 .setMaxVersions(16)
													 .setDebugName("ViewConstants"));
		m_LightBuffer = m_Device->createBuffer(nvrhi::BufferDesc()
												   .setByteSize(sizeof(GPU::Light) * GPU::MaxLights)
												   .setStructStride(sizeof(GPU::Light))
												   .setInitialState(nvrhi::ResourceStates::ShaderResource)
												   .setKeepInitialState(true)
												   .setDebugName("Lights"));

		m_CommandList->open();
		m_Resources.Initialize(m_CommandList);
		const bool environmentReady = m_Environment.Initialize(m_CommandList);
		const bool textReady = m_Text.Initialize(m_CommandList);
		const bool probeReady = m_ProbeGI.Initialize(m_CommandList);
		m_CommandList->close();
		m_Device->executeCommandList(m_CommandList);
		if (!environmentReady || !textReady || !probeReady)
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
		m_ShadowMaskPS = ShaderLibrary::CreateShader(m_Device, "Shadow", "ShadowMaskPS", nvrhi::ShaderType::Pixel);
		m_SkyVS = ShaderLibrary::CreateShader(m_Device, "Sky", "SkyVS", nvrhi::ShaderType::Vertex);
		m_SkyPS = ShaderLibrary::CreateShader(m_Device, "Sky", "SkyPS", nvrhi::ShaderType::Pixel);
		m_TonemapVS = ShaderLibrary::CreateShader(m_Device, "Tonemap", "TonemapVS", nvrhi::ShaderType::Vertex);
		m_TonemapPS = ShaderLibrary::CreateShader(m_Device, "Tonemap", "TonemapPS", nvrhi::ShaderType::Pixel);
		if (!m_ForwardVS || !m_ForwardPS || !m_ShadowVS || !m_ShadowMaskPS || !m_SkyVS || !m_SkyPS || !m_TonemapVS || !m_TonemapPS)
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
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),
			nvrhi::BindingLayoutItem::Texture_SRV(1),
			nvrhi::BindingLayoutItem::Texture_SRV(2),
			nvrhi::BindingLayoutItem::Texture_SRV(3),
			nvrhi::BindingLayoutItem::Texture_SRV(4),
			nvrhi::BindingLayoutItem::Texture_SRV(5),
			nvrhi::BindingLayoutItem::Sampler(0),
			nvrhi::BindingLayoutItem::Sampler(1),
			nvrhi::BindingLayoutItem::Sampler(2),
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
							nvrhi::BindingLayoutItem::Sampler(0)};
		m_TonemapLayout = m_Device->createBindingLayout(tonemap);

		nvrhi::BindingSetDesc shadowViewSet;
		shadowViewSet.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(glm::mat4)), nvrhi::BindingSetItem::Sampler(0, m_LinearWrap)};
		m_ShadowViewBindings = m_Device->createBindingSet(shadowViewSet, m_ShadowViewLayout);

		return m_InputLayout && m_ViewLayout && m_MaterialLayout && m_ShadowViewLayout && m_TonemapLayout && m_ShadowViewBindings;
	}

	void SceneRenderer::EnsureTargets(uint32_t width, uint32_t height)
	{
		const bool sizeChanged = width != m_Width || height != m_Height;
		const bool shadowChanged = m_ShadowMapSize != m_Settings.GetShadowMapSize();
		if (!sizeChanged && !shadowChanged && m_HDRTarget && m_ShadowMap)
			return;

		m_GraphicsDevice.WaitForIdle();
		m_StaticLightingDirty = true;

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
			m_HDRTarget = m_Device->createTexture(hdr);

			nvrhi::TextureDesc depth;
			depth.width = width;
			depth.height = height;
			depth.format = s_DepthFormat;
			depth.isRenderTarget = true;
			depth.debugName = "SceneDepth";
			depth.initialState = nvrhi::ResourceStates::DepthWrite;
			depth.keepInitialState = true;
			depth.setClearValue(nvrhi::Color(0.0f)); // Reversed Z: far plane = 0.
			m_DepthTarget = m_Device->createTexture(depth);

			m_HDRFramebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_HDRTarget).setDepthAttachment(m_DepthTarget));
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
			shadow.bindingLayouts = {m_ShadowViewLayout, m_MaterialLayout};
			shadow.renderState.rasterState.setCullNone().setDepthBias(1).setSlopeScaleDepthBias(1.5f);
			shadow.renderState.rasterState.depthClipEnable = false; // Pancaking: casters behind the near plane still cast.
			shadow.renderState.depthStencilState.enableDepthTest().enableDepthWrite().setDepthFunc(nvrhi::ComparisonFunc::LessOrEqual);
			m_ShadowPipeline = m_Device->createGraphicsPipeline(shadow, m_ShadowFramebuffers[0]->getFramebufferInfo());

			nvrhi::GraphicsPipelineDesc masked = shadow;
			masked.PS = m_ShadowMaskPS;
			m_ShadowMaskPipeline = m_Device->createGraphicsPipeline(masked, m_ShadowFramebuffers[0]->getFramebufferInfo());
		}
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
		return &entry;
	}

	void SceneRenderer::CollectDraws(Scene& scene, const CameraView& camera)
	{
		m_Draws.clear();
		const glm::mat4 viewProjection = camera.Projection * camera.View;
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
				item.Mesh = gpuMesh;
				item.IndexOffset = submesh.IndexOffset;
				item.IndexCount = submesh.IndexCount;
				item.VertexOffset = submesh.VertexOffset;
				item.World = submeshWorld;
				item.Material = material->BindingSet;
				item.Blend = material->Blend;
				item.DoubleSided = material->DoubleSided;
				item.AlphaMask = material->AlphaMask;
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
				state.vertexBuffers = {nvrhi::VertexBufferBinding().setBuffer(draw.Mesh->VertexBuffer)};
				state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(draw.Mesh->IndexBuffer).setFormat(nvrhi::Format::R32_UINT);
				state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(m_ShadowMapSize), static_cast<float>(m_ShadowMapSize)));
				m_CommandList->setGraphicsState(state);

				const glm::mat4 worldLightViewProjection = view.CascadeViewProjection[cascade] * draw.World;
				m_CommandList->setPushConstants(&worldLightViewProjection, sizeof(worldLightViewProjection));
				m_CommandList->drawIndexed(nvrhi::DrawArguments().setVertexCount(draw.IndexCount).setStartIndexLocation(draw.IndexOffset).setStartVertexLocation(draw.VertexOffset));
				m_Stats.ShadowDrawCalls++;
			}
		}
	}

	void SceneRenderer::RenderForward(const GPU::ViewConstants& view)
	{
		m_CommandList->clearTextureFloat(m_HDRTarget, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
		m_CommandList->clearDepthStencilTexture(m_DepthTarget, nvrhi::AllSubresources, true, 0.0f, false, 0);

		nvrhi::Viewport viewport(static_cast<float>(m_Width), static_cast<float>(m_Height));
		auto drawItems = [&](bool blendPass) {
			for (const DrawItem& draw : m_Draws)
			{
				if (!draw.Visible || draw.Blend != blendPass)
					continue;

				nvrhi::GraphicsState state;
				state.pipeline = blendPass          ? m_BlendPipeline
								 : draw.DoubleSided ? m_OpaqueDoubleSidedPipeline
								 : draw.Mirrored    ? m_OpaqueMirroredPipeline
													: m_OpaquePipeline;
				state.framebuffer = m_HDRFramebuffer;
				state.bindings = {m_ViewBindings, draw.Material, m_ProbeGI.GetBindings()};
				state.vertexBuffers = {nvrhi::VertexBufferBinding().setBuffer(draw.Mesh->VertexBuffer)};
				state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(draw.Mesh->IndexBuffer).setFormat(nvrhi::Format::R32_UINT);
				state.viewport.addViewportAndScissorRect(viewport);
				m_CommandList->setGraphicsState(state);

				const GPU::DrawConstants constants = MakeDrawConstants(draw.World);
				m_CommandList->setPushConstants(&constants, sizeof(constants));
				m_CommandList->drawIndexed(nvrhi::DrawArguments().setVertexCount(draw.IndexCount).setStartIndexLocation(draw.IndexOffset).setStartVertexLocation(draw.VertexOffset));
				m_Stats.DrawCalls++;
			}
		};

		drawItems(false);

		// Sky fills the pixels no geometry covered.
		{
			nvrhi::GraphicsState state;
			state.pipeline = m_SkyPipeline;
			state.framebuffer = m_HDRFramebuffer;
			state.bindings = {m_ViewBindings};
			state.viewport.addViewportAndScissorRect(viewport);
			m_CommandList->setGraphicsState(state);
			const GPU::DrawConstants unused{};
			m_CommandList->setPushConstants(&unused, sizeof(unused));
			m_CommandList->draw(nvrhi::DrawArguments().setVertexCount(3));
		}

		drawItems(true);
		(void)view;
	}

	void SceneRenderer::Render(Scene& scene, const CameraView& camera, nvrhi::IFramebuffer* output)
	{
		PX_CORE_VERIFY(m_Initialized, "SceneRenderer::Render called before a successful Initialize");
		PX_CORE_VERIFY(output, "SceneRenderer::Render requires an output framebuffer");

		const nvrhi::FramebufferInfoEx& outputInfo = output->getFramebufferInfo();
		if (outputInfo.width == 0 || outputInfo.height == 0)
			return;

		m_Stats = SceneRenderStats();
		m_FrameIndex++;
		EnsureTargets(outputInfo.width, outputInfo.height);

		m_CommandList->open();

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

		m_Lights.clear();
		auto addLights = [&](auto view, bool spot) {
			for (entt::entity handle : view)
			{
				if (m_Lights.size() >= GPU::MaxLights)
					return;
				Entity entity(handle, &scene);
				const glm::mat4 world = scene.GetWorldTransform(entity);
				GPU::Light light{};
				if (spot)
				{
					const auto& component = entity.GetComponent<SpotLightComponent>();
					light.PositionRange = glm::vec4(glm::vec3(world[3]), component.Range);
					light.ColorIntensity = glm::vec4(component.Color * component.Intensity, 1.0f);
					light.DirectionCos = glm::vec4(glm::normalize(glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f))), std::cos(component.OuterConeAngle));
					light.Params = glm::vec4(std::cos(std::min(component.InnerConeAngle, component.OuterConeAngle - 0.001f)), 0.0f, 0.0f, 0.0f);
				}
				else
				{
					const auto& component = entity.GetComponent<PointLightComponent>();
					light.PositionRange = glm::vec4(glm::vec3(world[3]), component.Range);
					light.ColorIntensity = glm::vec4(component.Color * component.Intensity, 0.0f);
				}
				m_Lights.push_back(light);
			}
		};
		addLights(scene.GetAllEntitiesWith<PointLightComponent>(), false);
		addLights(scene.GetAllEntitiesWith<SpotLightComponent>(), true);
		m_Stats.Lights = static_cast<uint32_t>(m_Lights.size());
		if (!m_Lights.empty())
			m_CommandList->writeBuffer(m_LightBuffer, m_Lights.data(), m_Lights.size() * sizeof(GPU::Light));

		UpdateEnvironment(scene, hasSun ? sunDirection : glm::vec3(0.0f), sunColor);
		CollectDraws(scene, camera);

		GPU::ViewConstants view{};
		view.View = camera.View;
		view.Projection = camera.Projection;
		view.ViewProjection = camera.Projection * camera.View;
		view.InverseViewProjection = glm::inverse(view.ViewProjection);
		view.CameraPosition = glm::vec4(camera.Position, 1.0f);
		view.SunDirection = glm::vec4(hasSun ? sunDirection : glm::vec3(0.0f, -1.0f, 0.0f), hasSun ? 1.0f : 0.0f);
		view.SunColor = glm::vec4(sunColor, sunShadows ? 1.0f : 0.0f);
		view.EnvironmentParams = glm::vec4(m_EnvironmentIntensity, static_cast<float>(m_EnvironmentMaps.SpecularMipCount), m_EnvironmentRotation, m_ShowSky ? 1.0f : 0.0f);
		view.ViewportSize = glm::vec2(static_cast<float>(m_Width), static_cast<float>(m_Height));
		view.Exposure = m_Settings.Exposure;
		view.LightCount = static_cast<uint32_t>(m_Lights.size());
		view.DebugView = static_cast<uint32_t>(m_Settings.DebugView);
		if (sunShadows)
			ComputeCascades(camera, sunDirection, view);
		m_CommandList->writeBuffer(m_ViewConstants, &view, sizeof(view));

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
			if (!m_ProbeGI.Update(scene, m_Assets, specification, m_CommandList, m_ViewConstants, m_LightBuffer, m_EnvironmentMaps.Radiance, view.LightCount))
			{
				m_ProbeGI.Disable(m_CommandList);
				if (!m_ReportedGIFailure)
					PX_CORE_ERROR("Probe GI update failed: invalid volume, scene data or unavailable GPU resources");
				m_ReportedGIFailure = true;
			}
			else
				m_ReportedGIFailure = false;
		}
		else
			m_ProbeGI.Disable(m_CommandList);

		if (!m_ViewBindings || m_ViewBindings->getDesc()->bindings[3].resourceHandle != m_ShadowMap.Get() || m_ViewBindings->getDesc()->bindings[4].resourceHandle != m_EnvironmentMaps.Specular.Get())
		{
			nvrhi::BindingSetDesc set;
			set.bindings = {
				nvrhi::BindingSetItem::PushConstants(s_PushConstantSlot, sizeof(GPU::DrawConstants)),
				nvrhi::BindingSetItem::ConstantBuffer(0, m_ViewConstants),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_LightBuffer),
				nvrhi::BindingSetItem::Texture_SRV(1, m_ShadowMap),
				nvrhi::BindingSetItem::Texture_SRV(2, m_EnvironmentMaps.Specular),
				nvrhi::BindingSetItem::Texture_SRV(3, m_EnvironmentMaps.Diffuse),
				nvrhi::BindingSetItem::Texture_SRV(4, m_Environment.GetBRDFLut()),
				nvrhi::BindingSetItem::Texture_SRV(5, m_EnvironmentMaps.Radiance),
				nvrhi::BindingSetItem::Sampler(0, m_LinearWrap),
				nvrhi::BindingSetItem::Sampler(1, m_LinearClamp),
				nvrhi::BindingSetItem::Sampler(2, m_ShadowSampler),
			};
			m_ViewBindings = m_Device->createBindingSet(set, m_ViewLayout);
		}

		if (sunShadows)
			RenderShadows(view);
		RenderForward(view);

		// Tone map into the output.
		if (!m_TonemapBindings)
		{
			nvrhi::BindingSetDesc set;
			set.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(GPU::TonemapConstants)), nvrhi::BindingSetItem::Texture_SRV(0, m_HDRTarget),
							nvrhi::BindingSetItem::Sampler(0, m_PointClamp)};
			m_TonemapBindings = m_Device->createBindingSet(set, m_TonemapLayout);
		}
		nvrhi::GraphicsState tonemap;
		tonemap.pipeline = GetTonemapPipeline(output);
		tonemap.framebuffer = output;
		tonemap.bindings = {m_TonemapBindings};
		tonemap.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(outputInfo.width), static_cast<float>(outputInfo.height)));
		m_CommandList->setGraphicsState(tonemap);
		GPU::TonemapConstants tonemapConstants{};
		tonemapConstants.Exposure = m_Settings.Exposure;
		tonemapConstants.Operator = static_cast<uint32_t>(m_Settings.Tonemapper);
		m_CommandList->setPushConstants(&tonemapConstants, sizeof(tonemapConstants));
		m_CommandList->draw(nvrhi::DrawArguments().setVertexCount(3));

		// Text goes over the tone-mapped image, so exposure never changes it; world-space text still respects depth.
		m_Stats.TextGlyphs = m_Text.Render(m_CommandList, scene, camera.Projection * camera.View, output, m_DepthTarget);

		m_CommandList->close();
		m_Device->executeCommandList(m_CommandList);
	}

}
