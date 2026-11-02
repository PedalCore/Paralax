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

#include <glm/gtc/packing.hpp>
#include <glm/gtx/quaternion.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>

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
		desc.width = texture->getDesc().width;
		desc.height = texture->getDesc().height;
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
		std::vector<glm::vec4> result(desc.width * desc.height);
		for (size_t y = 0; y < desc.height; ++y)
			std::memcpy(result.data() + y * desc.width, bytes + y * pitch, desc.width * sizeof(glm::vec4));
		device->unmapStagingTexture(staging);
		return result;
	}

	void WriteUniformDistance(nvrhi::ICommandList* commands, nvrhi::ITexture* texture, uint32_t layer, glm::vec4 value)
	{
		const auto& desc = texture->getDesc();
		value.z = 1.0f; // Unit support keeps these explicitly seeded normalized moments unchanged.
		const std::vector<glm::vec4> pixels(desc.width * desc.height, value);
		commands->writeTexture(texture, layer, 0, pixels.data(), desc.width * sizeof(glm::vec4));
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
		uint32_t BufferCreates = 0, TextureCreates = 0, BindingCreates = 0;
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
			++BufferCreates;
			return Reject(Kind::Buffer) ? nullptr : device->createBuffer(desc);
		}
		nvrhi::TextureHandle CreateTexture(nvrhi::IDevice* device, const nvrhi::TextureDesc& desc) override
		{
			++TextureCreates;
			return Reject(Kind::Texture) ? nullptr : device->createTexture(desc);
		}
		nvrhi::BindingSetHandle CreateBindingSet(nvrhi::IDevice* device, const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout) override
		{
			++BindingCreates;
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

TEST_CASE("ProbeGI: visibility reconstruction remains continuous as all weights become small")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	ProbeFixture fixture(graphics.GetDevice());
	fixture.Specification.NormalBias = fixture.Specification.ViewBias = 0.0f;
	REQUIRE(fixture.Update(scene, assets));
	auto sample = [&](float excess, uint32_t activeMask = 1) {
		const float mean = glm::length(fixture.Specification.Origin) - excess;
		fixture.Commands->open();
		for (uint32_t layer = 0; layer < 8; ++layer)
		{
			std::array<glm::vec4, 64> irradiance, distance;
			irradiance.fill(glm::vec4(1.0f));
			distance.fill(glm::vec4(mean, mean * mean + 0.00001f, 0.0f, (activeMask & (1u << layer)) != 0 ? 1.0f : 0.0f));
			fixture.Commands->writeTexture(fixture.GI.GetIrradiance(), layer, 0, irradiance.data(), 8 * sizeof(glm::vec4));
			WriteUniformDistance(fixture.Commands, fixture.GI.GetDistance(), layer, distance[0]);
		}
		fixture.Commands->close();
		fixture.Device->executeCommandList(fixture.Commands);
		auto result = fixture.GI.Inspect({}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
		REQUIRE(result);
		return *result;
	};
	const auto near = sample(0.008f), far = sample(0.010f);
	CAPTURE(near.Summary.x);
	CAPTURE(far.Summary.x);
	CAPTURE(near.Irradiance.r);
	CAPTURE(far.Irradiance.r);
	CHECK(std::abs(near.Irradiance.r - far.Irradiance.r) < 0.2f);
	CHECK(sample(-0.01f, 255).Irradiance.r > 0.99f);
	CHECK(sample(1.0f, 255).Irradiance.r < 0.001f);
	CHECK(sample(-0.01f, 0).Irradiance.r == 0.0f);
	float previous = 1.0f;
	for (int step = 0; step <= 40; ++step)
	{
		const float value = sample(static_cast<float>(step) * 0.001f, 255).Irradiance.r;
		CHECK(std::isfinite(value));
		CHECK(value <= previous + 0.001f);
		previous = value;
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: normal bias preserves valid neighbour support across a probe grid plane")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	ProbeFixture fixture(graphics.GetDevice());
	fixture.Specification.Origin = {};
	fixture.Specification.Counts = {3, 3, 3};
	fixture.Specification.NormalBias = 0.05f;
	fixture.Specification.ViewBias = 0.0f;
	REQUIRE(fixture.Update(scene, assets));
	fixture.Commands->open();
	for (uint32_t layer = 0; layer < 27; ++layer)
	{
		std::array<glm::vec4, 64> irradiance, distance;
		irradiance.fill(glm::vec4(1.0f));
		distance.fill(glm::vec4(3.0f, 9.0f, 0.0f, (layer / 3) % 3 == 2 ? 1.0f : 0.0f));
		fixture.Commands->writeTexture(fixture.GI.GetIrradiance(), layer, 0, irradiance.data(), 8 * sizeof(glm::vec4));
		WriteUniformDistance(fixture.Commands, fixture.GI.GetDistance(), layer, distance[0]);
	}
	fixture.Commands->close();
	fixture.Device->executeCommandList(fixture.Commands);
	const auto onPlane = fixture.GI.Inspect({0.5f, 1.0f, 0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
	const auto abovePlane = fixture.GI.Inspect({0.5f, 1.0001f, 0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
	REQUIRE(onPlane);
	REQUIRE(abovePlane);
	CAPTURE(onPlane->Summary.x);
	CAPTURE(abovePlane->Summary.x);
	CHECK(onPlane->Irradiance.r == doctest::Approx(1.0).epsilon(0.001));
	CHECK(std::abs(onPlane->Irradiance.r - abovePlane->Irradiance.r) < 0.001f);
	// Cross the actual biased cell boundary with vanishing eligible support on the far side.
	const auto belowBiasedPlane = fixture.GI.Inspect({0.5f, 0.95f - 1e-6f, 0.5f}, {0, 1, 0}, {0, 0, 1});
	const auto aboveBiasedPlane = fixture.GI.Inspect({0.5f, 0.95f + 1e-6f, 0.5f}, {0, 1, 0}, {0, 0, 1});
	REQUIRE(belowBiasedPlane);
	REQUIRE(aboveBiasedPlane);
	CHECK(belowBiasedPlane->Irradiance.r == 0.0f);
	CHECK(aboveBiasedPlane->Irradiance.r < 0.03f);
	for (const auto& [position, normal] : {std::pair{glm::vec3(0), glm::vec3(-1, 0, 0)},
										   std::pair{glm::vec3(2), glm::vec3(1, 0, 0)}, std::pair{glm::vec3(0, 2, 0), glm::vec3(0, 1, 0)},
										   std::pair{glm::vec3(0, 2.0f - 1e-6f, 0), glm::vec3(0, -1, 0)}})
	{
		const auto edge = fixture.GI.Inspect(position, normal, {0, 0, 1});
		REQUIRE(edge);
		CHECK(std::isfinite(edge->Irradiance.r));
		for (const auto& neighbour : edge->Neighbours)
		{
			CHECK(neighbour.Metadata.x < 27);
			CHECK(std::isfinite(neighbour.Weights.y));
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: inspection reports shared GPU weights and rejects invalid queries")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	ProbeFixture fixture(graphics.GetDevice());
	fixture.Specification.NormalBias = fixture.Specification.ViewBias = 0.0f;
	CHECK_FALSE(fixture.GI.Inspect({}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}));
	REQUIRE(fixture.Update(scene, assets));
	auto query = fixture.GI.Inspect({}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
	REQUIRE(query);
	CHECK(query->Irradiance.r == doctest::Approx(0.2).epsilon(0.001));
	CHECK(query->Irradiance.g == doctest::Approx(0.4).epsilon(0.001));
	CHECK(query->Irradiance.b == doctest::Approx(0.8).epsilon(0.001));
	CHECK(query->Summary.y == 8.0f);
	float sum = 0.0f;
	for (uint32_t i = 0; i < 8; ++i)
	{
		const auto& neighbour = query->Neighbours[i];
		CHECK(neighbour.Metadata.x == i);
		CHECK(neighbour.Metadata.y == 1);
		CHECK(neighbour.Moments.x == doctest::Approx(3.0).epsilon(0.001));
		CHECK(neighbour.Moments.y == doctest::Approx(9.0).epsilon(0.001));
		sum += neighbour.IrradianceWeight.w;
	}
	CHECK(sum == doctest::Approx(1.0).epsilon(0.0001));
	query = fixture.GI.Inspect(fixture.Specification.Origin, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
	REQUIRE(query);
	CHECK(query->Neighbours[0].IrradianceWeight.w > 0.9f);
	CHECK_FALSE(fixture.GI.Inspect({100.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}));
	CHECK_FALSE(fixture.GI.Inspect({}, {}, {0.0f, 0.0f, 1.0f}));
	CHECK_FALSE(fixture.GI.Inspect({}, {0.0f, 1.0f, 0.0f}, {std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f}));
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: exact inspection distinguishes blocked neighbours and bias crossing thin geometry")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	auto material = CreateRef<MaterialAsset>();
	material->Handle = AssetHandle(60119);
	bool maskedHole = false;
	bool castShadows = true;
	glm::vec3 translation(0.0f);
	SUBCASE("opaque wall") {}
	SUBCASE("non-shadowing wall still blocks probe visibility")
	{
		castShadows = false;
	}
	SUBCASE("large translation on an unrelated axis")
	{
		translation.z = 500000.0f;
	}
	SUBCASE("alpha mask hole")
	{
		maskedHole = true;
		material->Alpha = AlphaMode::Mask;
		material->BaseColorFactor.a = 0.0f;
	}
	assets.AddMemoryAsset(material);
	AddCube(scene, translation, {0.04f, 4.0f, 4.0f}, material->Handle).GetComponent<MeshComponent>().CastShadows = castShadows;
	ProbeFixture fixture(graphics.GetDevice());
	fixture.Specification.Origin += translation;
	fixture.Specification.NormalBias = 0.0f;
	fixture.Specification.ViewBias = 0.25f;
	REQUIRE(fixture.Update(scene, assets));
	// Deliberately false moments model a leak; only segment visibility should reject the far side.
	fixture.Commands->open();
	for (uint32_t layer = 0; layer < 8; ++layer)
	{
		std::array<glm::vec4, 64> irradiance, distance;
		irradiance.fill(glm::vec4(layer % 2 == 0 ? 0.1f : 10.0f));
		distance.fill({3.0f, 9.0f, 0.0f, 1.0f});
		fixture.Commands->writeTexture(fixture.GI.GetIrradiance(), layer, 0, irradiance.data(), 8 * sizeof(glm::vec4));
		WriteUniformDistance(fixture.Commands, fixture.GI.GetDistance(), layer, distance[0]);
	}
	fixture.Commands->close();
	fixture.Device->executeCommandList(fixture.Commands);
	const auto before = ReadProbe(fixture.Device, fixture.GI.GetIrradiance());
	const uint32_t updates = fixture.GI.GetUpdateCount();
	const auto query = fixture.GI.Inspect(translation + glm::vec3(-0.1f, 0.0f, 0.0f), {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
	REQUIRE(query);
	CHECK(query->Irradiance.r > 5.0f);
	for (uint32_t i = 0; i < 8; ++i)
	{
		const auto& neighbour = query->Neighbours[i];
		CHECK(neighbour.SurfaceRay.Metadata.z == (i % 2 != 0 && !maskedHole ? 1 : 0));
		CHECK(neighbour.BiasedRay.Metadata.z == (i % 2 == 0 && !maskedHole ? 1 : 0));
		CHECK((neighbour.SurfaceRay.Metadata.w & 2) == 0);
		CHECK((neighbour.BiasedRay.Metadata.w & 2) == 0);
		CHECK(neighbour.SurfaceRay.OriginLength.x == doctest::Approx(-0.1));
		CHECK(neighbour.BiasedRay.OriginLength.x == doctest::Approx(0.15));
		if (neighbour.SurfaceRay.Metadata.z != 0)
		{
			CHECK(neighbour.SurfaceRay.HitPositionDistance.x == doctest::Approx(-0.02).epsilon(0.001));
			CHECK(neighbour.SurfaceRay.HitPositionDistance.w < neighbour.SurfaceRay.OriginLength.w);
			CHECK(neighbour.SurfaceRay.Metadata.w == 0);
		}
		if (neighbour.BiasedRay.Metadata.z != 0)
			CHECK(neighbour.BiasedRay.HitPositionDistance.x == doctest::Approx(0.02).epsilon(0.001));
	}
	CHECK(query->SurfaceRejected.r == doctest::Approx(static_cast<double>(maskedHole ? query->Irradiance.r : 0.1f)).epsilon(0.001));
	CHECK(query->SurfaceExact.r == doctest::Approx(static_cast<double>(query->SurfaceRejected.r)).epsilon(0.001));
	CHECK(query->BiasedRejected.r == doctest::Approx(static_cast<double>(maskedHole ? query->Irradiance.r : 10.0f)).epsilon(0.001));
	CHECK(query->BiasedExact.r == doctest::Approx(static_cast<double>(query->BiasedRejected.r)).epsilon(0.001));
	const auto coincident = fixture.GI.Inspect(fixture.Specification.Origin, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
	REQUIRE(coincident);
	CHECK(coincident->Neighbours[0].SurfaceRay.OriginLength.w == 0.0f);
	CHECK(coincident->Neighbours[0].SurfaceRay.Metadata.z == 0);
	CHECK(fixture.GI.GetUpdateCount() == updates);
	CHECK(ReadProbe(fixture.Device, fixture.GI.GetIrradiance()) == before);
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: inspection marks unresolvable segments indeterminate")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	ProbeFixture fixture(graphics.GetDevice());
	fixture.Specification.NormalBias = fixture.Specification.ViewBias = 0.0f;
	bool largeCoordinate = false;
	SUBCASE("short segments") {}
	SUBCASE("normal offset lost to floating point rounding")
	{
		largeCoordinate = true;
		fixture.Specification.Origin.x = 500000.0f;
	}
	REQUIRE(fixture.Update(scene, assets));
	if (largeCoordinate)
	{
		const auto query = fixture.GI.Inspect(fixture.Specification.Origin + glm::vec3(0.25f), {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
		REQUIRE(query);
		for (const auto& neighbour : query->Neighbours)
			CHECK((neighbour.SurfaceRay.Metadata.w & 2) != 0);
		CHECK(query->SurfaceExact.w == 0.0f);
	}
	else
		for (float distance : {0.0001f, 0.00025f})
		{
			const auto query = fixture.GI.Inspect(fixture.Specification.Origin + glm::vec3(distance, 0.0f, 0.0f), {-1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f});
			REQUIRE(query);
			CHECK((query->Neighbours[0].SurfaceRay.Metadata.w & 2) != 0);
		}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: frozen updates preserve history and an explicit reset refills once")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	ProbeFixture fixture(graphics.GetDevice());
	fixture.Specification.FreezeUpdates = true;
	REQUIRE(fixture.Update(scene, assets));
	CHECK(fixture.GI.GetUpdateCount() == 1);
	const auto before = ReadProbe(fixture.Device, fixture.GI.GetIrradiance());
	fixture.ViewData.EnvironmentParams.x = 0.0f;
	for (int frame = 0; frame < 3; ++frame)
		REQUIRE(fixture.Update(scene, assets));
	CHECK(fixture.GI.GetUpdateCount() == 1);
	CHECK(ReadProbe(fixture.Device, fixture.GI.GetIrradiance()) == before);
	fixture.GI.ResetHistory();
	REQUIRE(fixture.Update(scene, assets));
	CHECK(fixture.GI.GetUpdateCount() == 1);
	CHECK(ReadProbe(fixture.Device, fixture.GI.GetIrradiance())[27].b == 0.0f);
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: stable updates reuse bindings and resource replacement invalidates both directions")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	FailingProbeResources factory;
	ProbeFixture fixture(graphics.GetDevice(), &factory);
	REQUIRE(fixture.Update(scene, assets));
	REQUIRE(fixture.Update(scene, assets));
	const auto buffers = factory.BufferCreates, textures = factory.TextureCreates, bindings = factory.BindingCreates;
	for (int frame = 0; frame < 4; ++frame)
		REQUIRE(fixture.Update(scene, assets));
	CHECK(factory.BufferCreates == buffers);
	CHECK(factory.TextureCreates == textures);
	CHECK(factory.BindingCreates == bindings);
	ProbeFixture alternate(graphics.GetDevice());
	fixture.Sky = alternate.Sky;
	REQUIRE(fixture.Update(scene, assets));
	REQUIRE(fixture.Update(scene, assets));
	CHECK(factory.BindingCreates == bindings + 2);
	fixture.View = alternate.View;
	fixture.Lights = alternate.Lights;
	REQUIRE(fixture.Update(scene, assets));
	REQUIRE(fixture.Update(scene, assets));
	CHECK(factory.BindingCreates == bindings + 4);
	REQUIRE(fixture.Update(scene, assets));
	CHECK(factory.BindingCreates == bindings + 4);
	fixture.Sky = nullptr;
	CHECK_FALSE(fixture.Update(scene, assets));
	CHECK(fixture.GI.GetConstants().Origin.w == 0.0f);
	fixture.Sky = alternate.Sky;
	REQUIRE(fixture.Update(scene, assets));
	CHECK(fixture.GI.GetConstants().Update.w == 0);
	graphics.WaitForIdle();
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: static geometry edits upload the scene while preserving budget and untouched history")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	Entity cube = AddCube(scene, {100.0f, 0.0f, 0.0f}, glm::vec3(1.0f));
	cube.AddComponent<RigidbodyComponent>().Type = RigidbodyType::Static;
	FailingProbeResources factory;
	ProbeFixture fixture(graphics.GetDevice(), &factory);
	fixture.Specification.Spacing = 2.0f;
	fixture.Specification.ProbesPerFrame = 1;
	fixture.Specification.Hysteresis = 0.9f;
	REQUIRE(fixture.Update(scene, assets));
	const auto previous = ReadProbe(fixture.Device, fixture.GI.GetIrradiance(), 7);
	const auto previousDistance = ReadProbe(fixture.Device, fixture.GI.GetDistance(), 7);
	const auto buffers = factory.BufferCreates, textures = factory.TextureCreates;
	cube.GetTransform().Translation = fixture.Specification.Origin;
	REQUIRE(fixture.Update(scene, assets));
	CHECK(factory.BufferCreates == buffers + 4);
	CHECK(factory.TextureCreates == textures);
	CHECK(fixture.GI.GetConstants().Update.x == 0);
	CHECK(fixture.GI.GetConstants().Update.y == 1);
	CHECK(fixture.GI.GetConstants().Update.w == 1);
	CHECK(fixture.GI.GetConstants().Tuning.z == doctest::Approx(0.9));
	CHECK(ReadProbe(fixture.Device, fixture.GI.GetIrradiance(), 7) == previous);
	CHECK(ReadProbe(fixture.Device, fixture.GI.GetDistance(), 7) == previousDistance);
	// Newly uploaded geometry encloses probe zero. Classification must see it immediately even though
	// other probes keep their old values until their scheduled update.
	CHECK(ReadProbe(fixture.Device, fixture.GI.GetIrradiance(), 0)[27].a == 0.0f);
	CHECK(ReadProbe(fixture.Device, fixture.GI.GetDistance(), 0)[27].a == 0.0f);
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeTraceScene: moving rigid-body hierarchies do not invalidate static geometry")
{
	for (auto type : {RigidbodyType::Dynamic, RigidbodyType::Kinematic})
	{
		CAPTURE(type);
		AssetManager assets;
		Scene scene;
		AddCube(scene, {10.0f, 0.0f, 0.0f}, glm::vec3(1.0f));
		Entity body = AddCube(scene, {}, glm::vec3(1.0f));
		body.AddComponent<RigidbodyComponent>().Type = type;
		Entity child = AddCube(scene, {}, glm::vec3(1.0f));
		Entity intermediate = scene.CreateEntity("Body mesh parent");
		REQUIRE(scene.SetParent(intermediate, body, false));
		REQUIRE(scene.SetParent(child, intermediate, false));
		ProbeTraceScene trace;
		bool changed = false;
		REQUIRE(trace.Update(scene, assets, changed));
		CHECK(changed);
		CHECK(trace.GetTriangles().size() == 12);
		CHECK(trace.GetNodes().front().Minimum.x == doctest::Approx(9.5));
		body.GetTransform().Translation.x = 20.0f;
		child.GetTransform().Translation.y = 2.0f;
		REQUIRE(trace.Update(scene, assets, changed));
		CHECK_FALSE(changed);
		CHECK(trace.GetTriangles().size() == 12);
		body.GetComponent<RigidbodyComponent>().Type = RigidbodyType::Static;
		REQUIRE(trace.Update(scene, assets, changed));
		CHECK(changed);
		CHECK(trace.GetTriangles().size() == 36);
		body.GetComponent<RigidbodyComponent>().Type = type;
		REQUIRE(trace.Update(scene, assets, changed));
		CHECK(changed);
		CHECK(trace.GetTriangles().size() == 12);
	}
}

TEST_CASE("ProbeGI: allocation failures preserve valid bindings and retry the current scene")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	for (bool previouslyValid : {false, true})
		for (auto kind : {FailingProbeResources::Kind::Buffer, FailingProbeResources::Kind::Texture, FailingProbeResources::Kind::Bindings})
			for (int allocation = 0;; ++allocation)
			{
				REQUIRE(allocation < 32);
				CAPTURE(previouslyValid);
				CAPTURE(kind);
				CAPTURE(allocation);
				AssetManager assets;
				Scene scene;
				FailingProbeResources factory;
				Testing::ScopedLogCapture failureLogs;
				ProbeFixture fixture(graphics.GetDevice(), &factory);
				if (previouslyValid)
					REQUIRE(fixture.Update(scene, assets));
				Enclose(scene);
				fixture.Specification.Counts = {3, 2, 2};
				factory.FailureKind = kind;
				factory.Remaining = allocation;
				const bool updated = fixture.Update(scene, assets);
				if (!factory.Failed)
				{
					REQUIRE(updated);
					CHECK(allocation == (kind == FailingProbeResources::Kind::Buffer ? 5 : kind == FailingProbeResources::Kind::Texture ? 4
																																		: 3));
					break;
				}
				CHECK_FALSE(updated);
				if (kind == FailingProbeResources::Kind::Buffer && allocation < 4)
				{
					const std::array<const char*, 4> names{"ProbeTraceNodes", "ProbeTraceTriangles", "ProbeTraceMaterials", "ProbeTraceTexels"};
					CHECK(failureLogs.Contains(spdlog::level::err, names[static_cast<size_t>(allocation)]));
					factory.Remaining = allocation;
					CHECK_FALSE(fixture.Update(scene, assets));
					CHECK(failureLogs.Count(spdlog::level::err, "Probe GI upload refused") == 1);
				}
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
		for (int allocation = 0;; ++allocation)
		{
			REQUIRE(allocation < 32);
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
				const bool initialized = gi.Initialize(commands);
				CHECK(initialized == !factory.Failed);
				commands->close();
				device->executeCommandList(commands);
				device->waitForIdle();
			}
			if (!factory.Failed)
			{
				CHECK(allocation == (kind == FailingProbeResources::Kind::Texture ? 4 : kind == FailingProbeResources::Kind::Bindings ? 2
																																	  : 1));
				break;
			}
		}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: initialization and recording preconditions are verified in every configuration")
{
	Testing::ScopedAssertionCapture assertions;
	CHECK_THROWS_AS(ProbeGI(nullptr, {}), Testing::AssertionFailedError);
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	auto* device = graphics.GetDevice();
	ProbeGI gi(device, Testing::GetTestGraphicsDevice()->GetInfo().Limits);
	CHECK_THROWS_AS(gi.Initialize(nullptr), Testing::AssertionFailedError);
	CHECK_THROWS_AS(gi.Disable(nullptr), Testing::AssertionFailedError);
	Scene scene;
	AssetManager assets;
	CHECK_THROWS_AS(gi.Update(scene, assets, {}, nullptr, nullptr, nullptr, nullptr, 0), Testing::AssertionFailedError);
	ProbeFixture fixture(device);
	fixture.Commands->open();
	CHECK_THROWS_AS(fixture.GI.Update(scene, assets, fixture.Specification, nullptr, fixture.View, fixture.Lights, fixture.Sky, 0), Testing::AssertionFailedError);
	CHECK_THROWS_AS(fixture.GI.Update(scene, assets, fixture.Specification, fixture.Commands, nullptr, fixture.Lights, fixture.Sky, 0), Testing::AssertionFailedError);
	CHECK_THROWS_AS(fixture.GI.Update(scene, assets, fixture.Specification, fixture.Commands, fixture.View, nullptr, fixture.Sky, 0), Testing::AssertionFailedError);
	CHECK_THROWS_AS(fixture.GI.Disable(nullptr), Testing::AssertionFailedError);
	fixture.Commands->close();
	device->executeCommandList(fixture.Commands);
	device->waitForIdle();
	CHECK(graphics.GetValidationErrorCount() == errors);
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
		if (restriction < 2)
			CHECK(logs.Contains(spdlog::level::warn, "six storage-buffer descriptors"));
		CHECK_FALSE(fixture.Update(scene, assets));
		CHECK(fixture.GI.GetBindings() != nullptr);
		CHECK(fixture.GI.GetConstants().Origin.w == 0.0f);
		if (restriction == 4)
			CHECK(logs.Contains(spdlog::level::err, "ProbeTraceTriangles"));
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
	Entity extraVolume = scene.CreateEntity("Extra GI");
	extraVolume.AddComponent<GIVolumeComponent>();
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
		extraVolume.GetComponent<GIVolumeComponent>().Enabled = false;
		renderer.Render(scene, camera, framebuffer);
		extraVolume.GetComponent<GIVolumeComponent>().Enabled = true;
		renderer.Render(scene, camera, framebuffer);
		renderer.GetSettings().GIEnabled = false;
		renderer.Render(scene, camera, framebuffer);
		CHECK(fallback.GetPixels() == ReadbackTexture(graphics.GetDevice(), output).GetPixels());
	}
	uint32_t failures = 0, volumeWarnings = 0;
	for (const auto& entry : logs.GetEntries())
	{
		if (entry.Level == spdlog::level::err && entry.Message.find("Probe GI update failed") != std::string::npos)
			++failures;
		if (entry.Level == spdlog::level::warn && entry.Message.find("Only one GI volume") != std::string::npos)
			++volumeWarnings;
	}
	CHECK(failures == 0);
	CHECK(logs.Count(spdlog::level::warn, "Probe GI is unavailable") == 1);
	CHECK(volumeWarnings == 2);
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

TEST_CASE("ProbeGI: scene replacement at the same address resets all history even with identical geometry")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	std::optional<Scene> scene(std::in_place);
	Entity cube = AddCube(*scene, {100.0f, 0.0f, 0.0f}, glm::vec3(1.0f));
	const UUID cubeID = cube.GetUUID();
	const auto transform = cube.GetTransform();
	const auto mesh = cube.GetComponent<MeshComponent>();
	ProbeFixture fixture(graphics.GetDevice());
	fixture.Specification.ProbesPerFrame = 1;
	fixture.Specification.Hysteresis = 0.99f;
	REQUIRE(fixture.Update(*scene, assets));
	CHECK(ReadProbe(fixture.Device, fixture.GI.GetIrradiance(), 7)[27].b > 0.0f);
	Scene* address = &*scene;
	scene.reset();
	scene.emplace();
	REQUIRE(&*scene == address);
	cube = scene->CreateEntityWithUUID(cubeID);
	cube.GetTransform() = transform;
	cube.AddComponent<MeshComponent>(mesh);
	fixture.ViewData.EnvironmentParams.x = 0.0f;
	REQUIRE(fixture.Update(*scene, assets));
	CHECK(fixture.GI.GetConstants().Update.w == 0);
	CHECK(fixture.GI.GetConstants().Update.y == 8);
	CHECK(ReadProbe(fixture.Device, fixture.GI.GetIrradiance(), 7)[27].b == 0.0f);
	scene.reset();
	scene.emplace();
	Enclose(*scene);
	fixture.ViewData.EnvironmentParams.x = 1.0f;
	REQUIRE(fixture.Update(*scene, assets));
	CHECK(fixture.GI.GetConstants().Update.w == 0);
	CHECK(ReadProbe(fixture.Device, fixture.GI.GetIrradiance(), 7)[27].b == 0.0f);
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
				CHECK_FALSE(Reflection::SetField(field, &settings, -2, error));
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
	CHECK(logs.Contains(spdlog::level::err, "texture mip data"));
	auto masked = CreateRef<MaterialAsset>();
	masked->Handle = AssetHandle(60120);
	masked->BaseColorTexture = texture->Handle;
	masked->Alpha = AlphaMode::Mask;
	assets.AddMemoryAsset(masked);
	AddCube(scene, {2, 0, 0}, glm::vec3(1), masked->Handle);
	REQUIRE(trace.Update(scene, assets, changed));
	REQUIRE(trace.GetMaterials().size() == 2);
	for (const auto& entry : trace.GetMaterials())
		CHECK(entry.Textures[0].y == 1024);
	CHECK(trace.GetTexels().size() == 1024 * 512 + 1);
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
		fixture.Specification.Spacing = 2.0f;
		fixture.Specification.Origin = glm::vec3(-1.0f);
		REQUIRE(fixture.Update(scene, assets));
		for (const auto& pixel : ReadProbe(graphics.GetDevice(), fixture.GI.GetIrradiance()))
		{
			CHECK(pixel.r == doctest::Approx(0.2).epsilon(0.001));
			CHECK(pixel.g == doctest::Approx(0.4).epsilon(0.001));
			CHECK(pixel.b == doctest::Approx(0.8).epsilon(0.001));
		}
	}
	Enclose(scene);
	{
		ProbeFixture fixture(graphics.GetDevice());
		fixture.Specification.Spacing = 2.0f;
		fixture.Specification.Origin = glm::vec3(-1.0f);
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
		renderer.GetSettings().DebugView = RenderDebugView::GICoverage;
		renderer.Render(scene, camera, framebuffer);
		const auto covered = ReadbackTexture(graphics.GetDevice(), output).GetPixel(32, 32);
		CHECK(covered.r == 255);
		CHECK(covered.g == 255);
		CHECK(covered.b == 255);
		for (auto handle : scene.GetAllEntitiesWith<GIVolumeComponent>())
			Entity(handle, &scene).GetTransform().Translation = glm::vec3(100);
		renderer.Render(scene, camera, framebuffer);
		const auto outside = ReadbackTexture(graphics.GetDevice(), output).GetPixel(32, 32);
		CHECK(outside.r == 0);
		CHECK(outside.g == 0);
		CHECK(outside.b == 0);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: occluded neighbours do not dim reliable visible irradiance")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	ProbeFixture fixture(graphics.GetDevice());
	fixture.Specification.NormalBias = fixture.Specification.ViewBias = 0.0f;
	REQUIRE(fixture.Update(scene, assets));
	for (bool wall : {false, true})
	{
		fixture.Commands->open();
		for (uint32_t layer = 0; layer < 8; ++layer)
		{
			const float mean = wall && (layer & 2) == 0 ? 0.1f : 3.0f;
			std::array<glm::vec4, 64> irradiance, distance;
			irradiance.fill(glm::vec4(1.0f));
			distance.fill(glm::vec4(mean, mean * mean, 0, 1));
			fixture.Commands->writeTexture(fixture.GI.GetIrradiance(), layer, 0, irradiance.data(), 8 * sizeof(glm::vec4));
			WriteUniformDistance(fixture.Commands, fixture.GI.GetDistance(), layer, distance[0]);
		}
		fixture.Commands->close();
		fixture.Device->executeCommandList(fixture.Commands);
		const auto query = fixture.GI.Inspect({}, {0, 1, 0}, {0, 0, 1});
		REQUIRE(query);
		CHECK(query->Irradiance.r == doctest::Approx(1.0).epsilon(0.001));
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: closed double-sided solids reject embedded probes independently of shading sidedness")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	bool castShadows = true;
	SUBCASE("shadow-casting solid") {}
	SUBCASE("non-shadowing solid")
	{
		castShadows = false;
	}
	for (bool doubleSided : {false, true})
	{
		AssetManager assets;
		auto material = CreateRef<MaterialAsset>();
		material->Handle = AssetHandle(61234);
		material->DoubleSided = doubleSided;
		assets.AddMemoryAsset(material);
		Scene scene;
		AddCube(scene, {}, glm::vec3(2.0f), material->Handle).GetComponent<MeshComponent>().CastShadows = castShadows;
		ProbeFixture fixture(graphics.GetDevice());
		REQUIRE(fixture.Update(scene, assets));
		for (uint32_t probe = 0; probe < 8; ++probe)
		{
			const auto moments = ReadProbe(fixture.Device, fixture.GI.GetDistance(), probe);
			CHECK(moments[9].w == 0.0f);
		}
		const auto query = fixture.GI.Inspect({}, {0, 1, 0}, {0, 0, 1});
		REQUIRE(query);
		CHECK(query->Summary.y == 0.0f);
		CHECK(glm::length(glm::vec3(query->Irradiance)) == 0.0f);
		fixture.Specification.Origin = {1.1f, -0.5f, -0.5f};
		REQUIRE(fixture.Update(scene, assets));
		for (uint32_t probe = 0; probe < 8; ++probe)
			CHECK(ReadProbe(fixture.Device, fixture.GI.GetDistance(), probe)[9].w == 1.0f);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: small double-sided sheets shade both sides without invalidating surrounding probes")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	auto material = CreateRef<MaterialAsset>();
	material->Handle = AssetHandle(61235);
	material->DoubleSided = true;
	material->EmissiveFactor = {2, 0, 0};
	bool sunlight = false;
	SUBCASE("emission on both sides") {}
	SUBCASE("back face lit from below")
	{
		sunlight = true;
		material->EmissiveFactor = glm::vec3(0.0f);
		material->BaseColorFactor = glm::vec4(1.0f);
		material->MetallicFactor = 0.0f;
	}
	assets.AddMemoryAsset(material);
	Scene scene;
	Entity sheet = scene.CreateEntity("Two sided sheet");
	sheet.GetTransform().Scale = glm::vec3(0.1f);
	auto& mesh = sheet.AddComponent<MeshComponent>();
	mesh.Mesh = AssetHandle(AssetManager::BuiltinPlane);
	mesh.Materials = {material->Handle};
	ProbeFixture fixture(graphics.GetDevice());
	fixture.Specification.RaysPerProbe = 256;
	fixture.Specification.BounceFeedback = false;
	fixture.ViewData.EnvironmentParams.x = 0;
	if (sunlight)
	{
		fixture.ViewData.SunDirection = {0, 1, 0, 1};
		fixture.ViewData.SunColor = {6, 0, 0, 1};
	}
	REQUIRE(fixture.Update(scene, assets));
	for (uint32_t probe = 0; probe < 8; ++probe)
	{
		const auto irradiance = ReadProbe(fixture.Device, fixture.GI.GetIrradiance(), probe);
		float red = 0;
		for (const auto& pixel : irradiance)
		{
			red += pixel.r;
			CHECK(pixel.a == 1.0f);
			CHECK(pixel.g == 0.0f);
		}
		// Grid Y is bit 1. Only the underside faces this sun: emission alone would not test either normal flip.
		if (sunlight && (probe & 2u))
			CHECK(red == 0.0f);
		else
			CHECK(red > 0.1f);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: asymmetric directional irradiance preserves world axes and octahedral seam continuity")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	ProbeFixture fixture(graphics.GetDevice());
	fixture.Specification.RaysPerProbe = 256;
	fixture.Specification.NormalBias = fixture.Specification.ViewBias = 0;
	auto desc = fixture.Sky->getDesc();
	desc.width = desc.height = 32;
	fixture.Sky = fixture.Device->createTexture(desc);
	REQUIRE(fixture.Sky);
	fixture.Commands->open();
	// Analytic first-order radiance: its cosine convolution is 0.5 + normal/6 in RGB.
	for (uint32_t face = 0; face < 6; ++face)
	{
		std::array<glm::u8vec4, 32 * 32> texels;
		for (uint32_t y = 0; y < 32; ++y)
			for (uint32_t x = 0; x < 32; ++x)
			{
				const float u = (static_cast<float>(x) + 0.5f) / 16.0f - 1.0f;
				const float v = (static_cast<float>(y) + 0.5f) / 16.0f - 1.0f;
				const std::array<glm::vec3, 6> faces{{{1, -v, -u}, {-1, -v, u}, {u, 1, v}, {u, -1, -v}, {u, -v, 1}, {-u, -v, -1}}};
				const glm::vec3 color = glm::vec3(0.5f) + 0.25f * glm::normalize(faces[face]);
				texels[y * 32 + x] = glm::u8vec4(glm::round(color * 255.0f), 255);
			}
		fixture.Commands->writeTexture(fixture.Sky, face, 0, texels.data(), 32 * sizeof(glm::u8vec4));
	}
	fixture.Commands->close();
	fixture.Device->executeCommandList(fixture.Commands);
	REQUIRE(fixture.Update(scene, assets));
	const auto tile = ReadProbe(fixture.Device, fixture.GI.GetIrradiance());
	for (uint32_t i = 1; i <= 6; ++i)
	{
		CHECK(tile[i * 8] == tile[(7 - i) * 8 + 1]);
		CHECK(tile[i * 8 + 7] == tile[(7 - i) * 8 + 6]);
		CHECK(tile[i] == tile[8 + 7 - i]);
		CHECK(tile[56 + i] == tile[48 + 7 - i]);
	}
	CHECK(tile[0] == tile[54]);
	CHECK(tile[7] == tile[49]);
	CHECK(tile[56] == tile[14]);
	CHECK(tile[63] == tile[9]);
	for (glm::vec3 axis : {glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)})
		for (float sign : {-1.0f, 1.0f})
		{
			const glm::vec3 normal = axis * sign;
			const auto query = fixture.GI.Inspect({}, normal, {0, 0, 1});
			REQUIRE(query);
			const glm::vec3 expected = glm::vec3(0.5f) + normal / 6.0f;
			CHECK(glm::length(glm::vec3(query->Irradiance) - expected) < 0.035f);
		}
	for (glm::vec3 direction : {glm::vec3(0, 0, -1), glm::vec3(0, 1, -1), glm::vec3(1, 0, -1), glm::vec3(1, 1, 0), glm::vec3(-1, 1, 0)})
		for (glm::vec3 epsilon : {glm::vec3(1e-5f, 0, 0), glm::vec3(0, 1e-5f, 0), glm::vec3(0, 0, 1e-5f)})
		{
			const auto left = fixture.GI.Inspect({}, direction - epsilon, {0, 0, 1});
			const auto right = fixture.GI.Inspect({}, direction + epsilon, {0, 0, 1});
			REQUIRE(left);
			REQUIRE(right);
			CHECK(glm::length(glm::vec3(left->Irradiance - right->Irradiance)) < 0.001f);
		}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: mip filtering preserves sub-ray sky energy and bounds temporal variation")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	ProbeFixture fixture(graphics.GetDevice());
	fixture.Specification.NormalBias = fixture.Specification.ViewBias = 0.0f;
	// A bright texel smaller than one ray's angular footprint must contribute even in updates
	// whose ray centres miss it. Build a box-filtered mip chain independently of the renderer.
	constexpr uint32_t Size = 32, BrightX = 18, BrightY = 16;
	constexpr float Radiance = 256.0f;
	auto desc = fixture.Sky->getDesc();
	desc.width = desc.height = Size;
	desc.mipLevels = 6;
	desc.format = nvrhi::Format::RGBA16_FLOAT;
	fixture.Sky = fixture.Device->createTexture(desc);
	REQUIRE(fixture.Sky);
	fixture.Commands->open();
	for (uint32_t face = 0; face < 6; ++face)
	{
		std::vector<glm::vec4> texels(Size * Size, glm::vec4(0.0f));
		if (face == 2)
			texels[BrightY * Size + BrightX] = glm::vec4(Radiance);
		uint32_t size = Size;
		for (uint32_t mip = 0; mip < desc.mipLevels; ++mip)
		{
			std::vector<glm::u16vec4> encoded(texels.size());
			for (size_t i = 0; i < texels.size(); ++i)
				for (int channel = 0; channel < 4; ++channel)
					encoded[i][channel] = glm::packHalf1x16(texels[i][channel]);
			fixture.Commands->writeTexture(fixture.Sky, face, mip, encoded.data(), size * sizeof(glm::u16vec4));
			if (size == 1)
				break;
			const uint32_t nextSize = size / 2;
			std::vector<glm::vec4> next(nextSize * nextSize);
			for (uint32_t y = 0; y < nextSize; ++y)
				for (uint32_t x = 0; x < nextSize; ++x)
					next[y * nextSize + x] = (texels[2 * y * size + 2 * x] + texels[2 * y * size + 2 * x + 1] +
											  texels[(2 * y + 1) * size + 2 * x] + texels[(2 * y + 1) * size + 2 * x + 1]) *
											 0.25f;
			texels = std::move(next);
			size = nextSize;
		}
	}
	fixture.Commands->close();
	fixture.Device->executeCommandList(fixture.Commands);
	const auto solidAngleIntegral = [](float u, float v) { return std::atan2(u * v, std::sqrt(u * u + v * v + 1.0f)); };
	const float u0 = 2.0f * BrightX / Size - 1.0f, u1 = 2.0f * (BrightX + 1) / Size - 1.0f;
	const float v0 = 2.0f * BrightY / Size - 1.0f, v1 = 2.0f * (BrightY + 1) / Size - 1.0f;
	const float solidAngle = solidAngleIntegral(u1, v1) - solidAngleIntegral(u0, v1) - solidAngleIntegral(u1, v0) + solidAngleIntegral(u0, v0);
	const float cosine = glm::normalize(glm::vec3((u0 + u1) * 0.5f, 1.0f, (v0 + v1) * 0.5f)).y;
	const float expected = Radiance * solidAngle * cosine / 3.14159265358979323846f;
	float minimum = std::numeric_limits<float>::max(), maximum = 0.0f, sum = 0.0f;
	for (uint32_t frame = 0; frame < 8; ++frame)
	{
		REQUIRE(fixture.Update(scene, assets));
		const auto query = fixture.GI.Inspect({}, {0, 1, 0}, {0, 0, 1});
		REQUIRE(query);
		const float value = query->Irradiance.r;
		CAPTURE(frame);
		CAPTURE(value);
		CAPTURE(expected);
		CHECK(std::isfinite(value));
		CHECK(value > expected * 0.5f);
		CHECK(value < expected * 1.5f);
		minimum = std::min(minimum, value);
		maximum = std::max(maximum, value);
		sum += value;
	}
	CAPTURE(minimum);
	CAPTURE(maximum);
	CAPTURE(sum / 8.0f);
	CAPTURE(expected);
	// Finite ray quadrature and the broad mip footprint introduce integration error, but neither
	// rare spikes nor a consistently dark result meet this energy and stability bound.
	CHECK(std::abs(sum / 8.0f - expected) < expected * 0.3f);
	// With only 64 rays, even footprint-filtered quadrature has variance. The normal temporal
	// filter must suppress it while keeping the integral, rather than averaging rare huge spikes.
	fixture.Specification.Hysteresis = 0.85f;
	for (uint32_t frame = 0; frame < 32; ++frame)
	{
		REQUIRE(fixture.Update(scene, assets));
		fixture.Device->waitForIdle();
		fixture.Device->runGarbageCollection();
	}
	minimum = std::numeric_limits<float>::max();
	maximum = 0.0f;
	for (uint32_t frame = 0; frame < 8; ++frame)
	{
		REQUIRE(fixture.Update(scene, assets));
		const auto query = fixture.GI.Inspect({}, {0, 1, 0}, {0, 0, 1});
		REQUIRE(query);
		minimum = std::min(minimum, query->Irradiance.r);
		maximum = std::max(maximum, query->Irradiance.r);
	}
	CHECK(minimum > expected * 0.7f);
	CHECK(maximum < expected * 1.3f);
	CHECK(maximum - minimum < expected * 0.1f);
	CHECK(graphics.GetValidationErrorCount() == errors);
}

static void CheckFurnace(bool visibility)
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	auto material = CreateRef<MaterialAsset>();
	material->Handle = AssetHandle(60120);
	material->BaseColorFactor = glm::vec4(glm::vec3(0.5f), 1.0f);
	material->MetallicFactor = 0.0f;
	material->EmissiveFactor = {0.5f, 1.0f, 2.0f};
	SUBCASE("single-sided walls")
	{
		material->DoubleSided = false;
	}
	SUBCASE("double-sided walls")
	{
		material->DoubleSided = true;
	}
	assets.AddMemoryAsset(material);
	Enclose(scene, material->Handle);
	ProbeFixture fixture(graphics.GetDevice());
	fixture.ViewData.EnvironmentParams.x = 0.0f;
	// Wall hit points must lie within the volume for bounce feedback. The centre probe is
	// in air; the other probes lie inside the closed walls and classification rejects them.
	fixture.Specification.Origin = glm::vec3(-2.0f);
	fixture.Specification.Spacing = 2.0f;
	fixture.Specification.Counts = glm::uvec3(3);
	fixture.Specification.ProbesPerFrame = 27;
	fixture.Specification.Hysteresis = 0.5f;
	// Isolate estimator/feedback energy from the approximate distance-moment visibility filter.
	// All rays from the central probe to these interior wall receivers are physically visible.
	fixture.Specification.VisibilityWeighting = visibility;
	fixture.Specification.BounceFeedback = false;
	for (uint32_t frame = 0; frame < 4; ++frame)
	{
		REQUIRE(fixture.Update(scene, assets));
		fixture.Device->waitForIdle();
		fixture.Device->runGarbageCollection();
	}
	for (const auto& pixel : ReadProbe(fixture.Device, fixture.GI.GetIrradiance(), 13))
		for (int channel = 0; channel < 3; ++channel)
			CHECK(pixel[channel] == doctest::Approx(static_cast<double>(material->EmissiveFactor[channel])).epsilon(0.001));
	for (uint32_t probe = 0; probe < 27; ++probe)
		CHECK(ReadProbe(fixture.Device, fixture.GI.GetDistance(), probe)[0].w == (probe == 13 ? 1.0f : 0.0f));
	fixture.Specification.BounceFeedback = true;
	for (uint32_t frame = 0; frame < 64; ++frame)
	{
		REQUIRE(fixture.Update(scene, assets));
		fixture.Device->waitForIdle();
		fixture.Device->runGarbageCollection();
		if (frame < 2 && !visibility)
		{
			const double factor = frame == 0 ? 1.24 : 1.4176;
			for (const auto& pixel : ReadProbe(fixture.Device, fixture.GI.GetIrradiance(), 13))
				for (int channel = 0; channel < 3; ++channel)
					CHECK(pixel[channel] == doctest::Approx(static_cast<double>(material->EmissiveFactor[channel]) * factor).epsilon(0.02));
		}
	}
	// Uniform emission and reflectance admit constant radiance everywhere in the enclosure:
	// L = Le + rho*L. The tracer retains 96% of dielectric base colour for diffuse reflection.
	const glm::vec3 expected = material->EmissiveFactor / (1.0f - 0.5f * 0.96f);
	for (const auto& pixel : ReadProbe(fixture.Device, fixture.GI.GetIrradiance(), 13))
	{
		CHECK(pixel.a == 1.0f);
		for (int channel = 0; channel < 3; ++channel)
			CHECK(pixel[channel] == doctest::Approx(static_cast<double>(expected[channel])).epsilon(0.02));
	}
	const auto query = fixture.GI.Inspect({1.94f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f});
	REQUIRE(query);
	for (const auto& neighbour : query->Neighbours)
		if (neighbour.Metadata.y != 0)
		{
			CHECK(neighbour.SurfaceRay.Metadata.z == 0);
			CHECK(neighbour.BiasedRay.Metadata.z == 0);
		}
	for (int channel = 0; channel < 3; ++channel)
	{
		CHECK(query->SurfaceExact[channel] == doctest::Approx(static_cast<double>(expected[channel])).epsilon(0.02));
		CHECK(query->BiasedExact[channel] == doctest::Approx(static_cast<double>(expected[channel])).epsilon(0.02));
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: a uniform emissive furnace converges to its analytic diffuse bounce series")
{
	CheckFurnace(false);
}

TEST_CASE("ProbeGI: distance visibility preserves analytic furnace energy after convergence")
{
	CheckFurnace(true);
}

TEST_CASE("ProbeGI: view bias scales with spacing and stops before a near view origin")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	ProbeFixture fixture(graphics.GetDevice());
	fixture.Specification.Origin = glm::vec3(-1.0f);
	fixture.Specification.Spacing = 2.0f;
	fixture.Specification.NormalBias = 0.0f;
	REQUIRE(fixture.Update(scene, assets));
	CHECK(fixture.GI.GetConstants().Tuning.y == doctest::Approx(0.4).epsilon(0.001));
	for (float viewDistance : {0.0f, 0.06f, 1000000.0f})
	{
		const auto query = fixture.GI.Inspect({}, {0, 1, 0}, {1, 0, 0}, viewDistance);
		REQUIRE(query);
		CHECK(query->Neighbours[0].BiasedRay.OriginLength.x == doctest::Approx(static_cast<double>(std::min(0.4f, viewDistance * 0.5f))).epsilon(0.001));
	}
	CHECK_FALSE(fixture.GI.Inspect({}, {0, 1, 0}, {1, 0, 0}, -1.0f));
	CHECK_FALSE(fixture.GI.Inspect({}, {0, 1, 0}, {1, 0, 0}, std::numeric_limits<float>::quiet_NaN()));
	CHECK_FALSE(fixture.GI.Inspect({}, {0, 1, 0}, {1, 0, 0}, std::numeric_limits<float>::infinity()));
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: converged oblique wall corner rejects exterior probes across tested scales")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	float spacing = 1.75f, wallThickness = 0.1f;
	bool nearProbe = false;
	const glm::vec3 view(0.8f, 0.25f, -0.6f);
	SUBCASE("reference scale") {}
	SUBCASE("half-metre spacing")
	{
		spacing = 0.5f;
	}
	SUBCASE("four-metre spacing")
	{
		spacing = 4.0f;
	}
	SUBCASE("thicker wall and near interior probe")
	{
		wallThickness = 0.3f;
		nearProbe = true;
	}
	const float worldScale = spacing / 1.75f;
	AssetManager assets;
	Scene scene;
	auto material = CreateRef<MaterialAsset>();
	material->Handle = AssetHandle(60121);
	material->BaseColorFactor = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
	material->MetallicFactor = 0.0f;
	material->EmissiveFactor = glm::vec3(0.005f);
	assets.AddMemoryAsset(material);
	const glm::vec3 low = glm::vec3(6.4f, 0.0f, -2.0f) * worldScale, high = glm::vec3(9.5f, 3.0f, 4.0f) * worldScale;
	wallThickness *= worldScale;
	for (int axis = 0; axis < 3; ++axis)
		for (int side : {0, 1})
		{
			glm::vec3 position = (low + high) * 0.5f;
			glm::vec3 scale = high - low + 2.0f * wallThickness;
			position[axis] = side == 0 ? low[axis] - wallThickness * 0.5f : high[axis] + wallThickness * 0.5f;
			scale[axis] = wallThickness;
			AddCube(scene, position, scale, material->Handle);
		}
	ProbeFixture fixture(graphics.GetDevice());
	fixture.Specification.Origin = glm::vec3(5.25f, -0.5f, 2.0f) * worldScale;
	if (nearProbe)
		fixture.Specification.Origin.z = high.z - 1.1f * spacing;
	fixture.Specification.Counts = {2, 2, 3};
	fixture.Specification.ProbesPerFrame = 12;
	fixture.Specification.Spacing = spacing;
	fixture.Specification.ViewBias = 0.2f;
	fixture.Specification.RaysPerProbe = 128;
	fixture.Specification.Hysteresis = 0.8f;
	for (int frame = 0; frame < 64; ++frame)
	{
		REQUIRE(fixture.Update(scene, assets));
		fixture.Device->waitForIdle();
		fixture.Device->runGarbageCollection();
	}
	const auto query = fixture.GI.Inspect(glm::vec3(6.4f, 0.35f, 3.994f) * worldScale, {1, 0, 0}, view);
	REQUIRE(query);
	CHECK(query->SurfaceExact.g > 0.005f);
	CAPTURE(spacing);
	CAPTURE(wallThickness);
	CAPTURE(nearProbe);
	CAPTURE(query->Irradiance.g);
	CAPTURE(query->SurfaceExact.g);

	CHECK(std::abs(query->Irradiance.g - query->SurfaceExact.g) < 0.1f * query->SurfaceExact.g);
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: fine distance tiles retain sample mass and report cold sampling confidence")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	ProbeFixture fixture(graphics.GetDevice());
	fixture.Specification.Origin = glm::vec3(-1.0f);
	fixture.Specification.Counts = glm::uvec3(3);
	fixture.Specification.ProbesPerFrame = 27;
	fixture.Specification.RaysPerProbe = 32;
	fixture.Specification.NormalBias = fixture.Specification.ViewBias = 0.0f;
	REQUIRE(fixture.Update(scene, assets));
	CHECK(fixture.GI.GetIrradiance()->getDesc().width == 8);
	CHECK(fixture.GI.GetDistance()->getDesc().width == GPU::ProbeDistanceTileSize);
	const auto pixels = ReadProbe(fixture.Device, fixture.GI.GetDistance(), 13);
	float minimumMass = 1.0f, maximumMass = 0.0f;
	for (const auto& value : pixels)
	{
		REQUIRE(value.z > 0.0f);
		CHECK(value.x / value.z == doctest::Approx(3.0).epsilon(0.001));
		CHECK(value.y / value.z == doctest::Approx(9.0).epsilon(0.001));
		CHECK(value.w == 1.0f);
		minimumMass = std::min(minimumMass, value.z);
		maximumMass = std::max(maximumMass, value.z);
	}
	CHECK(minimumMass < maximumMass * 0.01f);
	float minimumConfidence = 1.0f;
	for (const glm::vec3 direction : {glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1), glm::vec3(1, 1, 1)})
	{
		const auto query = fixture.GI.Inspect(glm::normalize(direction) * 0.25f, {0, 1, 0}, {0, 0, 1});
		REQUIRE(query);
		for (const auto& neighbour : query->Neighbours)
			minimumConfidence = std::min(minimumConfidence, neighbour.Sampling.z);
		CHECK(query->Irradiance.g == doctest::Approx(0.4).epsilon(0.001));
	}
	CHECK(minimumConfidence < 0.1f);
	fixture.Commands->open();
	const std::vector<glm::vec4> missing(pixels.size(), glm::vec4(0, 0, 0, 1));
	for (uint32_t layer = 0; layer < 27; ++layer)
		fixture.Commands->writeTexture(fixture.GI.GetDistance(), layer, 0, missing.data(), GPU::ProbeDistanceTileSize * sizeof(glm::vec4));
	fixture.Commands->close();
	fixture.Device->executeCommandList(fixture.Commands);
	const auto missingQuery = fixture.GI.Inspect({}, {0, 1, 0}, {0, 0, 1});
	REQUIRE(missingQuery);
	CHECK(missingQuery->Summary.y == 0.0f);
	CHECK(missingQuery->Irradiance.g == 0.0f);
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: inspection allocation failures preserve history and recover on retry")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	FailingProbeResources factory;
	ProbeFixture fixture(graphics.GetDevice(), &factory);
	REQUIRE(fixture.Update(scene, assets));
	const auto before = ReadProbe(fixture.Device, fixture.GI.GetIrradiance());
	const uint32_t updates = fixture.GI.GetUpdateCount();
	for (auto kind : {FailingProbeResources::Kind::Buffer, FailingProbeResources::Kind::Bindings})
		for (int index = 0;; ++index)
		{
			factory.FailureKind = kind;
			factory.Remaining = index;
			REQUIRE(index < 32);
			factory.Failed = false;
			const auto result = fixture.GI.Inspect({}, {0, 1, 0}, {0, 0, 1});
			if (!factory.Failed)
			{
				REQUIRE(result);
				CHECK(index == (kind == FailingProbeResources::Kind::Buffer ? 3 : 1));
				break;
			}
			CHECK_FALSE(result);
			CHECK(fixture.GI.HasHistory());
			CHECK(fixture.GI.GetUpdateCount() == updates);
			CHECK(ReadProbe(fixture.Device, fixture.GI.GetIrradiance()) == before);
			factory.Remaining = -1;
			REQUIRE(fixture.GI.Inspect({}, {0, 1, 0}, {0, 0, 1}));
			CHECK(fixture.GI.GetUpdateCount() == updates);
		}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: sun-lit tilted surfaces retain analytic energy far from the world origin")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	float translation = 0.0f;
	bool blocked = false;
	SUBCASE("near origin") {}
	SUBCASE("positive large translation")
	{
		translation = 100000.0f;
	}
	SUBCASE("negative large translation")
	{
		translation = -100000.0f;
	}
	SUBCASE("sun blocker at large translation")
	{
		translation = 100000.0f;
		blocked = true;
	}
	AssetManager assets;
	Scene scene;
	auto material = CreateRef<MaterialAsset>();
	material->Handle = AssetHandle(60130);
	material->BaseColorFactor = glm::vec4(1.0f);
	material->MetallicFactor = 0.0f;
	assets.AddMemoryAsset(material);
	const glm::quat rotation = glm::angleAxis(0.61f, glm::normalize(glm::vec3(1, 0, 1)));
	const glm::vec3 normal = rotation * glm::vec3(0, 1, 0);
	const glm::vec3 centre(translation);
	auto floor = AddCube(scene, centre - normal * 0.1f, {100, 0.2f, 100}, material->Handle);
	floor.GetTransform().Rotation = rotation;
	if (blocked)
	{
		auto ceiling = AddCube(scene, centre + normal * 10.0f, {110, 0.2f, 110}, material->Handle);
		ceiling.GetTransform().Rotation = rotation;
	}
	ProbeFixture fixture(graphics.GetDevice());
	fixture.Specification.Origin = centre + normal * 2.0f - glm::vec3(0.5f);
	fixture.Specification.RaysPerProbe = 128;
	fixture.Specification.BounceFeedback = false;
	fixture.ViewData.EnvironmentParams.x = 0.0f;
	fixture.ViewData.SunDirection = glm::vec4(-normal, 1.0f);
	fixture.ViewData.SunColor = glm::vec4(3.14159265358979323846f / 0.96f);
	REQUIRE(fixture.Update(scene, assets));
	const auto pixels = ReadProbe(fixture.Device, fixture.GI.GetIrradiance());
	const glm::vec3 probe = fixture.Specification.Origin;
	for (uint32_t y = 1; y < 7; ++y)
		for (uint32_t x = 1; x < 7; ++x)
		{
			const glm::vec2 oct = (glm::vec2(x, y) - 0.5f) / 3.0f - 1.0f;
			glm::vec3 direction(oct, 1.0f - std::abs(oct.x) - std::abs(oct.y));
			const float fold = glm::clamp(-direction.z, 0.0f, 1.0f);
			direction.x += direction.x >= 0.0f ? -fold : fold;
			direction.y += direction.y >= 0.0f ? -fold : fold;
			direction = glm::normalize(direction);
			float sum = 0.0f, weight = 0.0f;
			for (uint32_t ray = 0; ray < 128; ++ray)
			{
				const float vertical = 1.0f - 2.0f * (static_cast<float>(ray) + 0.5f) / 128.0f;
				const float radius = std::sqrt(1.0f - vertical * vertical);
				const float angle = 2.399963229728653f * static_cast<float>(ray);
				const glm::vec3 rayDirection(std::cos(angle) * radius, vertical, std::sin(angle) * radius);
				const float cosine = std::max(glm::dot(direction, rayDirection), 0.0f);
				weight += cosine;
				const float denominator = glm::dot(normal, rayDirection);
				if (blocked || denominator >= 0.0f)
					continue;
				const float distance = glm::dot(centre - probe, normal) / denominator;
				const glm::vec3 local = glm::conjugate(rotation) * (probe - centre + rayDirection * distance);
				if (std::abs(local.x) < 50.0f && std::abs(local.z) < 50.0f)
					sum += cosine;
			}
			CAPTURE(translation);
			CAPTURE(blocked);
			CAPTURE(x);
			CAPTURE(y);
			CHECK(pixels[y * 8 + x].r == doctest::Approx(static_cast<double>(sum / weight)).epsilon(0.015).scale(1.0));
		}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: large translations retain distinct walls one representable step from a receiver")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	// At 100000 the float32 step is 0.0078125. The near face is exactly one step away.
	AddCube(scene, {100000.015625f, 0, 0}, {0.015625f, 4, 4});
	ProbeFixture fixture(graphics.GetDevice());
	fixture.Specification.Origin.x += 100000.0f;
	fixture.Specification.NormalBias = fixture.Specification.ViewBias = 0.0f;
	REQUIRE(fixture.Update(scene, assets));
	const auto query = fixture.GI.Inspect({100000.0f, 0.001f, 0}, {0, 1, 0}, {0, 1, 0});
	REQUIRE(query);
	for (uint32_t corner = 0; corner < 8; ++corner)
	{
		const auto& ray = query->Neighbours[corner].SurfaceRay;
		CHECK((ray.Metadata.w & 2) == 0);
		CHECK(ray.Metadata.z == (corner % 2));
		if (corner % 2)
			CHECK(ray.HitPositionDistance.x == 100000.0078125f);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: secondary rays retain close parallel and near-light blockers at large translations")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	bool pointLight = false;
	float translation = 100000.0f;
	SUBCASE("parallel blocker four representable steps above the source") {}
	SUBCASE("blocker two representable steps before a point light")
	{
		pointLight = true;
	}
	SUBCASE("same point-light blocker near the origin")
	{
		pointLight = true;
		translation = 0.0f;
	}
	AssetManager assets;
	Scene scene;
	auto material = CreateRef<MaterialAsset>();
	material->Handle = AssetHandle(60131);
	material->BaseColorFactor = glm::vec4(1.0f);
	material->MetallicFactor = 0.0f;
	assets.AddMemoryAsset(material);
	AddCube(scene, {0, translation - 0.0078125f, 0}, {100, 0.015625f, 100}, material->Handle);
	// The source at Y=translation and blocker faces are exactly representable at either translation.
	const float blockerCentre = translation + (pointLight ? 2.0f - 0.0234375f : 0.0390625f);
	auto blocker = AddCube(scene, {0, blockerCentre, 0}, {100, 0.015625f, 100}, material->Handle);
	ProbeFixture fixture(graphics.GetDevice());
	fixture.Specification.Origin = {0, translation + (pointLight ? 1.0f : 0.015625f), 0};
	fixture.Specification.RaysPerProbe = 128;
	fixture.Specification.BounceFeedback = false;
	fixture.ViewData.EnvironmentParams.x = 0.0f;
	if (pointLight)
	{
		GPU::Light light{};
		light.PositionRange = {0, translation + 2.0f, 0, 200};
		light.ColorIntensity = {10, 10, 10, 0};
		fixture.LightData = {light};
	}
	else
	{
		fixture.ViewData.SunDirection = {0, -1, 0, 1};
		fixture.ViewData.SunColor = glm::vec4(1.0f);
	}
	REQUIRE(fixture.Update(scene, assets));
	for (const auto& pixel : ReadProbe(fixture.Device, fixture.GI.GetIrradiance()))
		CHECK(glm::length(glm::vec3(pixel)) < 1e-6f);
	scene.DestroyEntity(blocker);
	REQUIRE(fixture.Update(scene, assets));
	float maximum = 0.0f;
	for (const auto& pixel : ReadProbe(fixture.Device, fixture.GI.GetIrradiance()))
		maximum = std::max(maximum, pixel.r);
	CHECK(maximum > 0.01f);
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeTraceScene: mesh shadow edits invalidate triangles without changing their geometry")
{
	AssetManager assets;
	Scene scene;
	auto cube = AddCube(scene, {}, glm::vec3(1.0f));
	ProbeTraceScene trace;
	bool changed = false;
	for (bool castShadows : {true, false, true})
	{
		cube.GetComponent<MeshComponent>().CastShadows = castShadows;
		REQUIRE(trace.Update(scene, assets, changed));
		CHECK(changed);
		REQUIRE(trace.GetTriangles().size() == 12);
		for (const auto& triangle : trace.GetTriangles())
			CHECK(triangle.Material.y == (castShadows ? 0 : 1));
		REQUIRE(trace.Update(scene, assets, changed));
		CHECK_FALSE(changed);
	}
}

TEST_CASE("ProbeGI: non-shadowing emissive proxies retain emission while releasing enclosed point lights")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	auto diffuse = CreateRef<MaterialAsset>();
	diffuse->Handle = AssetHandle(60132);
	diffuse->BaseColorFactor = glm::vec4(1.0f);
	diffuse->MetallicFactor = 0.0f;
	assets.AddMemoryAsset(diffuse);
	auto emission = CreateRef<MaterialAsset>();
	emission->Handle = AssetHandle(60133);
	emission->BaseColorFactor = {0, 0, 0, 1};
	emission->EmissiveFactor = {3, 0, 0};
	assets.AddMemoryAsset(emission);
	AddCube(scene, {0, -0.1f, 0}, {50, 0.2f, 50}, diffuse->Handle);
	auto proxy = AddCube(scene, {0, 2, 0}, glm::vec3(0.8f), emission->Handle);
	ProbeFixture fixture(graphics.GetDevice());
	fixture.Specification.Origin = {0, 1, 0};
	fixture.Specification.RaysPerProbe = 128;
	fixture.Specification.BounceFeedback = false;
	fixture.ViewData.EnvironmentParams.x = 0.0f;
	GPU::Light light{};
	light.PositionRange = {0, 2, 0, 100};
	light.ColorIntensity = {0, 30, 0, 0};
	fixture.LightData = {light};
	for (bool castShadows : {true, false, true})
	{
		CAPTURE(castShadows);
		proxy.GetComponent<MeshComponent>().CastShadows = castShadows;
		fixture.GI.ResetHistory();
		REQUIRE(fixture.Update(scene, assets));
		glm::vec3 maximum(0.0f);
		for (const auto& pixel : ReadProbe(fixture.Device, fixture.GI.GetIrradiance()))
			maximum = glm::max(maximum, glm::vec3(pixel));
		CHECK(maximum.r > 0.01f);
		if (castShadows)
			CHECK(maximum.g < 1e-6f);
		else
			CHECK(maximum.g > 0.01f);
		CHECK(maximum.b < 1e-6f);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeTraceScene: rejected causes ignore unrelated motion and asset edits then recover")
{
	AssetManager assets;
	Scene scene;
	auto mesh = MeshPrimitives::CreateCube();
	mesh->Handle = AssetHandle(60101);
	assets.AddMemoryAsset(mesh);
	auto bad = AddCube(scene, {}, glm::vec3(1));
	bad.GetComponent<MeshComponent>().Mesh = mesh->Handle;
	auto otherMaterial = CreateRef<MaterialAsset>();
	otherMaterial->Handle = AssetHandle(60102);
	assets.AddMemoryAsset(otherMaterial);
	auto unrelated = AddCube(scene, {3, 0, 0}, glm::vec3(1), otherMaterial->Handle);
	ProbeTraceScene trace;
	bool changed;
	REQUIRE(trace.Update(scene, assets, changed));
	const auto originalIndex = mesh->Indices[0];
	mesh->Indices[0] = std::numeric_limits<uint32_t>::max();
	++mesh->Version;
	Testing::ScopedLogCapture logs;
	REQUIRE_FALSE(trace.Update(scene, assets, changed));
	CHECK(logs.Count(spdlog::level::err, "vertex index outside mesh") == 1);
	for (int frame = 0; frame < 3; ++frame)
	{
		unrelated.GetTransform().Translation.x += 1;
		bad.GetComponent<MeshComponent>().Materials = {otherMaterial->Handle};
		otherMaterial->RoughnessFactor = 0.1f * static_cast<float>(frame);
		++otherMaterial->Version;
		CHECK_FALSE(trace.Update(scene, assets, changed));
		CHECK_FALSE(changed);
	}
	CHECK(logs.Count(spdlog::level::err, "Probe GI scene rejected") == 1);
	CHECK(trace.GetTriangles().size() == 24);
	SUBCASE("repair version releases the rejection")
	{
		mesh->Indices[0] = originalIndex;
		++mesh->Version;
		REQUIRE(trace.Update(scene, assets, changed));
		CHECK(changed);
		CHECK(trace.GetTriangles().size() == 24);
	}
	SUBCASE("removing the offending instance releases the rejection")
	{
		scene.DestroyEntity(bad);
		REQUIRE(trace.Update(scene, assets, changed));
		CHECK(trace.GetTriangles().size() == 12);
	}
}

TEST_CASE("ProbeTraceScene: a transformed coordinate rejection follows only the offending transform")
{
	AssetManager assets;
	Scene scene;
	auto bad = AddCube(scene, {}, glm::vec3(1));
	auto unrelated = AddCube(scene, {3, 0, 0}, glm::vec3(1));
	ProbeTraceScene trace;
	bool changed;
	REQUIRE(trace.Update(scene, assets, changed));
	bad.GetTransform().Translation.x = ProbeTraceScene::MaxCoordinate * 2;
	Testing::ScopedLogCapture logs;
	REQUIRE_FALSE(trace.Update(scene, assets, changed));
	unrelated.GetTransform().Translation.x = 4;
	CHECK_FALSE(trace.Update(scene, assets, changed));
	CHECK(logs.Count(spdlog::level::err, "transformed vertex") == 1);
	bad.GetTransform().Translation.x = 5;
	REQUIRE(trace.Update(scene, assets, changed));
	CHECK(changed);
	CHECK(trace.GetTriangles().size() == 24);
}

TEST_CASE("ProbeTraceScene: adapter budgets reject named buffers and retain the previous snapshot")
{
	AssetManager assets;
	Scene scene;
	bool changed;
	Testing::ScopedLogCapture logs;
	SUBCASE("triangle range is inclusive and zero scale releases a budget rejection")
	{
		GraphicsDeviceLimits limits;
		limits.MaxStorageBufferBytes = 12 * sizeof(GPU::ProbeTraceTriangle);
		ProbeTraceScene trace(limits);
		AddCube(scene, {}, glm::vec3(1));
		REQUIRE(trace.Update(scene, assets, changed));
		auto extra = AddCube(scene, {3, 0, 0}, glm::vec3(1));
		REQUIRE_FALSE(trace.Update(scene, assets, changed));
		CHECK_FALSE(changed);
		CHECK(trace.GetTriangles().size() == 12);
		extra.GetTransform().Translation.x = 4;
		CHECK_FALSE(trace.Update(scene, assets, changed));
		CHECK(logs.Count(spdlog::level::err, "ProbeTraceTriangles") == 1);
		extra.GetTransform().Scale = glm::vec3(0);
		REQUIRE(trace.Update(scene, assets, changed));
		CHECK(trace.GetTriangles().size() == 12);
	}
	SUBCASE("placeholder triangle must fit even in an empty scene")
	{
		GraphicsDeviceLimits limits;
		limits.MaxStorageBufferBytes = sizeof(GPU::ProbeTraceTriangle) - 1;
		ProbeTraceScene trace(limits);
		CHECK_FALSE(trace.Update(scene, assets, changed));
		CHECK_FALSE(trace.Update(scene, assets, changed));
		CHECK(logs.Count(spdlog::level::err, "ProbeTraceTriangles placeholder") == 1);
	}
	SUBCASE("material and texel limits are independent of triangle count")
	{
		auto mesh = CreateRef<MeshAsset>();
		mesh->Handle = AssetHandle(60103);
		mesh->Submeshes.emplace_back();
		assets.AddMemoryAsset(mesh);
		GraphicsDeviceLimits limits;
		limits.MaxStorageBufferBytes = 2 * sizeof(GPU::ProbeTraceMaterial);
		ProbeTraceScene trace(limits);
		std::array<Ref<MaterialAsset>, 3> materials;
		for (uint32_t i = 0; i < materials.size(); ++i)
		{
			materials[i] = CreateRef<MaterialAsset>();
			materials[i]->Handle = AssetHandle(60104 + i);
			assets.AddMemoryAsset(materials[i]);
			auto entity = AddCube(scene, {}, glm::vec3(1), materials[i]->Handle);
			entity.GetComponent<MeshComponent>().Mesh = mesh->Handle;
			if (i < 2)
				REQUIRE(trace.Update(scene, assets, changed));
		}
		CHECK_FALSE(trace.Update(scene, assets, changed));
		CHECK(logs.Count(spdlog::level::err, "ProbeTraceMaterials") == 1);
		CHECK(trace.GetMaterials().size() == 2);
		// Share a material to release that budget, then exceed the independent texel budget by one texel.
		for (auto handle : scene.GetAllEntitiesWith<MeshComponent>())
			Entity(handle, &scene).GetComponent<MeshComponent>().Materials = {materials[0]->Handle};
		std::array<uint8_t, 3 * 4 * 4> pixels{};
		auto texture = TextureAsset::CreateRGBA8(3, 4, pixels.data(), false);
		texture->Handle = AssetHandle(60107);
		assets.AddMemoryAsset(texture);
		materials[0]->BaseColorTexture = texture->Handle;
		++materials[0]->Version;
		CHECK_FALSE(trace.Update(scene, assets, changed));
		CHECK(logs.Count(spdlog::level::err, "ProbeTraceTexels") == 1);
		materials[0]->BaseColorTexture = 0;
		++materials[0]->Version;
		REQUIRE(trace.Update(scene, assets, changed));
		CHECK(trace.GetMaterials().size() == 1);
	}
}

TEST_CASE("ProbeTraceScene: validates index ranges and transforms without losing small valid surfaces")
{
	AssetManager assets;
	Scene scene;
	auto mesh = MeshPrimitives::CreateCube();
	mesh->Handle = AssetHandle(60108);
	assets.AddMemoryAsset(mesh);
	auto entity = AddCube(scene, {}, glm::vec3(1));
	entity.GetComponent<MeshComponent>().Mesh = mesh->Handle;
	ProbeTraceScene trace;
	bool changed;
	Testing::ScopedLogCapture logs;
	SUBCASE("index addition cannot wrap")
	{
		mesh->Submeshes[0].IndexOffset = std::numeric_limits<uint32_t>::max();
		++mesh->Version;
		CHECK_FALSE(trace.Update(scene, assets, changed));
		CHECK(logs.Contains(spdlog::level::err, "submesh indices"));
	}
	SUBCASE("incomplete triangles are rejected")
	{
		mesh->Submeshes[0].IndexCount -= 1;
		++mesh->Version;
		CHECK_FALSE(trace.Update(scene, assets, changed));
		CHECK(logs.Contains(spdlog::level::err, "submesh indices"));
	}
	SUBCASE("a scaled million-unit model retains its ten-centimetre surface")
	{
		for (auto& vertex : mesh->Vertices)
			vertex.Position *= 1000000;
		++mesh->Version;
		entity.GetTransform().Scale = glm::vec3(0.0000001f);
		REQUIRE(trace.Update(scene, assets, changed));
		REQUIRE(trace.GetTriangles().size() == 12);
		CHECK(trace.GetNodes()[0].Maximum.x - trace.GetNodes()[0].Minimum.x == doctest::Approx(0.1));
	}
	SUBCASE("invalid transforms warn once and skip only that instance")
	{
		AddCube(scene, {3, 0, 0}, glm::vec3(1));
		entity.GetTransform().Scale.x = std::numeric_limits<float>::infinity();
		REQUIRE(trace.Update(scene, assets, changed));
		CHECK(trace.GetTriangles().size() == 12);
		entity.GetTransform().Translation.x = 2;
		REQUIRE(trace.Update(scene, assets, changed));
		CHECK(logs.Count(spdlog::level::warn, "invalid transform") == 1);
		entity.GetTransform().Scale = glm::vec3(1);
		REQUIRE(trace.Update(scene, assets, changed));
		CHECK(trace.GetTriangles().size() == 24);
	}
}

TEST_CASE("ProbeTraceScene: mirrored transforms preserve corner attributes and inverse-transpose normals")
{
	AssetManager assets;
	Scene scene;
	auto mesh = CreateRef<MeshAsset>();
	mesh->Handle = AssetHandle(60121);
	mesh->Vertices.resize(3);
	mesh->Vertices[0].Position = {0, 0, 0};
	mesh->Vertices[1].Position = {1, 0, 0};
	mesh->Vertices[2].Position = {0, 1, 0};
	mesh->Vertices[0].Normal = glm::normalize(glm::vec3(1, 1, 0));
	mesh->Vertices[1].Normal = glm::normalize(glm::vec3(0, 1, 1));
	mesh->Vertices[2].Normal = glm::normalize(glm::vec3(1, 0, 1));
	mesh->Vertices[0].UV = {0, 0};
	mesh->Vertices[1].UV = {1, 0};
	mesh->Vertices[2].UV = {0, 1};
	mesh->Indices = {0, 1, 2};
	mesh->Submeshes.emplace_back().IndexCount = 3;
	assets.AddMemoryAsset(mesh);
	auto entity = AddCube(scene, {3, 4, 5}, {-2, 3, 0.5f});
	entity.GetComponent<MeshComponent>().Mesh = mesh->Handle;
	ProbeTraceScene trace;
	bool changed;
	REQUIRE(trace.Update(scene, assets, changed));
	REQUIRE(trace.GetTriangles().size() == 1);
	const auto& triangle = trace.GetTriangles()[0];
	const std::array<uint32_t, 3> sourceCorners{0, 2, 1};
	const std::array<glm::vec2, 3> actualUV{glm::vec2(triangle.UV01), glm::vec2(triangle.UV01.z, triangle.UV01.w), glm::vec2(triangle.UV2)};
	const glm::mat4 world = scene.GetWorldTransform(entity);
	const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(world)));
	for (uint32_t corner = 0; corner < 3; ++corner)
	{
		const auto& vertex = mesh->Vertices[sourceCorners[corner]];
		CHECK(triangle.Position[corner] == world * glm::vec4(vertex.Position, 1));
		CHECK(actualUV[corner] == vertex.UV);
		CHECK(glm::length(glm::normalize(glm::vec3(triangle.Normal[corner])) - glm::normalize(normalMatrix * vertex.Normal)) < 1e-6f);
	}
	const glm::vec3 barycentric(0.2f, 0.3f, 0.5f);
	glm::vec3 actual(0), expected(0);
	for (uint32_t corner = 0; corner < 3; ++corner)
	{
		actual += glm::vec3(triangle.Normal[corner]) * barycentric[corner];
		expected += mesh->Vertices[sourceCorners[corner]].Normal * barycentric[corner];
	}
	CHECK(glm::length(glm::normalize(actual) - glm::normalize(normalMatrix * expected)) < 1e-6f);
	CHECK(glm::cross(glm::vec3(triangle.Position[1] - triangle.Position[0]), glm::vec3(triangle.Position[2] - triangle.Position[0])).z > 0);
}

TEST_CASE("ProbeGI: history blends radiance and reactivated probes discard inactive history")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	ProbeFixture fixture(graphics.GetDevice());
	fixture.Specification.Spacing = 2;
	fixture.Specification.Origin = glm::vec3(-1);
	REQUIRE(fixture.Update(scene, assets));
	const auto before = ReadProbe(fixture.Device, fixture.GI.GetIrradiance());
	fixture.ViewData.EnvironmentParams.x = 0;
	fixture.Specification.Hysteresis = 0.5f;
	REQUIRE(fixture.Update(scene, assets));
	const auto blended = ReadProbe(fixture.Device, fixture.GI.GetIrradiance());
	for (size_t i = 0; i < before.size(); ++i)
	{
		CHECK(glm::length(glm::vec3(blended[i]) - glm::vec3(before[i]) * 0.5f) < 1e-6f);
		CHECK(blended[i].a == 1);
	}
	auto solid = AddCube(scene, fixture.Specification.Origin, glm::vec3(0.5f));
	REQUIRE(fixture.Update(scene, assets));
	for (const auto& pixel : ReadProbe(fixture.Device, fixture.GI.GetIrradiance()))
		CHECK(pixel.a == 0);
	scene.DestroyEntity(solid);
	fixture.ViewData.EnvironmentParams.x = 1;
	fixture.Specification.Hysteresis = 0.9f;
	REQUIRE(fixture.Update(scene, assets));
	const auto reactivated = ReadProbe(fixture.Device, fixture.GI.GetIrradiance());
	for (size_t i = 0; i < before.size(); ++i)
	{
		CHECK(glm::length(glm::vec3(reactivated[i]) - glm::vec3(before[i])) < 1e-5f);
		CHECK(reactivated[i].a == 1);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: spot cones illuminate only surfaces in their direction")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	AddCube(scene, {0, -1, 0}, {40, 0.1f, 40});
	ProbeFixture fixture(graphics.GetDevice());
	fixture.Specification.RaysPerProbe = 256;
	fixture.Specification.NormalBias = fixture.Specification.ViewBias = 0;
	fixture.Specification.BounceFeedback = false;
	fixture.ViewData.EnvironmentParams.x = 0;
	GPU::Light spot{};
	spot.PositionRange = {0, 1, 0, 10};
	spot.ColorIntensity = {10, 10, 10, 1};
	spot.DirectionCos = {0, 1, 0, std::cos(glm::radians(30.0f))};
	spot.Params.x = std::cos(glm::radians(15.0f));
	fixture.LightData = {spot};
	REQUIRE(fixture.Update(scene, assets));
	for (const auto& pixel : ReadProbe(fixture.Device, fixture.GI.GetIrradiance()))
		CHECK(glm::length(glm::vec3(pixel)) < 1e-6f);
	fixture.LightData[0].DirectionCos.y = -1;
	fixture.GI.ResetHistory();
	REQUIRE(fixture.Update(scene, assets));
	const auto down = fixture.GI.Inspect({}, {0, -1, 0}, {0, 0, 1});
	const auto up = fixture.GI.Inspect({}, {0, 1, 0}, {0, 0, 1});
	REQUIRE(down);
	REQUIRE(up);
	CHECK(down->Irradiance.r > 0.02f);
	CHECK(up->Irradiance.r < down->Irradiance.r * 0.1f);
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: a quarter-turn sky rotation moves a bright texel from world X to Z")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	ProbeFixture fixture(graphics.GetDevice());
	fixture.Specification.RaysPerProbe = 256;
	fixture.Specification.NormalBias = fixture.Specification.ViewBias = 0;
	fixture.Commands->open();
	for (uint32_t face = 0; face < 6; ++face)
	{
		const glm::u8vec4 pixel(face == 0 ? 255 : 0, 0, 0, 255);
		fixture.Commands->writeTexture(fixture.Sky, face, 0, &pixel, sizeof(pixel));
	}
	fixture.Commands->close();
	fixture.Device->executeCommandList(fixture.Commands);
	REQUIRE(fixture.Update(scene, assets));
	const auto x = fixture.GI.Inspect({}, {1, 0, 0}, {0, 1, 0});
	const auto negativeX = fixture.GI.Inspect({}, {-1, 0, 0}, {0, 1, 0});
	REQUIRE(x);
	REQUIRE(negativeX);
	CHECK(x->Irradiance.r > 0.1f);
	CHECK(x->Irradiance.r > 4 * negativeX->Irradiance.r);
	fixture.ViewData.EnvironmentParams.z = glm::half_pi<float>();
	fixture.GI.ResetHistory();
	REQUIRE(fixture.Update(scene, assets));
	const auto z = fixture.GI.Inspect({}, {0, 0, 1}, {0, 1, 0});
	const auto negativeZ = fixture.GI.Inspect({}, {0, 0, -1}, {0, 1, 0});
	REQUIRE(z);
	REQUIRE(negativeZ);
	CHECK(z->Irradiance.r > 4 * negativeZ->Irradiance.r);
	CHECK(std::abs(z->Irradiance.r - x->Irradiance.r) < 0.03f);
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeGI: anisotropic instances shade authored normals like equivalent baked geometry")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	AssetManager assets;
	Scene scene;
	auto mesh = MeshPrimitives::CreateCube();
	mesh->Handle = AssetHandle(60122);
	for (auto& vertex : mesh->Vertices)
	{
		vertex.Position *= glm::vec3(40, 2, 40);
		vertex.Normal = glm::normalize(glm::vec3(1, 1e-6f, 0));
	}
	assets.AddMemoryAsset(mesh);
	auto entity = AddCube(scene, {}, {1, 1e-6f, 1});
	entity.GetComponent<MeshComponent>().Mesh = mesh->Handle;
	ProbeFixture fixture(graphics.GetDevice());
	fixture.Specification.Origin.y = 0.5f;
	fixture.Specification.RaysPerProbe = 256;
	fixture.Specification.BounceFeedback = false;
	fixture.ViewData.EnvironmentParams.x = 0;
	fixture.ViewData.SunDirection = {0, -1, 0, 1};
	fixture.ViewData.SunColor = glm::vec4(1);
	REQUIRE(fixture.Update(scene, assets));
	const auto instanced = ReadProbe(fixture.Device, fixture.GI.GetIrradiance());
	for (auto& vertex : mesh->Vertices)
	{
		vertex.Position.y *= 1e-6f;
		vertex.Normal = glm::normalize(glm::vec3(1, 1, 0));
	}
	++mesh->Version;
	entity.GetTransform().Scale = glm::vec3(1);
	fixture.GI.ResetHistory();
	REQUIRE(fixture.Update(scene, assets));
	const auto baked = ReadProbe(fixture.Device, fixture.GI.GetIrradiance());
	float energy = 0;
	for (size_t i = 0; i < instanced.size(); ++i)
	{
		CHECK(glm::length(glm::vec3(instanced[i] - baked[i])) < 1e-5f);
		energy += baked[i].r;
	}
	CHECK(energy > 0.1f);
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ProbeTraceScene: material and texture rejections ignore unrelated geometry changes")
{
	AssetManager assets;
	Scene scene;
	auto mesh = MeshPrimitives::CreateCube();
	mesh->Handle = AssetHandle(60123);
	assets.AddMemoryAsset(mesh);
	auto material = CreateRef<MaterialAsset>();
	material->Handle = AssetHandle(60124);
	assets.AddMemoryAsset(material);
	auto entity = AddCube(scene, {}, glm::vec3(1), material->Handle);
	entity.GetComponent<MeshComponent>().Mesh = mesh->Handle;
	ProbeTraceScene trace;
	bool changed;
	REQUIRE(trace.Update(scene, assets, changed));
	Testing::ScopedLogCapture logs;
	SUBCASE("material values depend on the material version")
	{
		material->BaseColorFactor.r = std::numeric_limits<float>::quiet_NaN();
		++material->Version;
		REQUIRE_FALSE(trace.Update(scene, assets, changed));
		mesh->Vertices[0].Position.x += 0.1f;
		++mesh->Version;
		CHECK_FALSE(trace.Update(scene, assets, changed));
		CHECK(logs.Count(spdlog::level::err, "material values") == 1);
		material->BaseColorFactor.r = 1;
		++material->Version;
		REQUIRE(trace.Update(scene, assets, changed));
	}
	SUBCASE("malformed texture data depends on the texture version")
	{
		const std::array<uint8_t, 4> white{255, 255, 255, 255};
		auto texture = TextureAsset::CreateRGBA8(1, 1, white.data(), false);
		texture->Handle = AssetHandle(60125);
		assets.AddMemoryAsset(texture);
		material->BaseColorTexture = texture->Handle;
		++material->Version;
		texture->Mips[0].pop_back();
		++texture->Version;
		REQUIRE_FALSE(trace.Update(scene, assets, changed));
		mesh->Vertices[0].Position.x += 0.1f;
		++mesh->Version;
		material->RoughnessFactor = 0.5f;
		++material->Version;
		CHECK_FALSE(trace.Update(scene, assets, changed));
		CHECK(logs.Count(spdlog::level::err, "texture size or mip data") == 1);
		texture->Mips[0].push_back(255);
		++texture->Version;
		REQUIRE(trace.Update(scene, assets, changed));
	}
	CHECK(changed);
	CHECK(trace.GetTriangles().size() == 12);
}
