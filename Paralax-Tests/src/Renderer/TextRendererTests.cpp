#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Renderer/FontAtlas.h>
#include <Paralax/Renderer/Image.h>
#include <Paralax/Renderer/SceneRenderer.h>
#include <Paralax/Renderer/TextRenderer.h>
#include <Paralax/Renderer/TextureReadback.h>
#include <Paralax/Scene/ComponentRegistry.h>
#include <Paralax/Scene/Components.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/Scene.h>

#include <algorithm>
#include <limits>
#include <string>

using namespace Paralax;

namespace {

	struct RenderTarget
	{
		nvrhi::TextureHandle Texture;
		nvrhi::FramebufferHandle Framebuffer;
	};

	RenderTarget CreateOutput(nvrhi::IDevice* device, uint32_t width, uint32_t height)
	{
		nvrhi::TextureDesc desc;
		desc.width = width;
		desc.height = height;
		desc.format = nvrhi::Format::SRGBA8_UNORM;
		desc.isRenderTarget = true;
		desc.debugName = "TextTestOutput";
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		RenderTarget target;
		target.Texture = device->createTexture(desc);
		target.Framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target.Texture));
		return target;
	}

	// Bright, neutral pixels: text is drawn pure white over a blue sky and grey ground, which have no such pixels.
	bool IsWhite(glm::u8vec4 c)
	{
		return c.r > 200 && c.g > 200 && c.b > 200;
	}

	struct Box
	{
		uint32_t MinX = ~0u, MinY = ~0u, MaxX = 0, MaxY = 0, Count = 0;
	};

	Box WhiteBox(const Image& image, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
	{
		Box box;
		for (uint32_t y = y0; y < y1; y++)
			for (uint32_t x = x0; x < x1; x++)
				if (IsWhite(image.GetPixel(x, y)))
				{
					box.MinX = std::min(box.MinX, x);
					box.MinY = std::min(box.MinY, y);
					box.MaxX = std::max(box.MaxX, x);
					box.MaxY = std::max(box.MaxY, y);
					box.Count++;
				}
		return box;
	}

	Entity AddText(Scene& scene, const std::string& text, glm::vec3 position, float size, bool screenSpace, TextAlignment alignment = TextAlignment::Left)
	{
		Entity entity = scene.CreateEntity("Text");
		entity.GetComponent<TransformComponent>().Translation = position;
		auto& component = entity.AddComponent<TextComponent>();
		component.Text = text;
		component.Size = size;
		component.ScreenSpace = screenSpace;
		component.Alignment = alignment;
		return entity;
	}

	float Width(const FontAtlas& atlas, const std::string& text)
	{
		return atlas.Layout(text, TextAlignment::Left).Size.x;
	}

}

TEST_CASE("FontAtlas: the embedded font builds a deterministic atlas covering ASCII and Latin-1")
{
	Ref<FontAtlas> atlas = FontAtlas::CreateDefault();
	REQUIRE(atlas);
	Ref<FontAtlas> again = FontAtlas::CreateDefault();
	REQUIRE(again);
	CHECK(atlas->GetPixels() == again->GetPixels());
	CHECK(atlas->GetPixels().size() == FontAtlas::Width * FontAtlas::Height);
	CHECK(std::count_if(atlas->GetPixels().begin(), atlas->GetPixels().end(), [](uint8_t v) { return v > 128; }) > 10000); // Glyph interiors.
	CHECK(atlas->GetAscent() > 0.5f);
	CHECK(atlas->GetAscent() < 1.0f);

	// Every supported character has a glyph; none is drawn as a box.
	std::string all;
	for (uint32_t c = 0x21; c <= 0x7E; c++)
		all += static_cast<char>(c);
	TextLayout ascii = atlas->Layout(all, TextAlignment::Left);
	CHECK(ascii.Quads.size() == all.size());
	CHECK(ascii.Replacements == 0);
	TextLayout latin = atlas->Layout("\xC3\xA9\xC3\xB1\xC2\xA9\xC3\x9F", TextAlignment::Left); // é ñ © ß
	CHECK(latin.Quads.size() == 4);
	CHECK(latin.Replacements == 0);

	// Metrics make sense: a space advances without a quad, 'W' is wider than 'i', and lines stack one unit apart.
	CHECK(atlas->Layout(" ", TextAlignment::Left).Quads.empty());
	CHECK(Width(*atlas, "a b") > Width(*atlas, "ab"));
	CHECK(Width(*atlas, "W") > Width(*atlas, "i") * 1.5f);
	TextLayout lines = atlas->Layout("a\na\na", TextAlignment::Left);
	CHECK(lines.Lines == 3);
	CHECK(lines.Size.y == 3.0f);
	REQUIRE(lines.Quads.size() == 3);
	CHECK(static_cast<double>(lines.Quads[2].Min.y - lines.Quads[1].Min.y) == doctest::Approx(1.0));
	// Advances add up (Inter's kerning sits in GPOS extension lookups, which stb_truetype does not follow, so none
	// applies here).
	CHECK(static_cast<double>(Width(*atlas, "AV")) == doctest::Approx(static_cast<double>(Width(*atlas, "A") + Width(*atlas, "V"))));
}

