#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include "TestUtilities.h"

#include <Paralax/Renderer/PostEffectResources.h>
#include <Paralax/Renderer/AmbientOcclusion.h>
#include <Paralax/Renderer/TextureReadback.h>

#include <array>
#include <limits>
#include <string>
#include <utility>

using namespace Paralax;

TEST_CASE("PostEffectBudget: live and staged reservations share one bounded capacity")
{
	PostEffectBudget budget(100);
	auto viewport = budget.Reserve(60);
	REQUIRE(bool(viewport));
	CHECK_FALSE(bool(budget.Reserve(41)));
	auto capture = budget.Reserve(40);
	REQUIRE(bool(capture));
	CHECK(budget.GetUsedBytes() == 100);
	CHECK(budget.GetPeakBytes() == 100);
	CHECK_FALSE(bool(budget.Reserve(1)));
	CHECK_FALSE(bool(budget.Reserve(0)));
	viewport.Reset();
	CHECK(budget.GetUsedBytes() == 40);
	auto replacement = budget.Reserve(60);
	REQUIRE(bool(replacement));
	capture.Reset();
	capture.Reset();
	CHECK(budget.GetUsedBytes() == 60);
	replacement.Reset();
	CHECK(budget.GetUsedBytes() == 0);
	CHECK(budget.GetPeakBytes() == 100);
}

TEST_CASE("PostEffectBudget: moves transfer ownership once and oversized requests never overflow")
{
	PostEffectBudget budget(std::numeric_limits<uint64_t>::max());
	auto first = budget.Reserve(1);
	CHECK_FALSE(bool(budget.Reserve(std::numeric_limits<uint64_t>::max())));
	auto second = budget.Reserve(std::numeric_limits<uint64_t>::max() - 1);
	REQUIRE(bool(second));
	auto moved = std::move(second);
	CHECK_FALSE(bool(second));
	CHECK(second.GetBytes() == 0);
	first = std::move(moved);
	CHECK(budget.GetUsedBytes() == std::numeric_limits<uint64_t>::max() - 1);
	CHECK_FALSE(bool(moved));
	first.Reset();
	CHECK(budget.GetUsedBytes() == 0);
	PostEffectBudget disabled(0);
	CHECK_FALSE(disabled.Fits(1));
	CHECK_FALSE(bool(disabled.Reserve(0)));
}

namespace {

	nvrhi::TextureDesc EffectTextureDescription()
	{
		nvrhi::TextureDesc desc;
		desc.width = 17;
		desc.height = 13;
		desc.format = nvrhi::Format::RGBA8_UNORM;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		desc.debugName = "PostEffectTest";
		return desc;
	}

	class EffectFailureFactory : public PostEffectResourceFactory
	{
	public:
		enum class Failure
		{
			None,
			Unsupported,
			Format,
			Texture,
			Requirements,
			Overflow,
			Committed
		};
		Failure Mode = Failure::None;
		uint32_t TextureCalls = 0, CommittedCalls = 0;
		PostEffectBudget* Budget = nullptr;

		bool SupportsVirtualResources(nvrhi::IDevice* device) override { return Mode != Failure::Unsupported && PostEffectResourceFactory::SupportsVirtualResources(device); }
		nvrhi::FormatSupport QueryFormatSupport(nvrhi::IDevice* device, nvrhi::Format format) override
		{
			return Mode == Failure::Format ? nvrhi::FormatSupport::None : PostEffectResourceFactory::QueryFormatSupport(device, format);
		}
		nvrhi::TextureHandle CreateTexture(nvrhi::IDevice* device, const nvrhi::TextureDesc& desc) override
		{
			++TextureCalls;
			if (desc.isVirtual)
				return Mode == Failure::Texture ? nullptr : PostEffectResourceFactory::CreateTexture(device, desc);
			++CommittedCalls;
			REQUIRE(Budget);
			CHECK(Budget->GetUsedBytes() > 0);
			CHECK(Budget->GetUsedBytes() <= Budget->GetCapacity());
			return Mode == Failure::Committed ? nullptr : PostEffectResourceFactory::CreateTexture(device, desc);
		}
		nvrhi::MemoryRequirements GetTextureMemoryRequirements(nvrhi::IDevice* device, nvrhi::ITexture* texture) override
		{
			if (Mode == Failure::Requirements)
				return {};
			if (Mode == Failure::Overflow)
				return {std::numeric_limits<uint64_t>::max(), 256};
			return PostEffectResourceFactory::GetTextureMemoryRequirements(device, texture);
		}
	};

}

