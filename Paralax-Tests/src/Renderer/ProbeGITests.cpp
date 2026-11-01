#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/MaterialAsset.h>
#include <Paralax/Asset/MeshAsset.h>
#include <Paralax/Asset/TextureAsset.h>
#include <Paralax/Renderer/GraphicsDevice.h>
#include <Paralax/Renderer/ProbeGI.h>
#include <Paralax/Renderer/SceneRenderer.h>
#include <Paralax/Renderer/TextureReadback.h>
#include <Paralax/Scene/Entity.h>

#include <array>
#include <cstring>
#include <limits>

using namespace Paralax;

namespace {

	Entity AddCube(Scene& scene, glm::vec3 position, glm::vec3 scale, AssetHandle material = 0)
	{
		Entity entity = scene.CreateEntity("GI surface");
		entity.GetTransform().Translation = position;
		entity.GetTransform().Scale = scale;
		auto& mesh = entity.AddComponent<MeshComponent>();
		mesh.Mesh = AssetHandle(AssetManager::BuiltinCube);
		mesh.Materials = {material};
		return entity;
	}

	void Enclose(Scene& scene, AssetHandle material = 0)
	{
		for (int axis = 0; axis < 3; ++axis)
			for (float side : {-1.0f, 1.0f})
			{
				glm::vec3 position(0.0f), scale(4.2f);
				position[axis] = side * 2.0f;
				scale[axis] = 0.1f;
				AddCube(scene, position, scale, material);
			}
	}

