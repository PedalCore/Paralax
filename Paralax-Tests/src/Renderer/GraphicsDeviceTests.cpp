#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include "TestUtilities.h"

#include <Paralax/Renderer/GraphicsDevice.h>
#include <Paralax/Renderer/ShaderLibrary.h>
#include <Paralax/Renderer/TextureReadback.h>

#include <glm/glm.hpp>
#include <nvrhi/utils.h>

#include <cstdlib>

using namespace Paralax;
using Paralax::Testing::GetTestGraphicsDevice;
using Paralax::Testing::GetTestOutputDirectory;
using Paralax::Testing::ReleaseTestGraphicsDevice;
using Paralax::Testing::RequireTestGraphicsDevice;

namespace {

	constexpr uint32_t s_Size = 64;

	nvrhi::TextureHandle CreateRenderTarget(nvrhi::IDevice* device, const char* name, uint32_t width = s_Size, uint32_t height = s_Size,
											nvrhi::Format format = nvrhi::Format::RGBA8_UNORM)
	{
		nvrhi::TextureDesc desc;
		desc.width = width;
		desc.height = height;
		desc.format = format;
		desc.isRenderTarget = true;
		desc.debugName = name;
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		desc.setClearValue(nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
		return device->createTexture(desc);
	}

	bool Near(glm::u8vec4 actual, glm::u8vec4 expected, int tolerance = 2)
	{
		for (int i = 0; i < 4; i++)
		{
			if (std::abs(int(actual[i]) - int(expected[i])) > tolerance)
				return false;
		}
		return true;
	}

	// Validation errors are counted for the whole shared device; each test checks it introduced none.
	struct ValidationCheck
	{
		ValidationCheck()
			: Initial(GetTestGraphicsDevice() ? GetTestGraphicsDevice()->GetValidationErrorCount() : 0)
		{
		}
		~ValidationCheck()
		{
			if (GraphicsDevice* device = GetTestGraphicsDevice())
				CHECK(device->GetValidationErrorCount() == Initial);
		}
		uint32_t Initial;
	};

}

TEST_CASE("GraphicsDevice: creates a headless Vulkan 1.3 device and reports adapter info")
{
	if (!RequireTestGraphicsDevice())
		return;

	GraphicsDevice* device = GetTestGraphicsDevice();
	const GraphicsDeviceInfo& info = device->GetInfo();
	MESSAGE("Adapter: ", info.AdapterName, " (", info.DriverName, ")");

	CHECK(device->GetDevice() != nullptr);
	CHECK_FALSE(info.AdapterName.empty());
	CHECK(info.ApiVersionMajor == 1);
	CHECK(info.ApiVersionMinor >= 3);
	CHECK(info.Limits.SubPixelPrecisionBits >= 4);
	CHECK(info.Limits.MaxSampledTexturesPerStage >= 16);
	CHECK(info.Limits.MaxSampledTexturesPerSet >= 96);
	CHECK(device->GetNativeDevice() != nullptr);
	CHECK(device->GetVulkanProcAddress("vkQueueSubmit") != nullptr);
	CHECK(device->GetValidationErrorCount() == 0);
}

TEST_CASE("GraphicsDevice: a preferred adapter that does not exist falls back to the best device")
{
	if (!RequireTestGraphicsDevice())
		return;

	Paralax::Testing::ReleaseTestGraphicsDevice();
	Paralax::Testing::ScopedEnvironmentVariable noOverride("PX_GPU", std::nullopt);
	Paralax::Testing::ScopedLogCapture capture;
	GraphicsDeviceSpecification specification;
	specification.Headless = true;
	specification.PreferredAdapter = "no-such-adapter-xyz";
	Scope<GraphicsDevice> device = GraphicsDevice::Create(specification);
	REQUIRE(device);
	CHECK(capture.Contains(spdlog::level::warn, "not found"));
}

TEST_CASE("GraphicsDevice: synchronization validation reports a read of a buffer written without a barrier")
{
	if (!Paralax::Testing::RequireSynchronizationValidation())
		return;
	Paralax::GraphicsDevice& graphics = *Paralax::Testing::GetTestGraphicsDevice();
	nvrhi::IDevice* device = graphics.GetDevice();
	const uint32_t errorsBefore = graphics.GetValidationErrorCount();
	Paralax::Testing::ScopedLogCapture logs;

	const auto makeBuffer = [&](const char* name, nvrhi::ResourceStates state) {
		return device->createBuffer(nvrhi::BufferDesc().setByteSize(256).setInitialState(state).setKeepInitialState(true).setDebugName(name));
	};
	nvrhi::BufferHandle source = makeBuffer("SyncValidationSource", nvrhi::ResourceStates::CopySource);
	nvrhi::BufferHandle middle = makeBuffer("SyncValidationMiddle", nvrhi::ResourceStates::CopyDest);
	nvrhi::BufferHandle destination = makeBuffer("SyncValidationDestination", nvrhi::ResourceStates::CopyDest);
	REQUIRE(source);
	REQUIRE(middle);
	REQUIRE(destination);

	nvrhi::CommandListHandle commands = device->createCommandList();
	REQUIRE(commands);
	commands->open();
	// NVRHI would order the second copy after the first by itself; recording them without its barriers is the
	// hazard the layer must report (a read of 'middle' racing the write that fills it).
	commands->setEnableAutomaticBarriers(false);
	commands->copyBuffer(middle, 0, source, 0, 256);
	commands->copyBuffer(destination, 0, middle, 0, 256);
	commands->setEnableAutomaticBarriers(true);
	commands->close();
	device->executeCommandList(commands);
	device->waitForIdle();

	CHECK(graphics.GetValidationErrorCount() > errorsBefore);
	CHECK(logs.Contains(spdlog::level::err, "SYNC-HAZARD-READ-AFTER-WRITE"));

	// The deliberate error stays counted on the shared device; later tests expect a clean one.
	commands = nullptr;
	source = nullptr;
	middle = nullptr;
	destination = nullptr;
	Paralax::Testing::ReleaseTestGraphicsDevice();
}

TEST_CASE("GraphicsDevice: missing required instance extensions fail creation with an error")
{
	Paralax::Testing::ReleaseTestGraphicsDevice();
	Paralax::Testing::ScopedLogCapture capture;
	GraphicsDeviceSpecification specification;
	specification.Headless = true;
	specification.InstanceExtensions = {"VK_PARALAX_definitely_not_an_extension"};
	CHECK(GraphicsDevice::Create(specification) == nullptr);
	// Either the extension is rejected or (on machines without Vulkan) no loader is found; both are errors.
	CHECK((capture.Contains(spdlog::level::err, "is not available") || capture.Contains(spdlog::level::err, "no Vulkan loader")));
}

TEST_CASE("GraphicsDevice: only one device may exist at a time")
{
	if (!RequireTestGraphicsDevice())
		return;

	Paralax::Testing::ScopedLogCapture capture;
	GraphicsDeviceSpecification specification;
	specification.Headless = true;
	CHECK(GraphicsDevice::Create(specification) == nullptr);
	CHECK(capture.Contains(spdlog::level::err, "already exists"));

	// The existing device is unaffected.
	CHECK(GetTestGraphicsDevice()->GetDevice() != nullptr);
}

TEST_CASE("ShaderLibrary: embedded modules are listed and unknown modules are reported")
{
	auto names = ShaderLibrary::GetModuleNames();
	CHECK(std::find(names.begin(), names.end(), "DebugShapes") != names.end());

	std::span<const uint8_t> module = ShaderLibrary::GetModule("DebugShapes");
	REQUIRE(module.size() >= 20);
	// SPIR-V magic number, little-endian.
	CHECK(module[0] == 0x03);
	CHECK(module[1] == 0x02);
	CHECK(module[2] == 0x23);
	CHECK(module[3] == 0x07);
	CHECK(ShaderLibrary::GetModule("NoSuchModule").empty());
	{
		Paralax::Testing::ScopedAssertionCapture assertions;
		CHECK_THROWS_AS(ShaderLibrary::CreateShader(nullptr, "DebugShapes", "main", nvrhi::ShaderType::Vertex), Paralax::Testing::AssertionFailedError);
	}

	if (!RequireTestGraphicsDevice())
		return;
	Paralax::Testing::ScopedLogCapture capture;
	CHECK(ShaderLibrary::CreateShader(GetTestGraphicsDevice()->GetDevice(), "NoSuchModule", "main", nvrhi::ShaderType::Vertex) == nullptr);
	CHECK(capture.Contains(spdlog::level::err, "is not embedded"));
}

TEST_CASE("Renderer: clearing a render target and reading it back returns the clear color")
{
	if (!RequireTestGraphicsDevice())
		return;
	ValidationCheck validation;
	nvrhi::IDevice* device = GetTestGraphicsDevice()->GetDevice();

	nvrhi::TextureHandle target = CreateRenderTarget(device, "ClearTarget");
	REQUIRE(target);

	nvrhi::CommandListHandle commandList = device->createCommandList();
	commandList->open();
	commandList->clearTextureFloat(target, nvrhi::AllSubresources, nvrhi::Color(0.25f, 0.5f, 0.75f, 1.0f));
	commandList->close();
	device->executeCommandList(commandList);

	Image image = ReadbackTexture(device, target);
	REQUIRE(image.GetWidth() == s_Size);
	REQUIRE(image.GetHeight() == s_Size);
	for (uint32_t y = 0; y < s_Size; y += 7)
		for (uint32_t x = 0; x < s_Size; x += 7)
			REQUIRE(Near(image.GetPixel(x, y), {64, 128, 191, 255}));
}

TEST_CASE("Renderer: draws a vertex-colored triangle with the embedded DebugShapes shaders")
{
	if (!RequireTestGraphicsDevice())
		return;
	ValidationCheck validation;
	nvrhi::IDevice* device = GetTestGraphicsDevice()->GetDevice();

	nvrhi::TextureHandle target = CreateRenderTarget(device, "TriangleTarget");
	nvrhi::FramebufferHandle framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target));
	nvrhi::ShaderHandle vs = ShaderLibrary::CreateShader(device, "DebugShapes", "TriangleVS", nvrhi::ShaderType::Vertex);
	nvrhi::ShaderHandle ps = ShaderLibrary::CreateShader(device, "DebugShapes", "TrianglePS", nvrhi::ShaderType::Pixel);
	REQUIRE(vs);
	REQUIRE(ps);

	nvrhi::GraphicsPipelineDesc pipelineDesc;
	pipelineDesc.VS = vs;
	pipelineDesc.PS = ps;
	pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
	pipelineDesc.renderState.rasterState.setCullNone();
	pipelineDesc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
	nvrhi::GraphicsPipelineHandle pipeline = device->createGraphicsPipeline(pipelineDesc, framebuffer->getFramebufferInfo());
	REQUIRE(pipeline);

	nvrhi::CommandListHandle commandList = device->createCommandList();
	commandList->open();
	commandList->clearTextureFloat(target, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
	nvrhi::GraphicsState state;
	state.pipeline = pipeline;
	state.framebuffer = framebuffer;
	state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(s_Size), float(s_Size)));
	commandList->setGraphicsState(state);
	commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
	commandList->close();
	device->executeCommandList(commandList);

	Image image = ReadbackTexture(device, target);
	REQUIRE_FALSE(image.IsEmpty());
	image.SavePNG(GetTestOutputDirectory() / "Triangle.png");

	// Outside the triangle: background.
	CHECK(Near(image.GetPixel(1, 1), {0, 0, 0, 255}));
	CHECK(Near(image.GetPixel(62, 62), {0, 0, 0, 255}));

	// Near each vertex the corresponding color dominates. Clip space is D3D-style (+Y up): red is at the top.
	const glm::u8vec4 nearTop = image.GetPixel(32, 19);
	const glm::u8vec4 nearRight = image.GetPixel(46, 46);
	const glm::u8vec4 nearLeft = image.GetPixel(18, 46);
	CHECK(nearTop.r > 150);
	CHECK(nearTop.r > nearTop.g + 50);
	CHECK(nearRight.g > 150);
	CHECK(nearRight.g > nearRight.b + 50);
	CHECK(nearLeft.b > 150);
	CHECK(nearLeft.b > nearLeft.r + 50);

	// Interior is covered and opaque.
	const glm::u8vec4 center = image.GetPixel(32, 36);
	CHECK(int(center.r) + int(center.g) + int(center.b) > 200);
	CHECK(center.a == 255);
}