TEST_CASE("FontAtlas: hostile text is laid out safely")
{
	Ref<FontAtlas> atlas = FontAtlas::CreateDefault();
	REQUIRE(atlas);

	// Characters outside the atlas and invalid UTF-8 become boxes, one per character or bad byte.
	TextLayout arrows = atlas->Layout("\xE2\x86\x92\xE2\x86\x92", TextAlignment::Left); // Two arrows (U+2192).
	CHECK(arrows.Quads.size() == 2);
	CHECK(arrows.Replacements == 2);
	TextLayout bad = atlas->Layout("a\xFF\xFE"
								   "b\xC3"
								   "c\xE2\x82"
								   "d\xED\xA0\x80"
								   "e",
								   TextAlignment::Left); // Stray, truncated, surrogate.
	CHECK(bad.Replacements >= 5);
	CHECK(bad.Quads.size() == 5 + bad.Replacements);
	TextLayout overlong = atlas->Layout("\xC0\xAF", TextAlignment::Left); // Overlong '/'.
	CHECK(overlong.Replacements == 2);
	TextLayout beyond = atlas->Layout("\xF4\x90\x80\x80", TextAlignment::Left); // Above U+10FFFF.
	CHECK(beyond.Replacements == 4);

	// Control characters other than newline and tab draw nothing; a tab is four spaces.
	CHECK(atlas->Layout(std::string("\x01\x02\x7F\x1B"), TextAlignment::Left).Quads.empty());
	CHECK(static_cast<double>(atlas->Layout("\t", TextAlignment::Left).Size.x) == doctest::Approx(4.0 * static_cast<double>(Width(*atlas, " "))));
	CHECK(atlas->Layout("", TextAlignment::Left).Quads.empty());
	CHECK(atlas->Layout("\n\n\n", TextAlignment::Left).Lines == 4);

	// Huge input is cut at MaxTextBytes.
	const std::string huge(100000, 'x');
	TextLayout cut = atlas->Layout(huge, TextAlignment::Left);
	CHECK(cut.Quads.size() == FontAtlas::MaxTextBytes);
	const std::string newlines(100000, '\n');
	CHECK(atlas->Layout(newlines, TextAlignment::Left).Lines == FontAtlas::MaxTextBytes + 1);
}

TEST_CASE("FontAtlas: alignment places shorter lines against the widest")
{
	Ref<FontAtlas> atlas = FontAtlas::CreateDefault();
	REQUIRE(atlas);
	const std::string text = "Wide line here\nI";
	const float widest = Width(*atlas, "Wide line here");
	auto lastQuad = [&](TextAlignment alignment) {
		TextLayout layout = atlas->Layout(text, alignment);
		CHECK(static_cast<double>(layout.Size.x) == doctest::Approx(static_cast<double>(widest)));
		return layout.Quads.back();
	};
	const GlyphQuad left = lastQuad(TextAlignment::Left);
	const GlyphQuad centre = lastQuad(TextAlignment::Center);
	const GlyphQuad right = lastQuad(TextAlignment::Right);
	CHECK(left.Min.x < centre.Min.x);
	CHECK(centre.Min.x < right.Min.x);
	CHECK(static_cast<double>(centre.Min.x - left.Min.x) == doctest::Approx(static_cast<double>(right.Min.x - centre.Min.x)).epsilon(1e-4));
	// Right-aligned, the short line is moved over by the difference in widths.
	CHECK(static_cast<double>(right.Min.x - left.Min.x) == doctest::Approx(static_cast<double>(widest - Width(*atlas, "I"))).epsilon(1e-4));
}