	std::vector<glm::vec4> ReadProbe(nvrhi::IDevice* device, nvrhi::ITexture* texture, uint32_t layer = 0)
	{
		nvrhi::TextureDesc desc;
		desc.width = 8;
		desc.height = 8;
		desc.format = nvrhi::Format::RGBA32_FLOAT;
		desc.initialState = nvrhi::ResourceStates::CopyDest;
		desc.keepInitialState = true;
		auto staging = device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
		auto commands = device->createCommandList();
		REQUIRE(staging);
		REQUIRE(commands);
		nvrhi::TextureSlice source;
		source.arraySlice = layer;
		commands->open();
		commands->copyTexture(staging, nvrhi::TextureSlice(), texture, source);
		commands->close();
		device->executeCommandList(commands);
		device->waitForIdle();
		size_t pitch = 0;
		const auto* bytes = static_cast<const uint8_t*>(device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
		REQUIRE(bytes);
		std::vector<glm::vec4> result(64);
		for (size_t y = 0; y < 8; ++y)
			std::memcpy(result.data() + y * 8, bytes + y * pitch, 8 * sizeof(glm::vec4));
		device->unmapStagingTexture(staging);
		return result;
	}

	struct ProbeFixture
	{
		nvrhi::IDevice* Device;
		ProbeGI GI;
		nvrhi::CommandListHandle Commands;
		nvrhi::BufferHandle View, Lights;
		nvrhi::TextureHandle Sky;
		ProbeGISpecification Specification;
		GPU::ViewConstants ViewData{};
		std::vector<GPU::Light> LightData;

		explicit ProbeFixture(nvrhi::IDevice* device, ProbeGIResourceFactory* factory = nullptr, const GraphicsDeviceLimits* limits = nullptr)
			: Device(device)
			, GI(device, limits ? *limits : Testing::GetTestGraphicsDevice()->GetInfo().Limits, factory)
		{
			Commands = device->createCommandList();
			View = device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(ViewData)).setIsConstantBuffer(true).setIsVolatile(true).setMaxVersions(16));
			Lights = device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(GPU::Light) * GPU::MaxLights).setStructStride(sizeof(GPU::Light)).setInitialState(nvrhi::ResourceStates::ShaderResource).setKeepInitialState(true));
			nvrhi::TextureDesc sky;
			sky.width = sky.height = 1;
			sky.arraySize = 6;
			sky.dimension = nvrhi::TextureDimension::TextureCube;
			sky.format = nvrhi::Format::RGBA8_UNORM;
			sky.initialState = nvrhi::ResourceStates::ShaderResource;
			sky.keepInitialState = true;
			Sky = device->createTexture(sky);
			REQUIRE(Commands);
			REQUIRE(View);
			REQUIRE(Lights);
			REQUIRE(Sky);
			Commands->open();
			REQUIRE(GI.Initialize(Commands));
			const std::array<uint8_t, 4> color{51, 102, 204, 255};
			for (uint32_t face = 0; face < 6; ++face)
				Commands->writeTexture(Sky, face, 0, &color, sizeof(color));
			Commands->close();
			device->executeCommandList(Commands);
			Specification.Origin = {-0.5f, -0.5f, -0.5f};
			Specification.Counts = {2, 2, 2};
			Specification.ProbesPerFrame = 8;
			Specification.RaysPerProbe = 64;
			Specification.Hysteresis = 0.0f;
			ViewData.EnvironmentParams.x = 1.0f;
		}

		~ProbeFixture() { Device->waitForIdle(); }

		bool Update(Scene& scene, AssetManager& assets)
		{
			Commands->open();
			Commands->writeBuffer(View, &ViewData, sizeof(ViewData));
			if (!LightData.empty())
				Commands->writeBuffer(Lights, LightData.data(), LightData.size() * sizeof(GPU::Light));
			const bool success = GI.Update(scene, assets, Specification, Commands, View, Lights, Sky, static_cast<uint32_t>(LightData.size()));
			if (!success)
				GI.Disable(Commands);
			Commands->close();
			Device->executeCommandList(Commands);
			return success;
		}
	};

	class FailingProbeResources : public ProbeGIResourceFactory
	{
	public:
		enum class Kind
		{
			Buffer,
			Texture,
			Bindings
		};
		Kind FailureKind = Kind::Buffer;
		int Remaining = -1;
		bool Failed = false;
		bool Reject(Kind kind)
		{
			if (kind != FailureKind || Remaining < 0)
				return false;
			if (Remaining-- == 0)
			{
				Failed = true;
				return true;
			}
			return false;
		}
		nvrhi::BufferHandle CreateBuffer(nvrhi::IDevice* device, const nvrhi::BufferDesc& desc) override
		{
			return Reject(Kind::Buffer) ? nullptr : device->createBuffer(desc);
		}
		nvrhi::TextureHandle CreateTexture(nvrhi::IDevice* device, const nvrhi::TextureDesc& desc) override
		{
			return Reject(Kind::Texture) ? nullptr : device->createTexture(desc);
		}
		nvrhi::BindingSetHandle CreateBindingSet(nvrhi::IDevice* device, const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout) override
		{
			return Reject(Kind::Bindings) ? nullptr : device->createBindingSet(desc, layout);
		}
	};

	// Non-owning adapter view exercises the real forward renderer while advertising conservative GI limits.
	class LimitedGraphicsDevice : public GraphicsDevice
	{
	public:
		explicit LimitedGraphicsDevice(GraphicsDevice& device)
			: m_Device(device)
			, m_Info(device.GetInfo())
		{
			m_Info.Limits.MaxStorageBuffersPerStage = 4;
			m_Info.Limits.MaxStorageBuffersPerSet = 4;
		}
		nvrhi::IDevice* GetDevice() const override { return m_Device.GetDevice(); }
		nvrhi::IDevice* GetBackendDevice() const override { return m_Device.GetBackendDevice(); }
		const GraphicsDeviceInfo& GetInfo() const override { return m_Info; }
		uint32_t GetValidationErrorCount() const override { return m_Device.GetValidationErrorCount(); }
		void WaitForIdle() override { m_Device.WaitForIdle(); }
		void CollectGarbage() override { m_Device.CollectGarbage(); }
		void* GetNativeInstance() const override { return m_Device.GetNativeInstance(); }
		void* GetNativePhysicalDevice() const override { return m_Device.GetNativePhysicalDevice(); }
		void* GetNativeDevice() const override { return m_Device.GetNativeDevice(); }
		void* GetNativeGraphicsQueue() const override { return m_Device.GetNativeGraphicsQueue(); }
		uint32_t GetGraphicsQueueFamily() const override { return m_Device.GetGraphicsQueueFamily(); }
		void* GetVulkanProcAddress(const char* name) const override { return m_Device.GetVulkanProcAddress(name); }

	private:
		GraphicsDevice& m_Device;
		GraphicsDeviceInfo m_Info;
	};

}