TEST_CASE("Renderer: a fullscreen pass fills the target with a color from a constant buffer")
{
	if (!RequireTestGraphicsDevice())
		return;
	ValidationCheck validation;
	nvrhi::IDevice* device = GetTestGraphicsDevice()->GetDevice();

	nvrhi::TextureHandle target = CreateRenderTarget(device, "SolidTarget");
	nvrhi::FramebufferHandle framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target));

	nvrhi::BufferDesc constantsDesc = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(glm::vec4), "SolidColorConstants", 4);
	nvrhi::BufferHandle constants = device->createBuffer(constantsDesc);

	nvrhi::BindingLayoutDesc layoutDesc;
	layoutDesc.visibility = nvrhi::ShaderType::Pixel;
	layoutDesc.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0)};
	nvrhi::BindingLayoutHandle layout = device->createBindingLayout(layoutDesc);

	nvrhi::BindingSetDesc setDesc;
	setDesc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, constants)};
	nvrhi::BindingSetHandle bindingSet = device->createBindingSet(setDesc, layout);

	nvrhi::GraphicsPipelineDesc pipelineDesc;
	pipelineDesc.VS = ShaderLibrary::CreateShader(device, "DebugShapes", "FullscreenVS", nvrhi::ShaderType::Vertex);
	pipelineDesc.PS = ShaderLibrary::CreateShader(device, "DebugShapes", "SolidColorPS", nvrhi::ShaderType::Pixel);
	pipelineDesc.bindingLayouts = {layout};
	pipelineDesc.renderState.rasterState.setCullNone();
	pipelineDesc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
	nvrhi::GraphicsPipelineHandle pipeline = device->createGraphicsPipeline(pipelineDesc, framebuffer->getFramebufferInfo());
	REQUIRE(pipeline);

	const glm::vec4 color(1.0f, 0.5f, 0.0f, 1.0f);
	nvrhi::CommandListHandle commandList = device->createCommandList();
	commandList->open();
	commandList->writeBuffer(constants, &color, sizeof(color));
	nvrhi::GraphicsState state;
	state.pipeline = pipeline;
	state.framebuffer = framebuffer;
	state.bindings = {bindingSet};
	state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(s_Size), float(s_Size)));
	commandList->setGraphicsState(state);
	commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
	commandList->close();
	device->executeCommandList(commandList);

	Image image = ReadbackTexture(device, target);
	REQUIRE_FALSE(image.IsEmpty());
	for (uint32_t y = 0; y < s_Size; y += 3)
		for (uint32_t x = 0; x < s_Size; x += 3)
			REQUIRE(Near(image.GetPixel(x, y), {255, 128, 0, 255}));
}