TEST_CASE("TextRenderer: a null device, a null command list and rendering before Initialize are asserted preconditions")
{
	Paralax::Testing::ScopedAssertionCapture capture;
	CHECK_THROWS_AS(TextRenderer(nullptr), Paralax::Testing::AssertionFailedError);

	if (!Paralax::Testing::RequireTestGraphicsDevice())
		return;
	GraphicsDevice& graphics = *Paralax::Testing::GetTestGraphicsDevice();
	const uint32_t initialErrors = graphics.GetValidationErrorCount();
	{
		TextRenderer renderer(graphics.GetDevice());
		CHECK_THROWS_AS(renderer.Initialize(nullptr), Paralax::Testing::AssertionFailedError);
		// Never initialised: rendering has no atlas to draw with. The verify comes before the command list is used, so
		// it is never opened (an opened list that is never submitted would keep its command buffer past the device).
		Scene scene("TextTest");
		RenderTarget target = CreateOutput(graphics.GetDevice(), 16, 16);
		nvrhi::CommandListHandle commands = graphics.GetDevice()->createCommandList();
		CHECK_THROWS_AS(renderer.Render(commands, scene, glm::mat4(1.0f), target.Framebuffer, nullptr), Paralax::Testing::AssertionFailedError);
	}
	CHECK(graphics.GetValidationErrorCount() == initialErrors);
}

TEST_CASE("TextRenderer: screen-space text is drawn where its anchor says, at any output size")
{
	if (!Paralax::Testing::RequireTestGraphicsDevice())
		return;
	GraphicsDevice& graphics = *Paralax::Testing::GetTestGraphicsDevice();
	const uint32_t initialErrors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("TextTest");
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		Entity hud = AddText(scene, "HUD 123", {0.05f, 0.05f, 0.0f}, 160.0f, true);
		Entity right = AddText(scene, "R", {0.95f, 0.05f, 0.0f}, 160.0f, true, TextAlignment::Right);

		Box boxes[2];
		for (int pass = 0; pass < 2; pass++)
		{
			const uint32_t width = pass == 0 ? 320 : 640;
			const uint32_t height = pass == 0 ? 180 : 360;
			RenderTarget output = CreateOutput(graphics.GetDevice(), width, height);
			const CameraView camera = CameraView::LookAt({0.0f, 2.0f, 4.5f}, {0.0f, 0.4f, 0.0f}, glm::radians(50.0f), float(width) / float(height));
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().TextGlyphs == 7);
			const Image image = ReadbackTexture(graphics.GetDevice(), output.Texture);
			REQUIRE(image.SavePNG(Paralax::Testing::GetTestOutputDirectory() / (pass == 0 ? "Text-Screen-320.png" : "Text-Screen-640.png")));
			// The HUD occupies the top-left; nothing is drawn in the bottom half.
			boxes[pass] = WhiteBox(image, 0, 0, width / 2, height / 2);
			CHECK(boxes[pass].Count > 40u * (pass + 1) * (pass + 1));
			CHECK(WhiteBox(image, 0, height / 2, width, height).Count == 0);
			// The right-aligned glyph ends near the right edge and starts after the HUD.
			const Box rightBox = WhiteBox(image, width / 2, 0, width, height / 2);
			CHECK(rightBox.Count > 0);
			CHECK(rightBox.MaxX > width * 9 / 10);
			CHECK(rightBox.MaxX < width * 96 / 100);
			// Rendering again gives the same image.
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(Image::MaxChannelDifference(image, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
		}
		// Twice the output: the text covers the same fraction of it, at the same normalised place.
		CHECK(boxes[1].MinX == doctest::Approx(boxes[0].MinX * 2).epsilon(0.08));
		CHECK(boxes[1].MaxX == doctest::Approx(boxes[0].MaxX * 2).epsilon(0.08));
		CHECK(boxes[1].MinY == doctest::Approx(boxes[0].MinY * 2).epsilon(0.1));
		CHECK(boxes[1].MaxY == doctest::Approx(boxes[0].MaxY * 2).epsilon(0.08));

		// Exposure does not touch text: a dark render keeps the HUD white.
		renderer.GetSettings().Exposure = 0.05f;
		RenderTarget output = CreateOutput(graphics.GetDevice(), 320, 180);
		renderer.Render(scene, CameraView::LookAt({0.0f, 2.0f, 4.5f}, {0.0f, 0.4f, 0.0f}, glm::radians(50.0f), 320.0f / 180.0f), output.Framebuffer);
		const Image dark = ReadbackTexture(graphics.GetDevice(), output.Texture);
		// Glyph interiors stay white; only edge pixels, blended with the now dark background, drop below the threshold.
		CHECK(WhiteBox(dark, 0, 0, 160, 90).Count > boxes[0].Count * 3 / 4);
		CHECK_FALSE(IsWhite(dark.GetPixel(160, 170)));

		// Empty text and text with no glyphs draw nothing; hostile text draws its boxes without trouble.
		hud.GetComponent<TextComponent>().Text = "";
		right.GetComponent<TextComponent>().Text = "\t\n";
		renderer.Render(scene, CameraView::LookAt({0.0f, 2.0f, 4.5f}, {0.0f, 0.4f, 0.0f}, glm::radians(50.0f), 320.0f / 180.0f), output.Framebuffer);
		CHECK(renderer.GetStats().TextGlyphs == 0);
		hud.GetComponent<TextComponent>().Text = std::string(100000, '\xFF');
		hud.GetComponent<TextComponent>().Color = glm::vec4(1e30f, -1.0f, std::numeric_limits<float>::quiet_NaN(), 2.0f);
		hud.GetComponent<TextComponent>().Size = std::numeric_limits<float>::infinity();
		renderer.Render(scene, CameraView::LookAt({0.0f, 2.0f, 4.5f}, {0.0f, 0.4f, 0.0f}, glm::radians(50.0f), 320.0f / 180.0f), output.Framebuffer);
		CHECK(renderer.GetStats().TextGlyphs == FontAtlas::MaxTextBytes);
	}
	CHECK(graphics.GetValidationErrorCount() == initialErrors);
}

