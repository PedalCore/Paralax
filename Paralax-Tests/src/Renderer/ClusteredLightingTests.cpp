#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include <Paralax/Core/EmbeddedFile.h>
#include <Paralax/Renderer/ClusteredLighting.h>
#include <Paralax/Renderer/ShaderLibrary.h>

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

namespace Paralax::EmbeddedTestShaders {
	extern const EmbeddedFile* const g_Entries;
	extern const size_t g_EntryCount;
}

using namespace Paralax;

namespace {
	constexpr uint64_t MaxBytes = 128ull * 1024 * 1024;
	glm::mat4 Perspective()
	{
		return glm::perspectiveRH_ZO(1.1f, 1.3f, 0.1f, 100.0f);
	}

	struct Fixture
	{
		nvrhi::IDevice* Device;
		PostEffectBudget Budget;
		ClusteredLighting Clusters;
		nvrhi::BufferHandle Constants, Original;
		GPU::ViewConstants View{};
		std::vector<GPU::Light> Lights;

		explicit Fixture(GraphicsDevice& graphics, PostEffectResourceFactory* factory = nullptr, uint64_t budgetCapacity = PostEffectBudget::DefaultCapacity)
			: Device(graphics.GetDevice())
			, Budget(budgetCapacity)
			, Clusters(graphics, Budget, factory)
		{
			Constants = Device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(View)).setIsConstantBuffer(true).setInitialState(nvrhi::ResourceStates::ConstantBuffer).setKeepInitialState(true));
			Original = Device->createBuffer(nvrhi::BufferDesc().setByteSize(ClusteredLightingGrid::PrefixBytes).setStructStride(sizeof(GPU::Light)).setCanHaveRawViews(true).setInitialState(nvrhi::ResourceStates::ShaderResource).setKeepInitialState(true));
			REQUIRE(Constants);
			REQUIRE(Original);
		}

		void Prepare(uint32_t width, uint32_t height, const glm::mat4& projection, const glm::mat4& view, float nearDepth = 0.1f, float farDepth = 100.0f)
		{
			auto grid = ClusteredLightingGrid::Create(width, height, static_cast<uint32_t>(Lights.size()), nearDepth, farDepth, view, projection, MaxBytes);
			REQUIRE(grid);
			REQUIRE(Clusters.Prepare(*grid, Constants));
			View.View = view;
			View.ViewProjection = projection * view;
			View.Clusters = grid->GetConstants();
		}

		std::vector<uint32_t> Run(const std::vector<glm::vec4>& positions, bool abi = false)
		{
			const auto code = FindEmbeddedFile(EmbeddedTestShaders::g_Entries, EmbeddedTestShaders::g_EntryCount, "ClusteredLightingTest");
			REQUIRE_FALSE(code.empty());
			nvrhi::ShaderDesc shaderDesc;
			shaderDesc.shaderType = nvrhi::ShaderType::Compute;
			shaderDesc.entryName = abi ? "CheckABI" : "Lookup";
			auto shader = Device->createShader(shaderDesc, code.data(), code.size());
			REQUIRE(shader);
			nvrhi::BindingLayoutDesc layoutDesc;
			layoutDesc.visibility = nvrhi::ShaderType::Compute;
			layoutDesc.bindings = {nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::RawBuffer_SRV(0), nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1), nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2), nvrhi::BindingLayoutItem::RawBuffer_UAV(0)};
			auto layout = Device->createBindingLayout(layoutDesc);
			auto pipeline = Device->createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(shader).addBindingLayout(layout));
			REQUIRE(pipeline);
			auto points = Device->createBuffer(nvrhi::BufferDesc().setByteSize(positions.size() * sizeof(glm::vec4)).setStructStride(sizeof(glm::vec4)).setInitialState(nvrhi::ResourceStates::ShaderResource).setKeepInitialState(true));
			const size_t count = abi ? Lights.size() : positions.size();
			const size_t bytes = count * (abi ? 128 : 80);
			auto output = Device->createBuffer(nvrhi::BufferDesc().setByteSize(bytes).setCanHaveRawViews(true).setCanHaveUAVs(true).setInitialState(nvrhi::ResourceStates::UnorderedAccess).setKeepInitialState(true));
			auto staging = Device->createBuffer(nvrhi::BufferDesc().setByteSize(bytes).setCpuAccess(nvrhi::CpuAccessMode::Read).setInitialState(nvrhi::ResourceStates::CopyDest).setKeepInitialState(true));
			REQUIRE(points);
			REQUIRE(output);
			REQUIRE(staging);
			nvrhi::BindingSetDesc desc;
			desc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, Constants), nvrhi::BindingSetItem::RawBuffer_SRV(0, abi ? Original.Get() : Clusters.GetBuffer()), nvrhi::BindingSetItem::StructuredBuffer_SRV(1, Original), nvrhi::BindingSetItem::StructuredBuffer_SRV(2, points), nvrhi::BindingSetItem::RawBuffer_UAV(0, output)};
			auto bindings = Device->createBindingSet(desc, layout);
			REQUIRE(bindings);
			auto commands = Device->createCommandList();
			commands->open();
			commands->writeBuffer(Constants, &View, sizeof(View));
			commands->writeBuffer(Original, Lights.data(), Lights.size() * sizeof(GPU::Light));
			commands->writeBuffer(points, positions.data(), positions.size() * sizeof(glm::vec4));
			if (!abi)
				REQUIRE(Clusters.Dispatch(commands, Lights));
			nvrhi::ComputeState state;
			state.pipeline = pipeline;
			state.bindings = {bindings};
			commands->setComputeState(state);
			commands->dispatch(static_cast<uint32_t>(count), 1, 1);
			commands->copyBuffer(staging, 0, output, 0, bytes);
			commands->close();
			Device->executeCommandList(commands);
			Device->waitForIdle();
			const void* mapped = Device->mapBuffer(staging, nvrhi::CpuAccessMode::Read);
			REQUIRE(mapped);
			std::vector<uint32_t> result(bytes / 4);
			std::memcpy(result.data(), mapped, bytes);
			Device->unmapBuffer(staging);
			return result;
		}
	};
}

