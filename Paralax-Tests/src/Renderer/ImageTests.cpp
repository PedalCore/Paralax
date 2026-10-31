#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Renderer/Image.h>

using namespace Paralax;

TEST_CASE("Image: pixels round-trip through SetPixel/GetPixel and PNG files")
{
	Image image(4, 3);
	CHECK(image.GetPixels().size() == 4 * 3 * 4);
	image.SetPixel(0, 0, {255, 0, 0, 255});
	image.SetPixel(3, 2, {1, 2, 3, 4});
	CHECK(image.GetPixel(3, 2) == glm::u8vec4(1, 2, 3, 4));

	const std::filesystem::path path = Paralax::Testing::CreateTestDirectory("ImagePNG") / "nested" / "image.png";
	REQUIRE(image.SavePNG(path));

	Image loaded = Image::LoadFromFile(path);
	REQUIRE(loaded.GetWidth() == 4);
	REQUIRE(loaded.GetHeight() == 3);
	CHECK(Image::MaxChannelDifference(image, loaded) == 0);
}

TEST_CASE("Image: MaxChannelDifference measures the largest channel delta and flags size mismatches")
{
	Image a(2, 2), b(2, 2);
	CHECK(Image::MaxChannelDifference(a, b) == 0);
	b.SetPixel(1, 1, {0, 9, 0, 0});
	CHECK(Image::MaxChannelDifference(a, b) == 9);
	CHECK(Image::MaxChannelDifference(a, Image(3, 2)) == 256);
}

TEST_CASE("Image: invalid operations fail safely")
{
	Paralax::Testing::ScopedLogCapture capture;
	CHECK_FALSE(Image().SavePNG(Paralax::Testing::CreateTestDirectory("ImageEmpty") / "empty.png"));
	CHECK(Image::LoadFromFile("does/not/exist.png").IsEmpty());
	CHECK(capture.Contains(spdlog::level::err, "Failed to load image"));

	Paralax::Testing::ScopedAssertionCapture assertCapture;
	Image image(2, 2);
	CHECK_THROWS_AS(image.GetPixel(2, 0), Paralax::Testing::AssertionFailedError);
	CHECK_THROWS_AS(image.SetPixel(0, 2, {}), Paralax::Testing::AssertionFailedError);
}
