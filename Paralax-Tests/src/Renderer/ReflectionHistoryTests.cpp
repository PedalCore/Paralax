#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Renderer/ReflectionHistory.h>
#include <Paralax/Renderer/SceneRenderer.h>
#include <Paralax/Renderer/SceneRendererTypes.h>
#include <Paralax/Scene/Scene.h>
#include <Paralax/Scene/Entity.h>

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstring>

using namespace Paralax;

namespace {

	struct HistoryFixture
	{
		GraphicsDevice& Graphics;
		PostEffectBudget Budget;
		ReflectionHistory History;
		ReflectionHistory::Inputs Inputs;
		TemporalFrameSnapshot Snapshot;
		GPU::ViewConstants View{};
		std::array<glm::vec4, 9> Colors, Normals, Guides, Hits, Source, Environment;
		std::array<float, 9> Depth;
		explicit HistoryFixture(GraphicsDevice& graphics)
			: Graphics(graphics)
			, History(graphics, Budget)
		{
			auto* device = Graphics.GetDevice();
			for (uint32_t i = 0; i < 8; ++i)
			{
				nvrhi::TextureDesc desc;
				desc.width = desc.height = 3;
				desc.format = i == 1 ? nvrhi::Format::R32_FLOAT : nvrhi::Format::RGBA32_FLOAT;
				desc.isRenderTarget = i == 7;
				desc.initialState = i == 7 ? nvrhi::ResourceStates::RenderTarget : nvrhi::ResourceStates::ShaderResource;
				desc.keepInitialState = true;
				auto texture = device->createTexture(desc);
				REQUIRE(texture);
				if (i == 7)
					Inputs.HDR = texture;
				else
					Inputs.Textures[i] = texture;
			}
			nvrhi::BufferDesc desc;
			desc.byteSize = sizeof(View);
			desc.isConstantBuffer = true;
			desc.isVolatile = true;
			desc.maxVersions = 16;
			Inputs.ViewConstants = device->createBuffer(desc);
			REQUIRE(Inputs.ViewConstants);
			Inputs.Generation = CreateRef<int>(1);
			Snapshot.Scene = CreateRef<int>(1);
			Snapshot.InternalSize = Snapshot.OutputSize = {3, 3};
			Snapshot.Projection = glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, 10.0f, 0.1f);
			View.View = glm::mat4(1);
			View.Projection = View.ViewProjection = Snapshot.Projection;
			View.InverseViewProjection = glm::inverse(View.ViewProjection);
			ResetInputs();
		}
		void ResetInputs()
		{
			Colors.fill({1, 0, 0, 1});
			Normals.fill({0, 0, 0, 0});
			Guides.fill({0, 0, 0.2f, 1});
			Hits.fill({0, 0, -3, 1});
			Source.fill({0.2f, 0.2f, 0.2f, 1});
			Environment.fill({0.1f, 0.1f, 0.1f, 1});
			Depth.fill((Snapshot.Projection * glm::vec4(0, 0, -1, 1)).z);
		}
		glm::vec4 Frame()
		{
			auto* device = Graphics.GetDevice();
			REQUIRE(History.Prepare(Inputs));
			REQUIRE(History.GetState().Stage(Snapshot, {}));
			auto commands = device->createCommandList();
			commands->open();
			commands->writeBuffer(Inputs.ViewConstants, &View, sizeof(View));
			commands->writeTexture(Inputs.Textures[0], 0, 0, Colors.data(), 3 * sizeof(glm::vec4));
			commands->writeTexture(Inputs.Textures[1], 0, 0, Depth.data(), 3 * sizeof(float));
			commands->writeTexture(Inputs.Textures[2], 0, 0, Normals.data(), 3 * sizeof(glm::vec4));
			commands->writeTexture(Inputs.Textures[3], 0, 0, Guides.data(), 3 * sizeof(glm::vec4));
			commands->writeTexture(Inputs.Textures[4], 0, 0, Hits.data(), 3 * sizeof(glm::vec4));
			commands->writeTexture(Inputs.Textures[5], 0, 0, Source.data(), 3 * sizeof(glm::vec4));
			commands->writeTexture(Inputs.Textures[6], 0, 0, Environment.data(), 3 * sizeof(glm::vec4));
			History.ResolveAndCompose(commands, View.ViewProjection, 0.85f, 0.1f, false);
			auto desc = Inputs.HDR->getDesc();
			desc.isRenderTarget = false;
			auto staging = device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
			REQUIRE(staging);
			commands->copyTexture(staging, {}, Inputs.HDR, {});
			commands->close();
			device->executeCommandList(commands);
			History.Commit();
			Graphics.WaitForIdle();
			size_t pitch = 0;
			const auto* bytes = static_cast<const uint8_t*>(device->mapStagingTexture(staging, {}, nvrhi::CpuAccessMode::Read, &pitch));
			REQUIRE(bytes);
			glm::vec4 result;
			std::memcpy(&result, bytes + pitch + sizeof(result), sizeof(result));
			device->unmapStagingTexture(staging);
			return result;
		}
	};

}

