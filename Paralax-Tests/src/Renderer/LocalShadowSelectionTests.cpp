#include <doctest/doctest.h>

#include <Paralax/Renderer/LocalShadowSelection.h>

#include <algorithm>
#include <limits>

using namespace Paralax;

TEST_CASE("LocalShadowSelection: layer budgets respect settings and adapter limits without overflow")
{
	CHECK(LocalShadowLayerCapacity(512, 16, 256) == 96);
	CHECK(LocalShadowLayerCapacity(1024, 16, 256) == 64);
	CHECK(LocalShadowLayerCapacity(2048, 16, 256) == 16);
	CHECK(LocalShadowLayerCapacity(512, 16, 5) == 5);
	CHECK(LocalShadowLayerCapacity(1024, 4, 256) == 24);
	CHECK(LocalShadowLayerCapacity(1024, 0, 256) == 0);
	CHECK(LocalShadowLayerCapacity(UINT32_MAX, 16, 256) == 0);
	CHECK(LocalShadowLayerCapacity(512, UINT32_MAX, 256) == 0);
}

TEST_CASE("LocalShadowSelection: creation order does not influence ties or direct-light truncation")
{
	std::vector<LocalShadowCandidate> lights;
	for (uint64_t id = 1; id <= 300; ++id)
		lights.push_back({id, {0, 1, 0}, 4, 6, true, true});
	for (bool reverse : {false, true})
	{
		if (reverse)
			std::reverse(lights.begin(), lights.end());
		const auto selection = SelectLocalShadows(lights, {}, 1, 6);
		REQUIRE(selection.DirectLights.size() == 256);
		REQUIRE(selection.Shadows.size() == 1);
		CHECK(lights[selection.Shadows[0].Candidate].ID == 1);
		CHECK(lights[selection.DirectLights.back()].ID == 256);
	}
}

TEST_CASE("LocalShadowSelection: insufficient layers skip a point and still admit a later spot")
{
	const std::vector<LocalShadowCandidate> lights{{1, {0, 1, 0}, 3, 6, true, true}, {2, {0, 2, 0}, 3, 1, true, true}};
	const auto selection = SelectLocalShadows(lights, {}, 16, 5);
	REQUIRE(selection.Shadows.size() == 1);
	CHECK(selection.Shadows[0].Candidate == 1);
	CHECK(selection.Shadows[0].FirstLayer == 0);
	CHECK(selection.LayersUsed == 1);
	CHECK(selection.Requested == 2);
}

TEST_CASE("LocalShadowSelection: explicit view history retains within the ratio and resets with scene identity")
{
	std::vector<LocalShadowCandidate> lights{{1, {0, 10, 0}, 20, 6, true, true}, {2, {0, 11, 0}, 20, 6, true, true}};
	LocalShadowHistory history;
	Ref<const void> scene = CreateRef<int>(1);
	auto choose = [&] {
		const auto result = SelectLocalShadows(lights, {}, 1, 6, scene, &history);
		REQUIRE(result.Shadows.size() == 1);
		return lights[result.Shadows[0].Candidate].ID;
	};
	CHECK(choose() == 1);
	// Both range-distance keys are zero: retention must compare the ordinary distance key next.
	lights[1].Position.y = 8.1f;
	CHECK(choose() == 1);
	const auto capture = SelectLocalShadows(lights, {}, 1, 6);
	CHECK(lights[capture.Shadows[0].Candidate].ID == 2);
	lights[1].Position.y = 7.9f;
	CHECK(choose() == 2);
	lights[1].Position.y = 11;
	CHECK(choose() == 2);
	scene = CreateRef<int>(2);
	CHECK(choose() == 1);
	lights[0].Visible = false;
	CHECK(choose() == 2);
	CHECK(history.Lights.size() == 1);
}

TEST_CASE("LocalShadowSelection: invalid user values and disabled lights never consume layers")
{
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	for (float range : {0.0f, -1.0f, 1e30f, nan, inf})
	{
		std::vector<LocalShadowCandidate> lights{{1, {}, range, 6, true, true}};
		CHECK(SelectLocalShadows(lights, {}, 16, 96).DirectLights.empty());
	}
	std::vector<LocalShadowCandidate> lights{{1, {nan, 0, 0}, 10, 6, true, true}, {2, {}, 10, 6, false, true}};
	const auto result = SelectLocalShadows(lights, {}, 16, 96);
	CHECK(result.DirectLights.size() == 1);
	CHECK(result.Shadows.empty());
	CHECK(SelectLocalShadows(lights, {inf, 0, 0}, 16, 96).DirectLights.empty());
}

TEST_CASE("LocalShadowSelection: small ranges and offscreen lights still feed direct lighting and GI")
{
	const std::vector<LocalShadowCandidate> lights{{1, {}, 0.025f, 6, true, true}, {2, {}, 10, 6, true, false}};
	const auto result = SelectLocalShadows(lights, {}, 16, 96);
	CHECK(result.DirectLights.size() == 2);
	CHECK(result.Shadows.empty());
}

TEST_CASE("LocalShadowSelection: point and spot on one entity have independent stable identities")
{
	std::vector<LocalShadowCandidate> lights{{1, {}, 10, 1, true, true, 1}, {1, {}, 10, 6, true, true, 0}};
	LocalShadowHistory history;
	const auto result = SelectLocalShadows(lights, {}, 1, 6, {}, &history);
	REQUIRE(result.Shadows.size() == 1);
	CHECK(result.Shadows[0].Candidate == 1);
	REQUIRE(history.Lights.size() == 1);
	CHECK(history.Lights[0].Kind == 0);
	CHECK(result.DirectLights[0] == 1);
}