TEST_CASE("TextRenderer: world-space text sits in the scene and is hidden by what is in front of it")
{
	if (!Paralax::Testing::RequireTestGraphicsDevice())
		return;
	GraphicsDevice& graphics = *Paralax::Testing::GetTestGraphicsDevice();
	const uint32_t initialErrors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("WorldTextTest");
		Entity sun = scene.CreateEntity("Sun");
		sun.AddComponent<DirectionalLightComponent>();
		// A label a metre wide, facing the camera, centred on the view.
		AddText(scene, "LABEL", {0.0f, 0.6f, 0.0f}, 0.5f, false, TextAlignment::Center);
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		RenderTarget output = CreateOutput(graphics.GetDevice(), 320, 180);
		const CameraView camera = CameraView::LookAt({0.0f, 0.4f, 4.0f}, {0.0f, 0.4f, 0.0f}, glm::radians(50.0f), 320.0f / 180.0f);

		renderer.Render(scene, camera, output.Framebuffer);
		const Image visible = ReadbackTexture(graphics.GetDevice(), output.Texture);
		REQUIRE(visible.SavePNG(Paralax::Testing::GetTestOutputDirectory() / "Text-World.png"));
		const Box label = WhiteBox(visible, 0, 0, 320, 180);
		CHECK(label.Count > 100);
		CHECK(label.MinX > 60);
		CHECK(label.MaxX < 260); // Centred on the view, about a third of its width.
		CHECK(label.MinY > 30);
		CHECK(label.MaxY < 110);

		// A wall between the camera and the label hides it (depth test), unlike screen-space text.
		Entity wall = scene.CreateEntity("Wall");
		wall.AddComponent<MeshComponent>().Mesh = AssetHandle(AssetManager::BuiltinCube);
		wall.GetComponent<TransformComponent>().Translation = {0.0f, 0.4f, 2.0f};
		wall.GetComponent<TransformComponent>().Scale = {3.0f, 3.0f, 0.1f};
		renderer.Render(scene, camera, output.Framebuffer);
		const Image hidden = ReadbackTexture(graphics.GetDevice(), output.Texture);
		REQUIRE(hidden.SavePNG(Paralax::Testing::GetTestOutputDirectory() / "Text-World-Hidden.png"));
		CHECK(WhiteBox(hidden, 0, 0, 320, 180).Count == 0);
		CHECK(renderer.GetStats().TextGlyphs == 5); // Still submitted; the depth test rejects it.

		for (float scale : {0.75f, 0.5f})
		{
			renderer.GetSettings().InternalRenderScale = scale;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().NativeDepthPasses == 1);
			CHECK(WhiteBox(ReadbackTexture(graphics.GetDevice(), output.Texture), 0, 0, 320, 180).Count == 0);
			wall.GetTransform().Translation.x = 10;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(WhiteBox(ReadbackTexture(graphics.GetDevice(), output.Texture), 0, 0, 320, 180).Count == label.Count);
			wall.GetTransform().Translation.x = 0;
		}
		renderer.GetSettings().AntiAliasing = AntiAliasingMode::Temporal;
		for (int frame = 0; frame < 3; ++frame)
		{
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().NativeDepthPasses == 1);
			CHECK(WhiteBox(ReadbackTexture(graphics.GetDevice(), output.Texture), 0, 0, 320, 180).Count == 0);
		}
		wall.GetTransform().Translation.x = 10;
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK(WhiteBox(ReadbackTexture(graphics.GetDevice(), output.Texture), 0, 0, 320, 180).Count > 100);
		renderer.GetSettings().AntiAliasing = AntiAliasingMode::Off;
		renderer.GetSettings().InternalRenderScale = 1;

		// Behind the camera's back it is not drawn at all.
		scene.DestroyEntity(wall);
		Entity behind = AddText(scene, "BEHIND", {0.0f, 0.6f, 8.0f}, 0.5f, false);
		(void)behind;
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK(WhiteBox(ReadbackTexture(graphics.GetDevice(), output.Texture), 0, 0, 320, 180).Count == label.Count);
	}
	CHECK(graphics.GetValidationErrorCount() == initialErrors);
}