TEST_CASE("ClusteredLighting: grid limits and packed depth boundaries reject malformed requests")
{
	const glm::mat4 view(1);
	auto projection = Perspective();
	auto grid = ClusteredLightingGrid::Create(1280, 720, 256, 0.1f, 100, view, projection, MaxBytes);
	REQUIRE(grid);
	CHECK(grid->GetCellCount() == 3840);
	CHECK(grid->GetBufferBytes() == 139264);
	const auto& constants = grid->GetConstants();
	CHECK(constants.Depth[0][0] == 0.1f);
	CHECK(constants.Depth[4][0] == 100);
	CHECK(constants.Depth[4][1] == 0);
	for (uint32_t i = 1; i <= 16; ++i)
		CHECK(constants.Depth[i / 4][i % 4] > constants.Depth[(i - 1) / 4][(i - 1) % 4]);
	grid = ClusteredLightingGrid::Create(8192, 8192, 1, 0.1f, 100, view, projection, MaxBytes);
	REQUIRE(grid);
	CHECK(grid->GetBufferBytes() == 8404992);
	CHECK_FALSE(ClusteredLightingGrid::Create(8192, 8192, 1, 0.1f, 100, view, projection, 8404991));
	for (uint32_t count : {0u, 1u, 255u, 256u})
		CHECK(ClusteredLightingGrid::Create(1, 1, count, 0.1f, 100, view, projection, MaxBytes));
	CHECK_FALSE(ClusteredLightingGrid::Create(1, 1, 257, 0.1f, 100, view, projection, MaxBytes));
	for (uint32_t width : {0u, 8193u, std::numeric_limits<uint32_t>::max()})
		CHECK_FALSE(ClusteredLightingGrid::Create(width, 1, 1, 0.1f, 100, view, projection, MaxBytes));
	for (float nearDepth : {0.0f, -1.0f, 100.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::denorm_min()})
		CHECK_FALSE(ClusteredLightingGrid::Create(1, 1, 1, nearDepth, 100, view, projection, MaxBytes));
	CHECK_FALSE(ClusteredLightingGrid::Create(1, 1, 1, 1, std::nextafter(1.0f, 2.0f), view, projection, MaxBytes));
	projection[0][3] = 0.1f;
	CHECK_FALSE(ClusteredLightingGrid::Create(1, 1, 1, 0.1f, 100, view, projection, MaxBytes));
	CHECK(ClusteredLightingGrid::Create(1, 1, 1, -10, 100, view, glm::orthoRH_ZO(-10.0f, 10.0f, -10.0f, 10.0f, -10.0f, 100.0f), MaxBytes));
}