TEST_CASE("ProbeGI: allocation failures preserve valid bindings and retry the current scene")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	for (bool previouslyValid : {false, true})
		for (auto kind : {FailingProbeResources::Kind::Buffer, FailingProbeResources::Kind::Texture, FailingProbeResources::Kind::Bindings})
			for (int allocation = 0; allocation < (kind == FailingProbeResources::Kind::Buffer ? 5 : kind == FailingProbeResources::Kind::Texture ? 4
																																				  : 3);
				 ++allocation)
			{
				CAPTURE(previouslyValid);
				CAPTURE(kind);
				CAPTURE(allocation);
				AssetManager assets;
				Scene scene;
				FailingProbeResources factory;
				ProbeFixture fixture(graphics.GetDevice(), &factory);
				if (previouslyValid)
					REQUIRE(fixture.Update(scene, assets));
				Enclose(scene);
				fixture.Specification.Counts = {3, 2, 2};
				factory.FailureKind = kind;
				factory.Remaining = allocation;
				CHECK_FALSE(fixture.Update(scene, assets));
				REQUIRE(factory.Failed);
				CHECK(fixture.GI.GetBindings() != nullptr);
				CHECK(fixture.GI.GetConstants().Origin.w == 0.0f);
				// Retry immediately: the renderer does not drain prior submissions after an allocation failure.
				REQUIRE(fixture.Update(scene, assets));
				auto recovered = ReadProbe(graphics.GetDevice(), fixture.GI.GetIrradiance());
				ProbeFixture fresh(graphics.GetDevice());
				fresh.Specification = fixture.Specification;
				REQUIRE(fresh.Update(scene, assets));
				CHECK(recovered == ReadProbe(graphics.GetDevice(), fresh.GI.GetIrradiance()));
			}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: partial initialization allocation failures release safely")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	auto* device = graphics.GetDevice();
	for (auto kind : {FailingProbeResources::Kind::Buffer, FailingProbeResources::Kind::Texture, FailingProbeResources::Kind::Bindings})
		for (int allocation = 0; allocation < (kind == FailingProbeResources::Kind::Texture ? 4 : 1); ++allocation)
		{
			CAPTURE(kind);
			CAPTURE(allocation);
			FailingProbeResources factory;
			factory.FailureKind = kind;
			factory.Remaining = allocation;
			auto commands = device->createCommandList();
			REQUIRE(commands);
			commands->open();
			{
				ProbeGI gi(device, graphics.GetInfo().Limits, &factory);
				CHECK_FALSE(gi.Initialize(commands));
				CHECK(factory.Failed);
			}
			commands->close();
			device->executeCommandList(commands);
			device->waitForIdle();
		}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: initialization and recording preconditions are verified in every configuration")
{
	Testing::ScopedAssertionCapture assertions;
	CHECK_THROWS_AS(ProbeGI(nullptr, {}), Testing::AssertionFailedError);
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto* device = Testing::GetTestGraphicsDevice()->GetDevice();
	ProbeGI gi(device, Testing::GetTestGraphicsDevice()->GetInfo().Limits);
	CHECK_THROWS_AS(gi.Initialize(nullptr), Testing::AssertionFailedError);
	CHECK_THROWS_AS(gi.Disable(nullptr), Testing::AssertionFailedError);
	Scene scene;
	AssetManager assets;
	CHECK_THROWS_AS(gi.Update(scene, assets, {}, nullptr, nullptr, nullptr, nullptr, 0), Testing::AssertionFailedError);
}