TEST_CASE("Renderer: fullscreen-triangle UVs start at the top-left of the render target")
{
	if (!RequireTestGraphicsDevice())
		return;
	ValidationCheck validation;
	nvrhi::IDevice* device = GetTestGraphicsDevice()->GetDevice();

	nvrhi::TextureHandle target = CreateRenderTarget(device, "UVTarget");
	nvrhi::FramebufferHandle framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target));

	nvrhi::GraphicsPipelineDesc pipelineDesc;
	pipelineDesc.VS = ShaderLibrary::CreateShader(device, "DebugShapes", "FullscreenVS", nvrhi::ShaderType::Vertex);
	pipelineDesc.PS = ShaderLibrary::CreateShader(device, "DebugShapes", "UVGradientPS", nvrhi::ShaderType::Pixel);
	pipelineDesc.renderState.rasterState.setCullNone();
	pipelineDesc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
	nvrhi::GraphicsPipelineHandle pipeline = device->createGraphicsPipeline(pipelineDesc, framebuffer->getFramebufferInfo());
	REQUIRE(pipeline);

	nvrhi::CommandListHandle commandList = device->createCommandList();
	commandList->open();
	nvrhi::GraphicsState state;
	state.pipeline = pipeline;
	state.framebuffer = framebuffer;
	state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(s_Size), float(s_Size)));
	commandList->setGraphicsState(state);
	commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
	commandList->close();
	device->executeCommandList(commandList);

	Image image = ReadbackTexture(device, target);
	REQUIRE_FALSE(image.IsEmpty());

	// Pixel centers: UV = (x + 0.5) / size.
	CHECK(image.GetPixel(0, 0).r < 4);
	CHECK(image.GetPixel(0, 0).g < 4);
	CHECK(image.GetPixel(63, 0).r > 250);
	CHECK(image.GetPixel(63, 0).g < 4);
	CHECK(image.GetPixel(0, 63).g > 250);
	CHECK(image.GetPixel(32, 32).r == doctest::Approx(130).epsilon(0.02));
}