TEST_CASE("ClusteredLighting: raw prefix and structured GI views preserve all light fields")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		Fixture fixture(graphics);
		fixture.Lights.resize(256);
		for (size_t i = 0; i < fixture.Lights.size(); ++i)
		{
			auto& light = fixture.Lights[i];
			light.PositionRange = glm::vec4(float(i), -float(i), 0.125f, 33.0f);
			light.ColorIntensity = glm::vec4(0.25f, 0.5f, 2.0f, float(i % 2));
			light.DirectionCos = glm::vec4(-0.25f, 0.375f, -0.75f, 0.66f);
			light.Params = glm::vec4(0.75f, float(i % 16), 1.0f, 0.1f);
		}
		const auto result = fixture.Run({glm::vec4(0)}, true);
		for (size_t i = 0; i < fixture.Lights.size(); ++i)
		{
			CHECK(std::memcmp(result.data() + i * 32, &fixture.Lights[i], 64) == 0);
			CHECK(std::memcmp(result.data() + i * 32 + 16, &fixture.Lights[i], 64) == 0);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ClusteredLighting: actual GPU lookup never omits double distance or legacy predicate hits")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		Fixture fixture(graphics);
		std::mt19937 random(9817);
		std::uniform_real_distribution<float> coordinate(-20, 20);
		for (uint32_t camera = 0; camera < 5; ++camera)
		{
			CAPTURE(camera);
			const bool ortho = camera == 1;
			glm::mat4 projection = ortho ? glm::orthoRH_ZO(-20.0f, 20.0f, -20.0f, 20.0f, -10.0f, 100.0f) : Perspective();
			if (camera == 2)
			{
				projection[2][0] = 0.157f;
				projection[2][1] = -0.271f;
			}
			const glm::vec3 translation = camera == 3 ? glm::vec3(1e7f, -1e7f, 1e7f) : glm::vec3(0);
			const auto view = glm::translate(glm::mat4(1), -translation);
			fixture.Lights.assign(256, {});
			for (auto& light : fixture.Lights)
				light.PositionRange = glm::vec4(glm::vec3(coordinate(random), coordinate(random), -25 + coordinate(random)) + translation, 2.25f);
			fixture.Lights[0].PositionRange = glm::vec4(translation, 1e-19f);
			fixture.Lights[1].PositionRange.w = 0;
			fixture.Lights[2].PositionRange.w = -3;
			fixture.Lights[3].PositionRange.w = std::numeric_limits<float>::denorm_min();
			fixture.Lights[4].PositionRange.w = std::numeric_limits<float>::max();
			fixture.Lights[5].PositionRange.w = std::numeric_limits<float>::quiet_NaN();
			fixture.Lights[255].PositionRange = glm::vec4(translation + glm::vec3(0, 0, -5), 1000);
			fixture.Prepare(193, 131, projection, view, ortho ? -10.0f : 0.1f);
			std::vector<glm::vec4> positions;
			// Independent double unprojection samples all cell edges and just-adjacent float points.
			const glm::dmat4 inverse = glm::inverse(glm::dmat4(fixture.View.ViewProjection));
			for (double y : {0.0, 0.25, 63.9999, 64.0, 64.0001, 130.5, 131.0})
				for (double x : {0.0, 0.25, 63.9999, 64.0, 64.0001, 128.0, 192.5, 193.0})
					for (double z : {0.0, 0.1, 0.7, 0.99, 1.0})
					{
						const glm::dvec4 world = inverse * glm::dvec4(x / 193 * 2 - 1, 1 - y / 131 * 2, z, 1);
						positions.emplace_back(glm::vec3(world / world.w), 1);
					}
			for (uint32_t i = 0; i <= 16; ++i)
			{
				const float depth = fixture.View.Clusters.Depth[i / 4][i % 4];
				for (float z : {depth, std::nextafter(depth, -std::numeric_limits<float>::infinity()), std::nextafter(depth, std::numeric_limits<float>::infinity())})
					positions.emplace_back(translation + glm::vec3(0.01f, 0.02f, -z), 1);
			}
			for (const auto& light : fixture.Lights)
			{
				positions.emplace_back(glm::vec3(light.PositionRange), 1);
				if (std::isfinite(light.PositionRange.w) && std::abs(light.PositionRange.w) < 100)
					for (int axis = 0; axis < 3; ++axis)
					{
						glm::vec4 point(glm::vec3(light.PositionRange), 1);
						point[axis] += light.PositionRange.w;
						positions.push_back(point);
					}
			}
			positions.emplace_back(0, 0, -1e-12f, 1);
			positions.emplace_back(std::numeric_limits<float>::denorm_min(), 0, -1, 1);
			positions.emplace_back(1e20f, 0, -1, 1);
			const auto result = fixture.Run(positions);
			uint32_t certified = 0, excluded = 0;
			for (size_t sample = 0; sample < positions.size(); ++sample)
			{
				certified += result[sample * 20] != 0xffffffffu;
				CHECK(result[sample * 20 + 2] == 0);
				if (result[sample * 20] != 0xffffffffu)
				{
					const glm::dvec4 point(positions[sample]);
					const glm::dvec4 clip = glm::dmat4(fixture.View.ViewProjection) * point;
					const double pixelX = (clip.x / clip.w * 0.5 + 0.5) * 193;
					const double pixelY = (0.5 - clip.y / clip.w * 0.5) * 131;
					REQUIRE(pixelX >= 0);
					REQUIRE(pixelX < 193);
					REQUIRE(pixelY >= 0);
					REQUIRE(pixelY < 131);
					const double depth = -(glm::dmat4(fixture.View.View) * point).z;
					uint32_t slice = 0;
					for (uint32_t boundary = 1; boundary < 16; ++boundary)
						if (depth >= double(fixture.View.Clusters.Depth[boundary / 4][boundary % 4]))
							slice = boundary;
					const uint32_t expected = (slice * 3 + static_cast<uint32_t>(pixelY / 64)) * 4 + static_cast<uint32_t>(pixelX / 64);
					CHECK(result[sample * 20] == expected);
				}
				for (uint32_t word = 0; word < 8; ++word)
				{
					const uint32_t mask = result[sample * 20 + 4 + word];
					const uint32_t legacy = result[sample * 20 + 12 + word];
					CHECK((legacy & ~mask) == 0);
					excluded += static_cast<uint32_t>(std::popcount(~mask));
					for (uint32_t bit = 0; bit < 32; ++bit)
					{
						const auto& light = fixture.Lights[word * 32 + bit];
						const glm::dvec3 delta = glm::dvec3(light.PositionRange) - glm::dvec3(positions[sample]);
						if (glm::dot(delta, delta) <= double(light.PositionRange.w) * double(light.PositionRange.w))
							CHECK((mask & (1u << bit)) != 0);
					}
				}
			}
			if (camera != 3)
			{
				CHECK(certified > 100);
				CHECK(excluded > 1000);
			}
			const auto attempts = fixture.Clusters.GetAllocationAttempts();
			fixture.Lights.resize(1);
			fixture.Prepare(193, 131, projection, view, ortho ? -10.0f : 0.1f);
			CHECK(fixture.Clusters.GetAllocationAttempts() == attempts);
			const auto shrunk = fixture.Run(positions);
			for (size_t sample = 0; sample < positions.size(); ++sample)
			{
				CHECK((shrunk[sample * 20 + 4] & ~1u) == 0);
				for (uint32_t word = 1; word < 8; ++word)
					CHECK(shrunk[sample * 20 + 4 + word] == 0);
			}
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

namespace {
	class ClusterFailureFactory : public PostEffectResourceFactory
	{
	public:
		uint32_t Mode = 0;
		nvrhi::BufferHandle CreateBuffer(nvrhi::IDevice* device, const nvrhi::BufferDesc& desc) override
		{
			if ((Mode == 1 && desc.isVirtual) || (Mode == 2 && !desc.isVirtual))
				return nullptr;
			return PostEffectResourceFactory::CreateBuffer(device, desc);
		}
		nvrhi::BindingLayoutHandle CreateBindingLayout(nvrhi::IDevice* device, const nvrhi::BindingLayoutDesc& desc) override
		{
			return Mode == 3 ? nullptr : PostEffectResourceFactory::CreateBindingLayout(device, desc);
		}
		nvrhi::ComputePipelineHandle CreateComputePipeline(nvrhi::IDevice* device, const nvrhi::ComputePipelineDesc& desc) override
		{
			return Mode == 4 ? nullptr : PostEffectResourceFactory::CreateComputePipeline(device, desc);
		}
		nvrhi::BindingSetHandle CreateBindingSet(nvrhi::IDevice* device, const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout) override
		{
			return Mode == 5 || Mode == 6 ? nullptr : PostEffectResourceFactory::CreateBindingSet(device, desc, layout);
		}
	};
}

TEST_CASE("ClusteredLighting: resource failures cache and publication leaves baseline bindings intact")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		ClusterFailureFactory factory;
		Fixture fixture(graphics, &factory);
		auto grid = ClusteredLightingGrid::Create(65, 65, 1, 0.1f, 100, glm::mat4(1), Perspective(), MaxBytes);
		REQUIRE(grid);
		for (uint32_t mode = 1; mode <= 5; ++mode)
		{
			CAPTURE(mode);
			factory.Mode = mode;
			fixture.Clusters.Release();
			const auto backing = fixture.Clusters.GetBackingAttempts();
			CHECK_FALSE(fixture.Clusters.Prepare(*grid, fixture.Constants));
			CHECK_FALSE(fixture.Clusters.IsReady());
			CHECK(fixture.Clusters.GetOwnedBytes() == 0);
			CHECK(fixture.Budget.GetUsedBytes() == 0);
			CHECK(fixture.Clusters.GetBackingAttempts() == backing + (mode == 1 ? 0 : 1));
			const auto attempts = fixture.Clusters.GetAllocationAttempts();
			CHECK_FALSE(fixture.Clusters.Prepare(*grid, fixture.Constants));
			CHECK(fixture.Clusters.GetAllocationAttempts() == attempts);
		}
		factory.Mode = 0;
		fixture.Clusters.Release();
		REQUIRE(fixture.Clusters.Prepare(*grid, fixture.Constants));
		auto* device = fixture.Device;
		nvrhi::BindingLayoutDesc layoutDesc;
		layoutDesc.visibility = nvrhi::ShaderType::All;
		layoutDesc.bindings = {nvrhi::BindingLayoutItem::RawBuffer_SRV(0), nvrhi::BindingLayoutItem::ConstantBuffer(0)};
		auto layout = device->createBindingLayout(layoutDesc);
		nvrhi::BindingSetDesc desc;
		desc.bindings = {nvrhi::BindingSetItem::RawBuffer_SRV(0, fixture.Original), nvrhi::BindingSetItem::ConstantBuffer(0, fixture.Constants)};
		auto baseline = device->createBindingSet(desc, layout);
		REQUIRE(baseline);
		const auto backing = fixture.Clusters.GetBackingAttempts();
		const auto preparedBytes = fixture.Clusters.GetOwnedBytes();
		factory.Mode = 6;
		CHECK_FALSE(fixture.Clusters.FinalizeView(baseline, layout, fixture.View.Clusters));
		CHECK(fixture.View.Clusters.State.y == 0);
		CHECK(fixture.Clusters.GetOwnedBytes() == preparedBytes);
		CHECK(fixture.Budget.GetUsedBytes() == preparedBytes);
		const auto attempts = fixture.Clusters.GetAllocationAttempts();
		CHECK_FALSE(fixture.Clusters.FinalizeView(baseline, layout, fixture.View.Clusters));
		CHECK(fixture.View.Clusters.State.y == 0);
		CHECK(fixture.Clusters.GetOwnedBytes() == preparedBytes);
		CHECK(fixture.Budget.GetUsedBytes() == preparedBytes);
		CHECK(fixture.Clusters.GetAllocationAttempts() == attempts);
		CHECK(baseline->getDesc()->bindings[0].resourceHandle == fixture.Original.Get());
		factory.Mode = 0;
		auto reservation = fixture.Budget.Reserve(1);
		REQUIRE(bool(reservation));
		auto* published = fixture.Clusters.FinalizeView(baseline, layout, fixture.View.Clusters);
		REQUIRE(published);
		CHECK(fixture.View.Clusters.State.y == 1);
		CHECK(published->getDesc()->bindings.size() == baseline->getDesc()->bindings.size());
		CHECK(published->getDesc()->bindings[0].resourceHandle == fixture.Clusters.GetBuffer());
		CHECK(published->getDesc()->bindings[1].resourceHandle == fixture.Constants.Get());
		CHECK(fixture.Clusters.GetBackingAttempts() == backing);
		CHECK(fixture.Clusters.FinalizeView(baseline, layout, fixture.View.Clusters) == published);
		auto commands = device->createCommandList();
		commands->open();
		std::vector<GPU::Light> invalid(257);
		CHECK_FALSE(fixture.Clusters.Dispatch(commands, invalid));
		CHECK_FALSE(fixture.Clusters.Dispatch(commands, {}));
		commands->close();
		// Retire even this empty recording; NVRHI retains an opened command buffer until submission.
		device->executeCommandList(commands);
		device->waitForIdle();
		commands = nullptr;
		const auto successfulAttempts = fixture.Clusters.GetAllocationAttempts();
		reservation.Reset();
		CHECK(fixture.Clusters.FinalizeView(baseline, layout, fixture.View.Clusters) == published);
		CHECK(fixture.Clusters.GetAllocationAttempts() == successfulAttempts);
		CHECK(fixture.Clusters.GetBackingAttempts() == backing);
		nvrhi::BufferHandle retained = fixture.Clusters.GetBuffer();
		fixture.Clusters.Release();
		CHECK(fixture.Clusters.GetOwnedBytes() == 0);
		CHECK(fixture.Budget.GetUsedBytes() > 0);
		retained = nullptr;
		device->runGarbageCollection();
		fixture.Budget.CollectRetired();
		CHECK(fixture.Budget.GetUsedBytes() == 0);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ClusteredLighting: final maximum grid word preserves integer masks across repeated dispatch and shrink")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		Fixture fixture(graphics);
		fixture.Lights.assign(256, {});
		for (auto& light : fixture.Lights)
			light.PositionRange = glm::vec4(0, 0, -1, 1e20f);
		fixture.Prepare(8192, 8192, Perspective(), glm::mat4(1));
		auto* device = fixture.Device;
		auto staging = device->createBuffer(nvrhi::BufferDesc().setByteSize(32).setCpuAccess(nvrhi::CpuAccessMode::Read).setInitialState(nvrhi::ResourceStates::CopyDest).setKeepInitialState(true));
		REQUIRE(staging);
		for (uint32_t count : {256u, 1u})
		{
			fixture.Lights.resize(count);
			fixture.Prepare(8192, 8192, Perspective(), glm::mat4(1));
			auto commands = device->createCommandList();
			commands->open();
			commands->writeBuffer(fixture.Constants, &fixture.View, sizeof(fixture.View));
			REQUIRE(fixture.Clusters.Dispatch(commands, fixture.Lights));
			REQUIRE(fixture.Clusters.Dispatch(commands, fixture.Lights));
			commands->copyBuffer(staging, 0, fixture.Clusters.GetBuffer(), 8404992 - 32, 32);
			commands->close();
			device->executeCommandList(commands);
			device->waitForIdle();
			const auto* mapped = static_cast<const uint32_t*>(device->mapBuffer(staging, nvrhi::CpuAccessMode::Read));
			REQUIRE(mapped);
			for (uint32_t word = 0; word < 8; ++word)
				CHECK(mapped[word] == (count == 256 ? 0xffffffffu : word == 0 ? 1u
																			  : 0u));
			device->unmapBuffer(staging);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ClusteredLighting: released external backing unblocks a cached resize without manual budget collection")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		Fixture fixture(graphics, nullptr, 300000);
		fixture.Lights.resize(1);
		fixture.Prepare(1024, 1024, Perspective(), glm::mat4(1));
		nvrhi::BufferHandle retained = fixture.Clusters.GetBuffer();
		auto larger = ClusteredLightingGrid::Create(2048, 1024, 1, 0.1f, 100, glm::mat4(1), Perspective(), MaxBytes);
		REQUIRE(larger);
		CHECK_FALSE(fixture.Clusters.Prepare(*larger, fixture.Constants));
		const auto attempts = fixture.Clusters.GetAllocationAttempts();
		CHECK_FALSE(fixture.Clusters.Prepare(*larger, fixture.Constants));
		CHECK(fixture.Clusters.GetAllocationAttempts() == attempts);
		retained = nullptr;
		REQUIRE(fixture.Clusters.Prepare(*larger, fixture.Constants));
		CHECK(fixture.Clusters.GetAllocationAttempts() > attempts);
		CHECK(fixture.Budget.GetUsedBytes() == fixture.Clusters.GetOwnedBytes());
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ClusteredLighting: certified plane tangencies and tiny range threshold preserve accepted GPU hits")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		Fixture fixture(graphics);
		for (float halfWidth : {1.0f, 1e-10f})
		{
			CAPTURE(halfWidth);
			const float inset = halfWidth * 0.001f;
			std::vector<glm::vec4> points;
			fixture.Lights.clear();
			// Points lie strictly inside all four cells adjacent to the x/y=0 corner.
			// Each axis tangent light extends through that point from the other cell.
			for (float sx : {-1.0f, 1.0f})
				for (float sy : {-1.0f, 1.0f})
				{
					const glm::vec3 point(sx * inset, sy * inset, -0.37f);
					points.emplace_back(point, 1);
					for (float radius : {std::nextafter(halfWidth, 0.0f), halfWidth, std::nextafter(halfWidth, 2.0f)})
						for (int axis = 0; axis < 3; ++axis)
						{
							GPU::Light light{};
							light.PositionRange = glm::vec4(point, radius);
							light.PositionRange[axis] -= radius;
							fixture.Lights.push_back(light);
						}
					GPU::Light corner{};
					corner.PositionRange = glm::vec4(point - glm::vec3(halfWidth * 0.5f, halfWidth * 0.5f, 0), halfWidth);
					fixture.Lights.push_back(corner);
				}
			// A clearly separated AABB must be excluded; an all-light oracle cannot pass.
			GPU::Light outside{};
			outside.PositionRange = glm::vec4(halfWidth * 16, halfWidth * 16, -0.37f, halfWidth * 2);
			fixture.Lights.push_back(outside);
			fixture.Prepare(128, 128, glm::orthoRH_ZO(-halfWidth, halfWidth, -halfWidth, halfWidth, 0.0f, 2.0f), glm::mat4(1), 0, 2);
			const auto output = fixture.Run(points);
			for (size_t i = 0; i < points.size(); ++i)
			{
				REQUIRE(output[i * 20] != 0xffffffffu);
				for (uint32_t word = 0; word < 8; ++word)
					CHECK((output[i * 20 + 12 + word] & ~output[i * 20 + 4 + word]) == 0);
				for (size_t index = 0; index < fixture.Lights.size(); ++index)
				{
					const auto& light = fixture.Lights[index];
					const auto delta = glm::dvec3(light.PositionRange) - glm::dvec3(points[i]);
					if (glm::dot(delta, delta) <= double(light.PositionRange.w) * double(light.PositionRange.w))
						CHECK((output[i * 20 + 4 + index / 32] & (1u << (index % 32))) != 0);
				}
				const size_t last = fixture.Lights.size() - 1;
				CHECK((output[i * 20 + 4 + last / 32] & (1u << (last % 32))) == 0);
			}
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ClusteredLighting: raw allocation rejects invalid descriptions before an allocation attempt")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		PostEffectBudget budget;
		PostEffectBuffers buffers(graphics, budget);
		for (uint64_t bytes : {uint64_t(0), uint64_t(3), std::numeric_limits<uint64_t>::max(), PostEffectBudget::DefaultCapacity + 4})
			CHECK_FALSE(buffers.CreateRaw(bytes, "InvalidRaw"));
		CHECK_FALSE(buffers.CreateRaw(16, nullptr));
		CHECK(buffers.GetAllocationAttempts() == 0);
		auto* raw = buffers.CreateRaw(16, "ValidRaw");
		REQUIRE(raw);
		CHECK(raw->getDesc().canHaveRawViews);
		CHECK(raw->getDesc().canHaveUAVs);
		CHECK(raw->getDesc().structStride == 0);
		CHECK(buffers.GetBackingAttempts() == 1);
		CHECK(budget.GetUsedBytes() == buffers.GetOwnedBytes());
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}