TEST_CASE("ProbeGI: restricted adapter limits reject unsupported work before validation")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	for (int restriction = 0; restriction < 5; ++restriction)
	{
		CAPTURE(restriction);
		auto limits = graphics.GetInfo().Limits;
		if (restriction == 0)
			limits.MaxStorageBuffersPerStage = 4;
		else if (restriction == 1)
			limits.MaxStorageBuffersPerSet = 4;
		else if (restriction == 2)
			limits.MaxTextureArrayLayers = 7;
		else if (restriction == 3)
			limits.MaxStorageBufferBytes = 4096;
		else
		{
			// The eight-probe ray buffer fits, but this mesh's triangle SRV exceeds the restricted range.
			limits.MaxStorageBufferBytes = 16384;
			for (int i = 0; i < 10; ++i)
				AddCube(scene, {static_cast<float>(i), 0.0f, 0.0f}, glm::vec3(1.0f));
		}
		Testing::ScopedLogCapture logs;
		ProbeFixture fixture(graphics.GetDevice(), nullptr, &limits);
		CHECK(fixture.GI.IsSupported() == (restriction >= 2));
		CHECK_FALSE(fixture.Update(scene, assets));
		CHECK(fixture.GI.GetBindings() != nullptr);
		CHECK(fixture.GI.GetConstants().Origin.w == 0.0f);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: unsupported adapters retain forward lighting and report failure once")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	LimitedGraphicsDevice graphics(*Testing::GetTestGraphicsDevice());
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	auto red = CreateRef<MaterialAsset>();
	red->Handle = AssetHandle(60013);
	red->BaseColorFactor = {1.0f, 0.0f, 0.0f, 1.0f};
	assets.AddMemoryAsset(red);
	AddCube(scene, {}, glm::vec3(1.0f), red->Handle);
	scene.CreateEntity("GI").AddComponent<GIVolumeComponent>();
	Testing::ScopedLogCapture logs;
	{
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		nvrhi::TextureDesc desc;
		desc.width = desc.height = 32;
		desc.format = nvrhi::Format::SRGBA8_UNORM;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		auto output = graphics.GetDevice()->createTexture(desc);
		REQUIRE(output);
		auto framebuffer = graphics.GetDevice()->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(output));
		REQUIRE(framebuffer);
		const auto camera = CameraView::LookAt({0.0f, 0.0f, 3.0f}, {}, glm::radians(45.0f), 1.0f);
		for (int frame = 0; frame < 3; ++frame)
			renderer.Render(scene, camera, framebuffer);
		const auto fallback = ReadbackTexture(graphics.GetDevice(), output);
		REQUIRE_FALSE(fallback.IsEmpty());
		CHECK(fallback.GetPixel(16, 16).r > fallback.GetPixel(16, 16).g + 20);
		renderer.GetSettings().GIEnabled = false;
		renderer.Render(scene, camera, framebuffer);
		CHECK(fallback.GetPixels() == ReadbackTexture(graphics.GetDevice(), output).GetPixels());
	}
	uint32_t failures = 0;
	for (const auto& entry : logs.GetEntries())
		if (entry.Level == spdlog::level::err && entry.Message.find("Probe GI update failed") != std::string::npos)
			++failures;
	CHECK(failures == 1);
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: disabled history and scene replacement cannot retain old lighting")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene sky, room;
	Enclose(room);
	{
		ProbeFixture fixture(graphics.GetDevice());
		fixture.Specification.ProbesPerFrame = 1;
		fixture.Specification.Hysteresis = 0.99f;
		REQUIRE(fixture.Update(sky, assets));
		fixture.Commands->open();
		fixture.GI.Disable(fixture.Commands);
		fixture.Commands->close();
		fixture.Device->executeCommandList(fixture.Commands);
		fixture.ViewData.EnvironmentParams.x = 0.0f;
		REQUIRE(fixture.Update(sky, assets));
		CHECK(ReadProbe(fixture.Device, fixture.GI.GetIrradiance(), 7)[27].b == 0.0f);
		fixture.ViewData.EnvironmentParams.x = 1.0f;
		REQUIRE(fixture.Update(room, assets));
		CHECK(ReadProbe(fixture.Device, fixture.GI.GetIrradiance(), 7)[27].b == 0.0f);
		REQUIRE(fixture.Update(sky, assets));
		CHECK(ReadProbe(fixture.Device, fixture.GI.GetIrradiance(), 7)[27].b == doctest::Approx(0.8).epsilon(0.001));
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: metallic surfaces suppress diffuse bounce")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	auto material = CreateRef<MaterialAsset>();
	material->Handle = AssetHandle(60008);
	material->MetallicFactor = 0.0f;
	assets.AddMemoryAsset(material);
	Enclose(scene, material->Handle);
	{
		ProbeFixture fixture(graphics.GetDevice());
		GPU::Light light{};
		light.PositionRange = {0.0f, 0.0f, 0.0f, 10.0f};
		light.ColorIntensity = {10.0f, 10.0f, 10.0f, 0.0f};
		fixture.LightData = {light};
		REQUIRE(fixture.Update(scene, assets));
		CHECK(ReadProbe(fixture.Device, fixture.GI.GetIrradiance())[27].r > 0.01f);
		material->MetallicFactor = 1.0f;
		++material->Version;
		REQUIRE(fixture.Update(scene, assets));
		for (const auto& pixel : ReadProbe(fixture.Device, fixture.GI.GetIrradiance()))
			CHECK(glm::vec3(pixel) == glm::vec3(0.0f));
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: reflected controls round trip and reject invalid settings")
{
	SceneRendererSettings settings;
	const auto fields = SceneRendererSettings::GetFields();
	std::string error;
	const nlohmann::json values = {{"GIEnabled", false}, {"GIRaysPerProbe", 128}, {"GIProbesPerFrame", 16}, {"GIHysteresis", 0.9f}, {"GINormalBias", 0.03f}, {"GIViewBias", 0.01f}, {"DebugView", "GICoverage"}};
	REQUIRE(Reflection::FromJson(fields, "Settings", &settings, values, error));
	SceneRendererSettings restored;
	REQUIRE(Reflection::FromJson(fields, "Settings", &restored, Reflection::ToJson(fields, &settings), error));
	CHECK(Reflection::ToJson(fields, &restored) == Reflection::ToJson(fields, &settings));
	for (const auto& field : fields)
	{
		if (std::string_view(field.Name).starts_with("GI"))
		{
			CHECK_FALSE(Reflection::SetField(field, &settings, "bad", error));
			if (std::string_view(field.Name) != "GIEnabled")
			{
				CHECK_FALSE(Reflection::SetField(field, &settings, -1, error));
				CHECK_FALSE(Reflection::SetField(field, &settings, 100000, error));
			}
		}
	}
}

TEST_CASE("ProbeTraceScene: snapshots geometry deterministically and invalidates transformed and edited assets")
{
	AssetManager assets;
	Scene scene;
	Entity cube = AddCube(scene, {100.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
	ProbeTraceScene trace;
	bool changed = false;
	REQUIRE(trace.Update(scene, assets, changed));
	CHECK(changed);
	CHECK(trace.GetTriangles().size() == 12);
	CHECK(trace.GetNodes().front().Minimum.x == doctest::Approx(99.5));
	CHECK(trace.GetNodes().front().Range.z == trace.GetNodes().size());
	REQUIRE(trace.Update(scene, assets, changed));
	CHECK_FALSE(changed);
	cube.GetTransform().Translation.x = -20.0f;
	REQUIRE(trace.Update(scene, assets, changed));
	CHECK(changed);
	CHECK(trace.GetNodes().front().Minimum.x == doctest::Approx(-20.5));
	auto material = CreateRef<MaterialAsset>();
	material->Handle = AssetHandle(60001);
	material->BaseColorFactor = {0.3f, 0.1f, 0.2f, 1.0f};
	assets.AddMemoryAsset(material);
	cube.GetComponent<MeshComponent>().Materials = {material->Handle};
	REQUIRE(trace.Update(scene, assets, changed));
	CHECK(changed);
	CHECK(trace.GetMaterials().front().BaseColor.r == doctest::Approx(0.3));
	material->BaseColorFactor.r = 0.7f;
	++material->Version;
	REQUIRE(trace.Update(scene, assets, changed));
	CHECK(changed);
	CHECK(trace.GetMaterials().front().BaseColor.r == doctest::Approx(0.7));
	scene.DestroyEntity(cube);
	REQUIRE(trace.Update(scene, assets, changed));
	CHECK(changed);
	CHECK(trace.GetTriangles().empty());
}

TEST_CASE("ProbeTraceScene: rejects invalid geometry and textures without replacing a valid snapshot")
{
	AssetManager assets;
	Scene scene;
	auto mesh = MeshPrimitives::CreateCube();
	mesh->Handle = AssetHandle(60002);
	assets.AddMemoryAsset(mesh);
	Entity cube = AddCube(scene, {}, glm::vec3(1.0f));
	cube.GetComponent<MeshComponent>().Mesh = mesh->Handle;
	ProbeTraceScene trace;
	bool changed = false;
	REQUIRE(trace.Update(scene, assets, changed));
	mesh->Indices[0] = std::numeric_limits<uint32_t>::max();
	++mesh->Version;
	Paralax::Testing::ScopedLogCapture logs;
	CHECK_FALSE(trace.Update(scene, assets, changed));
	CHECK_FALSE(changed);
	CHECK(trace.GetTriangles().size() == 12);
	const size_t errorCount = logs.GetEntries().size();
	CHECK_FALSE(trace.Update(scene, assets, changed));
	CHECK(logs.GetEntries().size() == errorCount);
	mesh->Indices[0] = 0;
	mesh->Vertices[0].Position.x = std::numeric_limits<float>::quiet_NaN();
	++mesh->Version;
	CHECK_FALSE(trace.Update(scene, assets, changed));
	mesh->Vertices[0].Position.x = 0.0f;
	++mesh->Version;
	SUBCASE("finite extreme coordinates are rejected before GPU arithmetic")
	{
		mesh->Vertices[0].Position.x = std::numeric_limits<float>::max();
		++mesh->Version;
		CHECK_FALSE(trace.Update(scene, assets, changed));
		mesh->Vertices[0].Position.x = 0.0f;
		++mesh->Version;
	}
	auto material = CreateRef<MaterialAsset>();
	material->Handle = AssetHandle(60003);
	auto texture = CreateRef<TextureAsset>(AssetType::Texture);
	texture->Handle = AssetHandle(60004);
	texture->Width = std::numeric_limits<uint32_t>::max();
	texture->Height = std::numeric_limits<uint32_t>::max();
	assets.AddMemoryAsset(texture);
	material->BaseColorTexture = texture->Handle;
	assets.AddMemoryAsset(material);
	cube.GetComponent<MeshComponent>().Materials = {material->Handle};
	CHECK_FALSE(trace.Update(scene, assets, changed));
	texture->Width = texture->Height = 1;
	texture->Format = TextureFormat::RGBA32F;
	texture->SRGB = false;
	texture->Mips = {std::vector<uint8_t>(16)};
	const glm::vec4 extreme(std::numeric_limits<float>::max());
	std::memcpy(texture->Mips[0].data(), &extreme, sizeof(extreme));
	++texture->Version;
	CHECK_FALSE(trace.Update(scene, assets, changed));
}

TEST_CASE("ProbeTraceScene: diffuse mip selection bounds memory without blurring alpha coverage")
{
	AssetManager assets;
	Scene scene;
	std::vector<uint8_t> pixels(1024 * 512 * 4, 128);
	auto texture = TextureAsset::CreateRGBA8(1024, 512, pixels.data(), true);
	texture->Handle = AssetHandle(60011);
	assets.AddMemoryAsset(texture);
	auto material = CreateRef<MaterialAsset>();
	material->Handle = AssetHandle(60012);
	material->BaseColorTexture = texture->Handle;
	assets.AddMemoryAsset(material);
	AddCube(scene, {}, glm::vec3(1.0f), material->Handle);
	ProbeTraceScene trace;
	bool changed = false;
	REQUIRE(trace.Update(scene, assets, changed));
	CHECK(trace.GetMaterials()[0].Textures[0].y == 512);
	CHECK(trace.GetMaterials()[0].Textures[0].z == 256);
	CHECK(trace.GetTexels().size() == 512 * 256 + 1);
	CHECK(trace.GetTexels()[1].r == doctest::Approx(0.215861).epsilon(0.001));
	material->Alpha = AlphaMode::Mask;
	++material->Version;
	REQUIRE(trace.Update(scene, assets, changed));
	CHECK(trace.GetMaterials()[0].Textures[0].y == 1024);
	CHECK(trace.GetTexels().size() == 1024 * 512 + 1);
	material->Alpha = AlphaMode::Opaque;
	++material->Version;
	texture->Mips[1].pop_back();
	++texture->Version;
	Testing::ScopedLogCapture logs;
	CHECK_FALSE(trace.Update(scene, assets, changed));
	CHECK_FALSE(changed);
	CHECK(trace.GetMaterials()[0].Textures[0].y == 1024);
}

TEST_CASE("ProbeGI: open sky conserves diffuse irradiance and thin enclosing walls block it")
{
	if (!Paralax::Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Paralax::Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	{
		ProbeFixture fixture(graphics.GetDevice());
		REQUIRE(fixture.Update(scene, assets));
		for (const auto& pixel : ReadProbe(graphics.GetDevice(), fixture.GI.GetIrradiance()))
		{
			CHECK(pixel.r == doctest::Approx(0.2).epsilon(0.001));
			CHECK(pixel.g == doctest::Approx(0.4).epsilon(0.001));
			CHECK(pixel.b == doctest::Approx(0.8).epsilon(0.001));
		}
		Enclose(scene);
		REQUIRE(fixture.Update(scene, assets));
		for (const auto& pixel : ReadProbe(graphics.GetDevice(), fixture.GI.GetIrradiance()))
		{
			CHECK(pixel.r == 0.0f);
			CHECK(pixel.g == 0.0f);
			CHECK(pixel.b == 0.0f);
			CHECK(pixel.a == 1.0f);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: emissive textures and alpha masks affect off-screen probe radiance")
{
	if (!Paralax::Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Paralax::Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	auto material = CreateRef<MaterialAsset>();
	material->Handle = AssetHandle(60005);
	material->EmissiveFactor = {1.0f, 1.0f, 1.0f};
	const uint8_t red[4] = {255, 0, 0, 255};
	auto texture = TextureAsset::CreateRGBA8(1, 1, red, true);
	texture->Handle = AssetHandle(60006);
	assets.AddMemoryAsset(texture);
	material->EmissiveTexture = texture->Handle;
	assets.AddMemoryAsset(material);
	Enclose(scene, material->Handle);
	{
		ProbeFixture fixture(graphics.GetDevice());
		REQUIRE(fixture.Update(scene, assets));
		for (const auto& pixel : ReadProbe(graphics.GetDevice(), fixture.GI.GetIrradiance()))
		{
			CHECK(pixel.r == doctest::Approx(1.0).epsilon(0.001));
			CHECK(pixel.g == 0.0f);
			CHECK(pixel.b == 0.0f);
		}
		material->Alpha = AlphaMode::Mask;
		const uint8_t transparent[4] = {255, 255, 255, 0};
		auto cutout = TextureAsset::CreateRGBA8(1, 1, transparent, true);
		cutout->Handle = AssetHandle(60007);
		assets.AddMemoryAsset(cutout);
		material->BaseColorTexture = cutout->Handle;
		++material->Version;
		REQUIRE(fixture.Update(scene, assets));
		for (const auto& pixel : ReadProbe(graphics.GetDevice(), fixture.GI.GetIrradiance()))
			CHECK(pixel.b == doctest::Approx(0.8).epsilon(0.001));
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: repeated runs are identical and budgets preserve untouched probes")
{
	if (!Paralax::Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Paralax::Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	AddCube(scene, {1.3f, 0.0f, 0.0f}, glm::vec3(0.4f));
	std::vector<glm::vec4> first;
	for (uint32_t run = 0; run < 2; ++run)
	{
		ProbeFixture fixture(graphics.GetDevice());
		fixture.Specification.ProbesPerFrame = 1;
		REQUIRE(fixture.Update(scene, assets));
		auto untouched = ReadProbe(graphics.GetDevice(), fixture.GI.GetIrradiance(), 7);
		REQUIRE(fixture.Update(scene, assets));
		CHECK(untouched == ReadProbe(graphics.GetDevice(), fixture.GI.GetIrradiance(), 7));
		for (int frame = 0; frame < 4; ++frame)
			REQUIRE(fixture.Update(scene, assets));
		std::vector<glm::vec4> values;
		for (uint32_t layer = 0; layer < 8; ++layer)
		{
			auto probe = ReadProbe(graphics.GetDevice(), fixture.GI.GetIrradiance(), layer);
			values.insert(values.end(), probe.begin(), probe.end());
		}
		if (run == 0)
			first = values;
		else
			CHECK(values == first);
		fixture.Specification.Counts = {3, 2, 2};
		REQUIRE(fixture.Update(scene, assets));
		CHECK(fixture.GI.GetIrradiance()->getDesc().arraySize == 12);
		fixture.Specification.Counts.x = std::numeric_limits<uint32_t>::max();
		CHECK_FALSE(fixture.Update(scene, assets));
		CHECK(fixture.GI.GetConstants().Origin.w == 0.0f);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: forward shading rejects exterior probes across thin walls and shows diffuse colour bleeding")
{
	if (!Paralax::Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Paralax::Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	Enclose(scene);
	auto& volume = scene.CreateEntity("GI volume").AddComponent<GIVolumeComponent>();
	volume.ProbeCountX = volume.ProbeCountY = volume.ProbeCountZ = 6;
	for (auto handle : scene.GetAllEntitiesWith<GIVolumeComponent>())
		Entity(handle, &scene).GetTransform().Translation = glm::vec3(-2.5f);
	{
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		renderer.GetSettings().DebugView = RenderDebugView::Indirect;
		renderer.GetSettings().GIRaysPerProbe = 256;
		renderer.GetSettings().GIProbesPerFrame = 216;
		renderer.GetSettings().GIHysteresis = 0.0f;
		renderer.GetSettings().Tonemapper = TonemapOperator::None;
		nvrhi::TextureDesc desc;
		desc.width = desc.height = 64;
		desc.format = nvrhi::Format::SRGBA8_UNORM;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		auto output = graphics.GetDevice()->createTexture(desc);
		auto framebuffer = graphics.GetDevice()->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(output));
		REQUIRE(output);
		REQUIRE(framebuffer);
		const CameraView camera = CameraView::LookAt({0.0f, 0.5f, 0.0f}, {0.0f, -2.0f, 0.0f}, glm::radians(35.0f), 1.0f);
		renderer.Render(scene, camera, framebuffer);
		Image dark = ReadbackTexture(graphics.GetDevice(), output);
		REQUIRE_FALSE(dark.IsEmpty());
		dark.SavePNG(Paralax::Testing::GetTestOutputDirectory() / "GI-ClosedRoom.png");
		const auto center = dark.GetPixel(32, 32);
		CHECK(center.r < 8);
		CHECK(center.g < 8);
		CHECK(center.b < 8);

		auto red = CreateRef<MaterialAsset>();
		red->Handle = AssetHandle(60010);
		red->BaseColorFactor = {0.9f, 0.03f, 0.03f, 1.0f};
		assets.AddMemoryAsset(red);
		for (auto handle : scene.GetAllEntitiesWith<MeshComponent>())
		{
			Entity wall(handle, &scene);
			if (wall.GetTransform().Translation.x > 1.9f)
				wall.GetComponent<MeshComponent>().Materials = {red->Handle};
		}
		Entity light = scene.CreateEntity("Moving light");
		light.GetTransform().Translation = {1.4f, 0.0f, 0.0f};
		auto& lamp = light.AddComponent<PointLightComponent>();
		lamp.Intensity = 10.0f;
		lamp.Range = 8.0f;
		for (int frame = 0; frame < 8; ++frame)
		{
			renderer.Render(scene, camera, framebuffer);
			// This direct renderer test has no application frame limiter or swapchain to bound in-flight work.
			graphics.WaitForIdle();
			graphics.CollectGarbage();
		}
		Image bleed = ReadbackTexture(graphics.GetDevice(), output);
		bleed.SavePNG(Paralax::Testing::GetTestOutputDirectory() / "GI-RedBounce.png");
		const auto bounced = bleed.GetPixel(32, 32);
		CHECK(bounced.r > bounced.g + 5);
		CHECK(bounced.r > 20);
		light.GetTransform().Translation = {6.0f, 0.0f, 0.0f};
		for (int frame = 0; frame < 24; ++frame)
		{
			renderer.Render(scene, camera, framebuffer);
			graphics.WaitForIdle();
			graphics.CollectGarbage();
		}
		const auto blocked = ReadbackTexture(graphics.GetDevice(), output).GetPixel(32, 32);
		CHECK(blocked.r < bounced.r / 2);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}