TEST_CASE("ReflectionHistory: compatible radiance blends with current confidence and rejects misses and disocclusions")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		HistoryFixture fixture(graphics);
		CHECK(fixture.Frame().x == doctest::Approx(1.1).epsilon(0.001));
		const auto backing = fixture.History.GetBackingAttempts();
		fixture.Colors[4].x = 0.5f;
		CHECK(fixture.Frame().x == doctest::Approx(1.025).epsilon(0.001));
		fixture.Colors[4].w = 0;
		CHECK(fixture.Frame() == fixture.Source[4]);
		fixture.Colors[4].w = 1;
		CHECK(fixture.Frame().x == doctest::Approx(0.6).epsilon(0.001));
		// A private capture can replace scratch descriptors without destroying this view's owned data.
		fixture.History.DetachInputs();
		CHECK(fixture.History.GetState().IsValid());
		CHECK(fixture.Frame().x == doctest::Approx(0.6).epsilon(0.001));
		CHECK(fixture.History.GetBackingAttempts() == backing);
		for (uint32_t invalidation = 0; invalidation < 5; ++invalidation)
		{
			CAPTURE(invalidation);
			fixture.History.GetState().Reset("test new source");
			fixture.ResetInputs();
			fixture.Frame();
			fixture.Colors[4].x = 0.5f;
			switch (invalidation)
			{
				case 0: fixture.Hits[4].x += 1; break;
				case 1: fixture.Normals[4].x = 0.5f; break;
				case 2: fixture.Guides[4].z = 0.5f; break;
				case 3: fixture.Depth[4] = (fixture.Snapshot.Projection * glm::vec4(0, 0, -2, 1)).z; break;
				case 4: ++fixture.Snapshot.Settings; break;
			}
			CHECK(fixture.Frame().x == doctest::Approx(0.6).epsilon(0.001));
		}
		fixture.History.Release();
		CHECK(fixture.Budget.GetUsedBytes() == 0);
		Testing::ScopedAssertionCapture capture;
		CHECK_THROWS_AS(fixture.History.Commit(), Testing::AssertionFailedError);
		CHECK_THROWS_AS(fixture.History.ResolveAndCompose(nullptr, glm::mat4(1), 0.85f, 0.1f, false), Testing::AssertionFailedError);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ReflectionHistory: private captures warmup and stateless external AA preserve viewport ownership")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene viewport("Viewport"), capture("Capture");
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		renderer.GetSettings().GIEnabled = false;
		renderer.GetSettings().SSREnabled = true;
		nvrhi::TextureDesc desc;
		desc.width = desc.height = 32;
		desc.format = nvrhi::Format::SRGBA8_UNORM;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		auto texture = graphics.GetDevice()->createTexture(desc);
		auto output = graphics.GetDevice()->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture));
		const auto a = CameraView::LookAt({0, 0, 4}, {0, 0, 0}, glm::radians(60.0f), 1);
		const auto b = CameraView::LookAt({2, 1, 4}, {0, 0, 0}, glm::radians(60.0f), 1);
		renderer.Render(viewport, a, output);
		renderer.Render(viewport, a, output);
		REQUIRE(renderer.GetStats().SSRHistoryActive);
		CHECK(renderer.GetStats().SSRHistoryEligible);
		auto& owner = renderer.GetDefaultReflectionHistory();
		const auto completed = owner.GetState().GetCompletedFrames();
		const auto token = owner.GetState().GetPreviousFrame().Scene;
		const auto backing = owner.GetBackingAttempts();
		auto aa = renderer.CreateTemporalHistory();
		auto privateHistory = renderer.CreateReflectionHistory();
		auto warmup = renderer.CreateReflectionHistory();
		renderer.Render(capture, b, output, nullptr, aa.get(), warmup.get());
		renderer.Render(capture, b, output, nullptr, aa.get(), privateHistory.get());
		const auto privateBacking = privateHistory->GetBackingAttempts();
		renderer.Render(capture, b, output, nullptr, aa.get(), privateHistory.get());
		CHECK(privateHistory->GetBackingAttempts() == privateBacking);
		CHECK(privateHistory->GetState().GetCompletedFrames() == 2);
		CHECK(warmup->GetState().GetCompletedFrames() == 1);
		CHECK(owner.GetState().GetCompletedFrames() == completed);
		CHECK(owner.GetState().GetPreviousFrame().Scene == token);
		renderer.Render(capture, b, output, nullptr, aa.get());
		CHECK(renderer.GetStats().SSRActive);
		CHECK_FALSE(renderer.GetStats().SSRHistoryActive);
		CHECK(owner.GetState().GetCompletedFrames() == completed);
		renderer.Render(viewport, a, output);
		CHECK(renderer.GetStats().SSRHistoryEligible);
		CHECK(owner.GetBackingAttempts() == backing);
		CHECK(owner.GetState().GetCompletedFrames() == completed + 1);
		CHECK(owner.GetState().GetPreviousFrame().Scene == token);
		renderer.ResetTemporalHistories("explicit reset");
		CHECK_FALSE(owner.GetState().IsValid());
		renderer.Render(viewport, a, output);
		CHECK_FALSE(renderer.GetStats().SSRHistoryEligible);

		// Disabling a private view must not destroy the viewport's completed data or hide its budget charge.
		warmup->Release();
		const auto viewportBytes = owner.GetOwnedBytes();
		const auto viewportFrames = owner.GetState().GetCompletedFrames();
		const auto viewportBacking = owner.GetBackingAttempts();
		REQUIRE(viewportBytes > 0);
		REQUIRE(privateHistory->GetOwnedBytes() > 0);
		renderer.GetSettings().SSREnabled = false;
		renderer.Render(capture, b, output, nullptr, aa.get(), privateHistory.get());
		CHECK_FALSE(renderer.GetStats().SSRActive);
		CHECK_FALSE(renderer.GetStats().SSRHistoryActive);
		CHECK(privateHistory->GetOwnedBytes() == 0);
		CHECK(owner.GetOwnedBytes() == viewportBytes);
		CHECK(owner.GetBackingAttempts() == viewportBacking);
		CHECK(owner.GetState().GetCompletedFrames() == viewportFrames);
		CHECK(owner.GetState().GetPreviousFrame().Scene == token);
		CHECK(owner.GetState().IsValid());
		CHECK(renderer.GetStats().EffectOwnedBytes == viewportBytes);
		aa.reset();
		privateHistory.reset();
		warmup.reset();
		renderer.Render(viewport, a, output);
		CHECK(owner.GetOwnedBytes() == 0);
		CHECK_FALSE(owner.GetState().IsValid());
		CHECK(renderer.GetStats().EffectOwnedBytes == 0);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ReflectionHistory: fractional reprojection follows a tilted plane but rejects a parallel depth edge")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	for (bool perspective : {false, true})
		for (bool discontinuity : {false, true})
		{
			CAPTURE(discontinuity);
			CAPTURE(perspective);
			HistoryFixture fixture(graphics);
			if (perspective)
			{
				fixture.Snapshot.Projection = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 10.0f, 0.1f);
				fixture.View.Projection = fixture.View.ViewProjection = fixture.Snapshot.Projection;
				fixture.View.InverseViewProjection = glm::inverse(fixture.View.ViewProjection);
			}
			for (uint32_t y = 0; y < 3; ++y)
				for (uint32_t x = 0; x < 3; ++x)
				{
					const auto i = y * 3 + x;
					const float planeDepth = perspective ? 1.0f / (0.5f - (float(x) - 1) * (2.0f / 9)) : 1.0f + float(x);
					const float depth = discontinuity && x == 0 ? 2.0f : planeDepth;
					const auto clip = fixture.Snapshot.Projection * glm::vec4(0, 0, -depth, 1);
					fixture.Depth[i] = clip.z / clip.w;
					fixture.Normals[i] = {0.6f, 0, 0.6f, 0};
					fixture.Hits[i] = {float(x) - 1, 0, -4 - 0.5f * (float(x) - 1), 1};
				}
			fixture.Frame();
			// The new center maps to previous x=1.75: .25 pixel beyond the stored center.
			fixture.Snapshot.View = glm::translate(glm::mat4(1), {perspective ? -0.375f : -1.0f / 6, 0, 0});
			fixture.View.View = fixture.Snapshot.View;
			fixture.View.ViewProjection = fixture.Snapshot.Projection * fixture.Snapshot.View;
			fixture.View.InverseViewProjection = glm::inverse(fixture.View.ViewProjection);
			const auto clip = fixture.Snapshot.Projection * glm::vec4(0, 0, -2.25f, 1);
			fixture.Depth[4] = clip.z / clip.w;
			fixture.Hits[4] = {0.25f, 0, -4.125f, 1};
			fixture.Colors[4].x = 0.5f;
			const auto result = fixture.Frame();
			CHECK(result.x == doctest::Approx(discontinuity ? 0.6 : 1.025).epsilon(0.001));
		}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ReflectionHistory: another view overwriting shared scratch cannot replace completed reflection radiance")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		HistoryFixture viewport(graphics), capture(graphics);
		viewport.Frame();
		const auto backing = viewport.History.GetBackingAttempts();
		viewport.History.DetachInputs();
		capture.Inputs = viewport.Inputs;
		capture.Colors.fill({0, 1, 0, 1});
		CHECK(capture.Frame().y == doctest::Approx(1.1).epsilon(0.001));
		capture.History.DetachInputs();
		viewport.Colors[4].x = 0.5f;
		const auto restored = viewport.Frame();
		CHECK(restored.x == doctest::Approx(1.025).epsilon(0.001));
		CHECK(restored.y == doctest::Approx(0.1).epsilon(0.001));
		CHECK(viewport.History.GetBackingAttempts() == backing);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ReflectionHistory: a shallow parallel depth step cannot gain acceptance from slope correction")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		HistoryFixture fixture(graphics);
		for (uint32_t y = 0; y < 3; ++y)
			for (uint32_t x = 0; x < 3; ++x)
				fixture.Depth[y * 3 + x] = (fixture.Snapshot.Projection * glm::vec4(0, 0, x == 2 ? -2.12f : -2.0f, 1)).z;
		fixture.Frame();
		fixture.Snapshot.View = glm::translate(glm::mat4(1), {-0.49f * 2 / 3, 0, 0});
		fixture.View.View = fixture.Snapshot.View;
		fixture.View.ViewProjection = fixture.Snapshot.Projection * fixture.Snapshot.View;
		fixture.View.InverseViewProjection = glm::inverse(fixture.View.ViewProjection);
		fixture.Depth[4] = (fixture.Snapshot.Projection * glm::vec4(0, 0, -2.12f, 1)).z;
		fixture.Colors[4].x = 0.5f;
		CHECK(fixture.Frame().x == doctest::Approx(0.6).epsilon(0.001));
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ReflectionHistory: malformed input contracts fail without GPU validation errors and recover")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		HistoryFixture fixture(graphics);
		for (uint32_t kind = 0; kind < 4; ++kind)
		{
			CAPTURE(kind);
			auto input = fixture.Inputs;
			if (kind == 0)
				input.Generation.reset();
			if (kind == 1)
				input.ViewConstants = nullptr;
			if (kind == 2)
				input.Textures[1] = input.Textures[2];
			if (kind == 3)
				input.HDR = input.Textures[0];
			CHECK_FALSE(fixture.History.Prepare(input));
			CHECK_FALSE(fixture.History.GetFallbackReason().empty());
			CHECK(fixture.History.GetOwnedBytes() == 0);
			REQUIRE(fixture.History.Prepare(fixture.Inputs));
			CHECK(fixture.History.GetOwnedBytes() > 0);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ReflectionHistory: visible shadow caster edits invalidate reflected lighting with GI disabled")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("ReflectionCasterEdit");
		auto caster = scene.CreateEntity("Visible caster");
		caster.AddComponent<MeshComponent>().Mesh = AssetManager::BuiltinCube;
		scene.CreateEntity("Sun").AddComponent<DirectionalLightComponent>();
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		renderer.GetSettings().GIEnabled = false;
		renderer.GetSettings().SSREnabled = true;
		renderer.GetSettings().ShadowResolution = ShadowQuality::Low;
		renderer.GetSettings().ShadowDistance = 10;
		nvrhi::TextureDesc desc;
		desc.width = desc.height = 32;
		desc.format = nvrhi::Format::SRGBA8_UNORM;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		auto texture = graphics.GetDevice()->createTexture(desc);
		auto output = graphics.GetDevice()->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture));
		const auto camera = CameraView::LookAt({0, 1, 4}, {0, 0, 0}, glm::radians(60.0f), 1);
		renderer.Render(scene, camera, output);
		renderer.Render(scene, camera, output);
		REQUIRE(renderer.GetStats().SSRHistoryActive);
		REQUIRE(renderer.GetStats().SSRHistoryEligible);
		REQUIRE(renderer.GetStats().ShadowDrawCalls > 0);
		caster.GetComponent<MeshComponent>().CastShadows = false;
		renderer.Render(scene, camera, output);
		CHECK(renderer.GetStats().ShadowDrawCalls == 0);
		CHECK_FALSE(renderer.GetStats().SSRHistoryEligible);
		renderer.Render(scene, camera, output);
		CHECK(renderer.GetStats().SSRHistoryEligible);
		caster.GetComponent<MeshComponent>().CastShadows = true;
		renderer.Render(scene, camera, output);
		CHECK(renderer.GetStats().ShadowDrawCalls > 0);
		CHECK_FALSE(renderer.GetStats().SSRHistoryEligible);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}