TEST_CASE("PostEffectTextures: actual aligned backing is reserved before allocation across views")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	auto* device = graphics.GetDevice();
	{
		auto desc = EffectTextureDescription();
		desc.isVirtual = true;
		auto unbacked = device->createTexture(desc);
		REQUIRE(unbacked);
		const auto requirements = device->getTextureMemoryRequirements(unbacked);
		REQUIRE(requirements.alignment > 0);
		const uint64_t bytes = (requirements.size + requirements.alignment - 1) / requirements.alignment * requirements.alignment;
		unbacked = nullptr;
		PostEffectBudget budget(bytes);
		EffectFailureFactory factory;
		factory.Budget = &budget;
		PostEffectTextures viewport(graphics, budget, &factory), capture(graphics, budget, &factory);
		auto* texture = viewport.Create(desc);
		REQUIRE(texture);
		CHECK(viewport.GetOwnedBytes() == bytes);
		CHECK(budget.GetUsedBytes() == bytes);
		CHECK_FALSE(capture.Create(desc));
		CHECK(factory.CommittedCalls == 1);
		CHECK(std::string(capture.GetFailureReason()) == "renderer effect memory budget exhausted");
		auto commands = device->createCommandList();
		REQUIRE(commands);
		commands->open();
		commands->clearTextureFloat(texture, nvrhi::AllSubresources, nvrhi::Color(0.25f, 0.5f, 0.75f, 1));
		commands->close();
		device->executeCommandList(commands);
		commands = nullptr;
		const Image image = ReadbackTexture(device, texture);
		REQUIRE_FALSE(image.IsEmpty());
		CHECK(image.GetPixel(8, 6) == glm::u8vec4(64, 128, 191, 255));
		viewport.Clear();
		CHECK(budget.GetUsedBytes() == 0);
		REQUIRE(capture.Create(desc));
		CHECK(factory.CommittedCalls == 2);
		CHECK(budget.GetPeakBytes() == bytes);
		capture.Clear();
		CHECK(budget.GetUsedBytes() == 0);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("PostEffectTextures: partial failures preserve live groups and release every failed reservation")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		PostEffectBudget budget;
		EffectFailureFactory factory;
		factory.Budget = &budget;
		PostEffectTextures textures(graphics, budget, &factory);
		for (auto failure : {EffectFailureFactory::Failure::Unsupported, EffectFailureFactory::Failure::Format, EffectFailureFactory::Failure::Texture,
							 EffectFailureFactory::Failure::Requirements, EffectFailureFactory::Failure::Overflow, EffectFailureFactory::Failure::Committed})
		{
			CAPTURE(static_cast<int>(failure));
			factory.Mode = EffectFailureFactory::Failure::None;
			REQUIRE(textures.Create(EffectTextureDescription()));
			const uint64_t liveBytes = budget.GetUsedBytes();
			factory.Mode = failure;
			CHECK_FALSE(textures.Create(EffectTextureDescription()));
			CHECK_FALSE(std::string(textures.GetFailureReason()).empty());
			CHECK(budget.GetUsedBytes() == liveBytes);
			CHECK(textures.GetOwnedBytes() == liveBytes);
			factory.Mode = EffectFailureFactory::Failure::None;
			REQUIRE(textures.Create(EffectTextureDescription()));
			CHECK(budget.GetUsedBytes() == liveBytes * 2);
			textures.Clear();
			CHECK(textures.GetOwnedBytes() == 0);
			CHECK(budget.GetUsedBytes() == 0);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("PostEffectTextures: invalid dimensions and mip counts fail before creating GPU objects")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		PostEffectBudget budget;
		EffectFailureFactory factory;
		factory.Budget = &budget;
		PostEffectTextures textures(graphics, budget, &factory);
		for (uint32_t invalid : {0u, 8193u, std::numeric_limits<uint32_t>::max()})
		{
			auto desc = EffectTextureDescription();
			desc.width = invalid;
			CHECK_FALSE(textures.Create(desc));
			desc = EffectTextureDescription();
			desc.height = invalid;
			CHECK_FALSE(textures.Create(desc));
		}
		for (uint32_t invalid : {0u, 6u, std::numeric_limits<uint32_t>::max()})
		{
			auto desc = EffectTextureDescription();
			desc.mipLevels = invalid;
			CHECK_FALSE(textures.Create(desc));
		}
		CHECK(factory.TextureCalls == 0);
		CHECK(factory.CommittedCalls == 0);
		CHECK(budget.GetUsedBytes() == 0);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("PostEffectTextures: retained bindings and commands remain charged after group retirement")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	auto* device = graphics.GetDevice();
	{
		PostEffectBudget budget;
		PostEffectTextures textures(graphics, budget);
		auto* texture = textures.Create(EffectTextureDescription());
		REQUIRE(texture);
		const auto bytes = budget.GetUsedBytes();
		nvrhi::FramebufferHandle framebuffer;
		nvrhi::BindingLayoutHandle layout;
		nvrhi::BindingSetHandle binding;
		nvrhi::CommandListHandle commands;
		SUBCASE("framebuffer retains backing")
		{
			framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture));
			REQUIRE(framebuffer);
		}
		SUBCASE("binding retains backing")
		{
			nvrhi::BindingLayoutDesc layoutDesc;
			layoutDesc.visibility = nvrhi::ShaderType::Pixel;
			layoutDesc.bindings = {nvrhi::BindingLayoutItem::Texture_SRV(0)};
			layout = device->createBindingLayout(layoutDesc);
			REQUIRE(layout);
			nvrhi::BindingSetDesc bindingDesc;
			bindingDesc.bindings = {nvrhi::BindingSetItem::Texture_SRV(0, texture)};
			binding = device->createBindingSet(bindingDesc, layout);
			REQUIRE(binding);
		}
		SUBCASE("unsubmitted command retains backing")
		{
			commands = device->createCommandList();
			REQUIRE(commands);
			commands->open();
			// NVRHI clear alone does not retain resources; explicit state tracking does.
			PostEffectTextures::TrackUsage(commands, texture, nvrhi::ResourceStates::CopyDest);
			commands->clearTextureFloat(texture, nvrhi::AllSubresources, nvrhi::Color(1));
			commands->close();
		}
		textures.Clear();
		CHECK(textures.GetOwnedBytes() == 0);
		CHECK(budget.GetUsedBytes() == bytes);
		budget.CollectRetired();
		CHECK(budget.GetUsedBytes() == bytes);
		// A command recorded before retirement can still be submitted safely afterwards.
		if (commands)
			device->executeCommandList(commands);
		framebuffer = nullptr;
		binding = nullptr;
		commands = nullptr;
		graphics.WaitForIdle();
		graphics.CollectGarbage();
		budget.CollectRetired();
		CHECK(budget.GetUsedBytes() == 0);
		CHECK(budget.GetPeakBytes() == bytes);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("PostEffectTextures: raw usage tracking requires both command list and texture in every configuration")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		PostEffectBudget budget;
		PostEffectTextures textures(graphics, budget);
		auto* texture = textures.Create(EffectTextureDescription());
		REQUIRE(texture);
		auto commands = graphics.GetDevice()->createCommandList();
		REQUIRE(commands);
		Testing::ScopedAssertionCapture capture;
		CHECK_THROWS_AS(PostEffectTextures::TrackUsage(nullptr, texture, nvrhi::ResourceStates::CopyDest), Testing::AssertionFailedError);
		CHECK_THROWS_AS(PostEffectTextures::TrackUsage(commands, nullptr, nvrhi::ResourceStates::CopyDest), Testing::AssertionFailedError);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("AmbientOcclusion: guide clearing and rendering require prepared resources")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		PostEffectBudget budget;
		AmbientOcclusion ao(graphics, budget);
		auto commands = graphics.GetDevice()->createCommandList();
		REQUIRE(commands);
		Testing::ScopedAssertionCapture capture;
		CHECK_THROWS_AS(ao.ClearGuide(commands), Testing::AssertionFailedError);
		CHECK_THROWS_AS(ao.Render(commands, glm::mat4(1), false, 0.5f, 0.02f, 1, 16), Testing::AssertionFailedError);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("PostEffectBuffers: structured buffers share texture capacity and retain charges until their last reference retires")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		PostEffectBudget budget;
		PostEffectTextures textures(graphics, budget);
		REQUIRE(textures.Create(EffectTextureDescription()));
		const auto textureBytes = budget.GetUsedBytes();
		PostEffectBuffers buffers(graphics, budget);
		CHECK_FALSE(buffers.Create(0, 16, "Invalid"));
		CHECK_FALSE(buffers.Create(32, 3, "Invalid"));
		CHECK_FALSE(buffers.Create(33, 16, "Invalid"));
		CHECK_FALSE(buffers.Create(32, 16, nullptr));
		CHECK(buffers.GetAllocationAttempts() == 0);
		nvrhi::BufferHandle retained = buffers.Create(16 * 8, 16, "TemporalTestRecords");
		REQUIRE(retained);
		const auto combined = budget.GetUsedBytes();
		CHECK(combined > textureBytes);
		buffers.Clear();
		CHECK(budget.GetUsedBytes() == combined);
		retained = nullptr;
		budget.CollectRetired();
		CHECK(budget.GetUsedBytes() == textureBytes);
		textures.Clear();
		CHECK(budget.GetUsedBytes() == 0);
		PostEffectBudget tiny(1);
		PostEffectBuffers rejected(graphics, tiny);
		CHECK_FALSE(rejected.Create(16, 16, "OverBudget"));
		CHECK(rejected.GetBackingAttempts() == 0);
		CHECK(tiny.GetUsedBytes() == 0);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("PostEffectBuffers: preflight and committed allocation failures leave no partial charge and recover")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		class Factory : public PostEffectResourceFactory
		{
		public:
			uint32_t Mode = 0;
			bool SupportsVirtualResources(nvrhi::IDevice* device) override { return Mode != 1 && PostEffectResourceFactory::SupportsVirtualResources(device); }
			nvrhi::BufferHandle CreateBuffer(nvrhi::IDevice* device, const nvrhi::BufferDesc& desc) override
			{
				return (Mode == 2 && desc.isVirtual) || (Mode == 3 && !desc.isVirtual) ? nullptr : PostEffectResourceFactory::CreateBuffer(device, desc);
			}
			nvrhi::MemoryRequirements GetBufferMemoryRequirements(nvrhi::IDevice* device, nvrhi::IBuffer* buffer) override
			{
				auto result = PostEffectResourceFactory::GetBufferMemoryRequirements(device, buffer);
				if (Mode == 4)
					result.alignment = 0;
				if (Mode == 5)
					result.size = std::numeric_limits<uint64_t>::max();
				return result;
			}
		} factory;
		PostEffectBudget budget;
		for (uint32_t mode = 1; mode <= 5; ++mode)
		{
			PostEffectBuffers buffers(graphics, budget, &factory);
			factory.Mode = mode;
			CHECK_FALSE(buffers.Create(144 * 4, 144, "FailureRecords"));
			CHECK(budget.GetUsedBytes() == 0);
			factory.Mode = 0;
			CHECK(buffers.Create(144 * 4, 144, "RecoveredRecords"));
			CHECK(budget.GetUsedBytes() > 0);
			buffers.Clear();
			CHECK(budget.GetUsedBytes() == 0);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}