TEST_CASE("TextureReadback: handles odd widths (padded row pitch) and BGRA formats")
{
	if (!RequireTestGraphicsDevice())
		return;
	ValidationCheck validation;
	nvrhi::IDevice* device = GetTestGraphicsDevice()->GetDevice();

	for (nvrhi::Format format : {nvrhi::Format::RGBA8_UNORM, nvrhi::Format::BGRA8_UNORM})
	{
		CAPTURE(static_cast<int>(format));
		nvrhi::TextureHandle target = CreateRenderTarget(device, "OddTarget", 33, 7, format);
		REQUIRE(target);

		nvrhi::CommandListHandle commandList = device->createCommandList();
		commandList->open();
		commandList->clearTextureFloat(target, nvrhi::AllSubresources, nvrhi::Color(1.0f, 0.0f, 0.0f, 1.0f));
		commandList->close();
		device->executeCommandList(commandList);

		Image image = ReadbackTexture(device, target);
		REQUIRE(image.GetWidth() == 33);
		REQUIRE(image.GetHeight() == 7);
		// Red regardless of the GPU channel order; the last column of every row must not contain padding bytes.
		for (uint32_t y = 0; y < 7; y++)
		{
			REQUIRE(Near(image.GetPixel(0, y), {255, 0, 0, 255}));
			REQUIRE(Near(image.GetPixel(32, y), {255, 0, 0, 255}));
		}
	}
}

TEST_CASE("TextureReadback: rejects unsupported textures and verifies its arguments")
{
	if (!RequireTestGraphicsDevice())
		return;
	ValidationCheck validation;
	nvrhi::IDevice* device = GetTestGraphicsDevice()->GetDevice();

	Paralax::Testing::ScopedLogCapture capture;
	nvrhi::TextureHandle floatTarget = CreateRenderTarget(device, "FloatTarget", 8, 8, nvrhi::Format::RGBA16_FLOAT);
	REQUIRE(floatTarget);
	CHECK(ReadbackTexture(device, floatTarget).IsEmpty());
	CHECK(capture.Contains(spdlog::level::err, "unsupported format"));

	Paralax::Testing::ScopedAssertionCapture assertions;
	CHECK_THROWS_AS(ReadbackTexture(device, nullptr), Paralax::Testing::AssertionFailedError);
	CHECK_THROWS_AS(ReadbackTexture(nullptr, floatTarget), Paralax::Testing::AssertionFailedError);
}
