#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Scene/ComponentRegistry.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/Scene.h>
#include <Paralax/Scene/SceneSerializer.h>
#include <Paralax/Scripting/ScriptSystem.h>

#include <regex>
#include <set>

// AGENTS.md: every component and every scripting API function must appear in the feature test scene. These tests
// enforce it, so the rule cannot be forgotten: they fail when something new is added without extending the scene.

using namespace Paralax;

namespace {

	std::filesystem::path RepositoryRoot()
	{
		// This file lives at <root>/Paralax-Tests/src/Scripting/.
		return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
	}

	std::filesystem::path FeatureTestDirectory()
	{
		return RepositoryRoot() / "Paralax-Tests" / "FeatureTest";
	}

	// The live scripting API, as enumerated from the bindings by the script system itself.
	std::vector<std::string> EnumerateScriptApi()
	{
		AssetManager assets;
		ScriptSystem scripts(assets);
		return scripts.GetApiNames();
	}

	// True if the member name is used in the source as a whole word (e.g. "GetComponent" or ".x").
	bool MentionsMember(const std::string& source, const std::string& name)
	{
		const std::string member = name.substr(name.find_first_of(".:") + 1);
		return std::regex_search(source, std::regex("(^|[^A-Za-z0-9_])" + member + "([^A-Za-z0-9_]|$)"));
	}

}

TEST_CASE("FeatureTest: the scene contains every registered component")
{
	Ref<Scene> scene = SceneSerializer::LoadFromFile(FeatureTestDirectory() / "Assets" / "Scenes" / "FeatureTest.pxscene");
	REQUIRE(scene);
	std::set<std::string> present;
	scene->ForEachEntity([&](Entity entity) {
		for (const ComponentInfo& info : ComponentRegistry::GetAll())
		{
			if (info.Has(scene->GetRegistry(), entity.GetHandle()))
				present.insert(info.Name);
		}
	});
	for (const ComponentInfo& info : ComponentRegistry::GetAll())
	{
		CAPTURE(info.Name);
		CHECK_MESSAGE(present.contains(info.Name),
					  "Add the component to the feature test scene (tools/featuretest/build_feature_test.py) and to the list in FeatureTest.lua");
	}
}

TEST_CASE("FeatureTest: the feature script and the documentation cover the whole scripting API")
{
	const std::string script = Paralax::Testing::ReadFileText(FeatureTestDirectory() / "Assets" / "Scripts" / "FeatureTest.lua");
	const std::string documentation = Paralax::Testing::ReadFileText(RepositoryRoot() / "docs" / "Scripting.md");
	REQUIRE_FALSE(script.empty());
	REQUIRE_FALSE(documentation.empty());

	const std::vector<std::string> api = EnumerateScriptApi();
	CHECK(api.size() > 60); // Sanity: the enumeration found the bindings.
	for (const std::string& name : api)
	{
		CAPTURE(name);
		CHECK_MESSAGE(MentionsMember(script, name), "Exercise it in Paralax-Tests/FeatureTest/Assets/Scripts/FeatureTest.lua");
		CHECK_MESSAGE(MentionsMember(documentation, name), "Document it in docs/Scripting.md");
	}
}