TEST_CASE("Text: the component's text is limited in length through reflection")
{
	const ComponentInfo* info = ComponentRegistry::Find("Text");
	REQUIRE(info);
	TextComponent text;
	std::string error;
	CHECK(ComponentRegistry::SetField(*info->FindField("Text"), &text, std::string(4096, 'a'), error));
	CHECK(text.Text.size() == 4096);
	CHECK_FALSE(ComponentRegistry::SetField(*info->FindField("Text"), &text, std::string(4097, 'a'), error));
	CHECK(error.find("4096") != std::string::npos);
	CHECK(text.Text.size() == 4096);
	CHECK(ComponentRegistry::SetField(*info->FindField("Alignment"), &text, "Center", error));
	CHECK(text.Alignment == TextAlignment::Center);
	CHECK_FALSE(ComponentRegistry::SetField(*info->FindField("Alignment"), &text, "Justified", error));
}

TEST_CASE("TextRenderer: temporal and spatial AA keep moving UI native and outside history")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("AAUI");
		auto hud = AddText(scene, "HUD", {0.1f, 0.1f, 0}, 160, true);
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		auto& settings = renderer.GetSettings();
		settings.GIEnabled = settings.Shadows = false;
		settings.DefaultEnvironmentIntensity = 0;
		settings.InternalRenderScale = 0.75f;
		auto output = CreateOutput(graphics.GetDevice(), 160, 90);
		const auto camera = CameraView::LookAt({0, 0, 3}, {0, 0, 0}, glm::radians(60.0f), 160.0f / 90);
		renderer.Render(scene, camera, output.Framebuffer);
		const auto baseline = ReadbackTexture(graphics.GetDevice(), output.Texture);
		REQUIRE(WhiteBox(baseline, 0, 0, 160, 90).Count > 10);
		for (auto mode : {AntiAliasingMode::Temporal, AntiAliasingMode::Spatial})
		{
			hud.GetTransform().Translation = {0.1f, 0.1f, 0};
			settings.AntiAliasing = mode;
			for (int frame = 0; frame < 3; ++frame)
			{
				renderer.Render(scene, camera, output.Framebuffer);
				CHECK(Image::MaxChannelDifference(baseline, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
			}
			hud.GetTransform().Translation = {0.6f, 0.6f, 0};
			renderer.Render(scene, camera, output.Framebuffer);
			const auto moved = ReadbackTexture(graphics.GetDevice(), output.Texture);
			CHECK(WhiteBox(moved, 0, 0, 80, 45).Count == 0);
			CHECK(WhiteBox(moved, 80, 45, 160, 90).Count > 10);
			settings.AntiAliasing = AntiAliasingMode::Off;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(Image::MaxChannelDifference(moved, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}
