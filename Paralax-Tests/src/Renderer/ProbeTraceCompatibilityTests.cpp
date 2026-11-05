#include <doctest/doctest.h>

#include "ProbeTraceCompatibilityExpected.h"
#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/MaterialAsset.h>
#include <Paralax/Asset/MeshAsset.h>
#include <Paralax/Asset/TextureAsset.h>
#include <Paralax/Renderer/GraphicsDevice.h>
#include <Paralax/Renderer/ProbeTraceScene.h>
#include <Paralax/Scene/Entity.h>

#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

using namespace Paralax;

namespace {

	void Append(std::vector<uint32_t>& words, const glm::vec4& value)
	{
		for (int i = 0; i < 4; ++i)
			words.push_back(std::bit_cast<uint32_t>(value[i]));
	}

	void Append(std::vector<uint32_t>& words, const glm::uvec4& value)
	{
		for (int i = 0; i < 4; ++i)
			words.push_back(value[i]);
	}

	struct FieldWords
	{
		std::vector<uint32_t> Nodes, Triangles, Materials, Texels;
	};

	FieldWords ReadWords(const ProbeTraceScene& trace)
	{
		FieldWords result;
		for (const auto& node : trace.GetNodes())
		{
			Append(result.Nodes, node.Minimum);
			Append(result.Nodes, node.Maximum);
			Append(result.Nodes, node.Range);
		}
		for (const auto& triangle : trace.GetTriangles())
		{
			for (const auto& position : triangle.Position)
				Append(result.Triangles, position);
			for (const auto& normal : triangle.Normal)
				Append(result.Triangles, normal);
			Append(result.Triangles, triangle.UV01);
			Append(result.Triangles, triangle.UV2);
			Append(result.Triangles, triangle.Material);
		}
		for (const auto& material : trace.GetMaterials())
		{
			Append(result.Materials, material.BaseColor);
			Append(result.Materials, material.Emissive);
			Append(result.Materials, material.Parameters);
			for (const auto& texture : material.Textures)
				Append(result.Materials, texture);
		}
		for (const auto& texel : trace.GetTexels())
			Append(result.Texels, texel);
		return result;
	}

	void CheckWords(std::span<const uint32_t> actual, std::span<const uint32_t> expected)
	{
		REQUIRE(actual.size() == expected.size());
		for (size_t i = 0; i < expected.size(); ++i)
		{
			CAPTURE(i);
			CHECK(actual[i] == expected[i]);
		}
	}

	void CheckFrozen(const ProbeTraceScene& trace)
	{
		const auto words = ReadWords(trace);
		CheckWords(words.Nodes, ProbeTraceCompatibilityExpected::Nodes);
		CheckWords(words.Triangles, ProbeTraceCompatibilityExpected::Triangles);
		CheckWords(words.Materials, ProbeTraceCompatibilityExpected::Materials);
		CheckWords(words.Texels, ProbeTraceCompatibilityExpected::Texels);
	}

	Ref<TextureAsset> Texture(AssetManager& assets, uint64_t handle, std::span<const glm::vec4> pixels)
	{
		auto texture = CreateRef<TextureAsset>();
		texture->Handle = AssetHandle(handle);
		texture->Width = static_cast<uint32_t>(pixels.size());
		texture->Height = 1;
		texture->Format = TextureFormat::RGBA32F;
		texture->SRGB = false;
		texture->Mips.emplace_back(pixels.size_bytes());
		std::memcpy(texture->Mips[0].data(), pixels.data(), pixels.size_bytes());
		assets.AddMemoryAsset(texture);
		return texture;
	}

	struct CompatibilityFixture
	{
		AssetManager Assets;
		Scene World;
		Ref<MeshAsset> Mesh;
		Ref<MaterialAsset> Opaque, Empty, Masked, Blend, Invalid, HighUUID;

		CompatibilityFixture()
		{
			const std::array<glm::vec4, 2> colors{{{0.25f, 0.5f, 0.75f, 1}, {1, 0.125f, 0.25f, 0.5f}}};
			const std::array<glm::vec4, 1> emission{{{0.125f, 0.25f, 0.5f, 1}}};
			auto color = Texture(Assets, 72001, colors);
			auto emissive = Texture(Assets, 72002, emission);
			auto material = [&](uint64_t handle) {
				auto result = CreateRef<MaterialAsset>();
				result->Handle = AssetHandle(handle);
				Assets.AddMemoryAsset(result);
				return result;
			};
			Opaque = material(72003);
			Opaque->BaseColorFactor = {1, 0.5f, 0.25f, 1};
			Opaque->EmissiveFactor = {0.25f, 0.5f, 1};
			Opaque->MetallicFactor = 0.25f;
			Opaque->RoughnessFactor = 0.75f;
			Opaque->BaseColorTexture = Opaque->MetallicRoughnessTexture = color->Handle;
			Opaque->EmissiveTexture = emissive->Handle;
			Empty = material(72004);
			Empty->BaseColorFactor = {0.125f, 0.25f, 0.5f, 1};
			Empty->DoubleSided = true;
			Masked = material(72005);
			Masked->BaseColorFactor = {0.5f, 1, 0.25f, 0.75f};
			Masked->MetallicFactor = 0.5f;
			Masked->RoughnessFactor = 0.25f;
			Masked->AlphaCutoff = 0.25f;
			Masked->Alpha = AlphaMode::Mask;
			Masked->DoubleSided = true;
			Masked->BaseColorTexture = color->Handle;
			Blend = material(72006);
			Blend->Alpha = AlphaMode::Blend;
			Invalid = material(72007);
			Invalid->BaseColorFactor.x = std::numeric_limits<float>::quiet_NaN();
			HighUUID = material(72009);
			HighUUID->BaseColorFactor = {0.75f, 0.25f, 0.5f, 1};

			Mesh = CreateRef<MeshAsset>();
			Mesh->Handle = AssetHandle(72008);
			// A padding vertex makes omission of VertexOffset observable.
			Mesh->Vertices.emplace_back().Position = glm::vec3(100);
			for (uint32_t tag = 0; tag < 4; ++tag)
			{
				const float y = tag == 1 ? 2.0f : 0.0f;
				const std::array<glm::vec3, 3> positions{{{-1, y, 0}, {1, y, 0}, {0, y + 1, 0}}};
				for (uint32_t corner = 0; corner < 3; ++corner)
				{
					MeshVertex vertex;
					vertex.Position = positions[corner];
					vertex.Normal = glm::vec3(0);
					vertex.Normal[static_cast<int>(corner)] = 1;
					vertex.UV = {static_cast<float>(tag) / 8, static_cast<float>(corner) / 2};
					Mesh->Vertices.push_back(vertex);
					Mesh->Indices.push_back(tag * 3 + corner);
				}
			}
			Mesh->Submeshes.resize(5);
			Mesh->Submeshes[0].IndexOffset = 999; // Blended: excluded before index admission.
			Mesh->Submeshes[0].IndexCount = 3;
			Mesh->Submeshes[1].IndexCount = 9;
			Mesh->Submeshes[1].VertexOffset = 1;
			Mesh->Submeshes[1].VertexCount = 1; // Deliberately underspecified; actual indices remain valid.
			Mesh->Submeshes[1].MaterialIndex = 1;
			Mesh->Submeshes[2].IndexCount = 2; // Invalid transform also skips invalid mesh/material data.
			Mesh->Submeshes[2].MaterialIndex = 2;
			Mesh->Submeshes[2].Transform = glm::mat4(0);
			Mesh->Submeshes[3].IndexOffset = 12;
			Mesh->Submeshes[3].VertexOffset = std::numeric_limits<uint32_t>::max();
			Mesh->Submeshes[3].MaterialIndex = 3;
			Mesh->Submeshes[4].IndexOffset = 9;
			Mesh->Submeshes[4].IndexCount = 3;
			Mesh->Submeshes[4].VertexOffset = 1;
			Mesh->Submeshes[4].VertexCount = 0;
			Mesh->Submeshes[4].MaterialIndex = 1;
			Mesh->Materials = {Blend->Handle, Opaque->Handle, Invalid->Handle, Empty->Handle};
			Assets.AddMemoryAsset(Mesh);
			// Creation order differs from unsigned UUID order; the high bit detects signed sorting.
			Add(0x8000000000000001ULL, 0, false, false);
			Add(42, -4, true, true);
			Add(7, 4, false, true);
		}

		void Add(uint64_t uuid, float x, bool mirrored, bool castShadows)
		{
			auto entity = World.CreateEntityWithUUID(UUID(uuid), "compatibility surface");
			entity.GetTransform().Translation.x = x;
			entity.GetTransform().Scale.x = mirrored ? -1.0f : 1.0f;
			auto& component = entity.AddComponent<MeshComponent>();
			component.Mesh = Mesh->Handle;
			component.CastShadows = castShadows;
			if (mirrored)
				component.Materials = {AssetHandle(0), Masked->Handle};
			else if (uuid == 0x8000000000000001ULL)
				component.Materials = {AssetHandle(0), HighUUID->Handle};
		}
	};

}

TEST_CASE("ProbeTraceCompatibility: pre-extraction field words preserve legacy ordering and packing")
{
	CompatibilityFixture fixture;
	ProbeTraceScene trace;
	bool changed = false;
	REQUIRE(trace.Update(fixture.World, fixture.Assets, changed));
	CHECK(changed);
	CheckFrozen(trace);
	REQUIRE(trace.GetTriangles().size() == 12);
	// Original ordinals 1 and 4 survive gaps 0/2/3; UV tags identify their triangles after sorting.
	for (uint32_t i = 0; i < 12; ++i)
		CHECK(trace.GetTriangles()[i].UV01.x == static_cast<float>(i % 4) / 8);
	CHECK(trace.GetTriangles()[0].Position[0].x == -3);
	CHECK(trace.GetTriangles()[4].Position[0].x == -1);
	CHECK(trace.GetTriangles()[8].Position[0].x == 3);
	CHECK(trace.GetMaterials().size() == 4); // Packed material 1 belongs only to empty submeshes.
	const auto* triangles = trace.GetTriangles().data();
	const auto identity = trace.GetSceneIdentity();
	REQUIRE(trace.Update(fixture.World, fixture.Assets, changed));
	CHECK_FALSE(changed);
	CHECK(trace.GetTriangles().data() == triangles);
	CHECK(trace.GetSceneIdentity() == identity);
	CheckFrozen(trace);
}

TEST_CASE("ProbeTraceCompatibility: rejected updates retain all frozen fields and recover by cause")
{
	CompatibilityFixture fixture;
	ProbeTraceScene trace;
	bool changed = false;
	REQUIRE(trace.Update(fixture.World, fixture.Assets, changed));
	Testing::ScopedLogCapture logs;
	// Range checks precede material checks, and material checks precede individual source vertices.
	fixture.Mesh->Submeshes[1].IndexCount = 10;
	fixture.Opaque->BaseColorFactor.x = std::numeric_limits<float>::quiet_NaN();
	fixture.Mesh->Vertices[1].Position.x = std::numeric_limits<float>::quiet_NaN();
	++fixture.Mesh->Version;
	++fixture.Opaque->Version;
	REQUIRE_FALSE(trace.Update(fixture.World, fixture.Assets, changed));
	CHECK_FALSE(changed);
	CHECK(logs.Count(spdlog::level::err, "submesh indices") == 1);
	CheckFrozen(trace);
	fixture.Mesh->Submeshes[1].IndexCount = 9;
	++fixture.Mesh->Version;
	REQUIRE_FALSE(trace.Update(fixture.World, fixture.Assets, changed));
	CHECK(logs.Count(spdlog::level::err, "nonfinite or excessive material values") == 1);
	CheckFrozen(trace);
	fixture.Opaque->BaseColorFactor.x = 1;
	++fixture.Opaque->Version;
	REQUIRE_FALSE(trace.Update(fixture.World, fixture.Assets, changed));
	CHECK(logs.Count(spdlog::level::err, "invalid source vertex") == 1);
	CheckFrozen(trace);
	REQUIRE_FALSE(trace.Update(fixture.World, fixture.Assets, changed));
	CHECK(logs.Count(spdlog::level::err, "invalid source vertex") == 1);
	fixture.Mesh->Vertices[1].Position.x = -1;
	++fixture.Mesh->Version;
	REQUIRE(trace.Update(fixture.World, fixture.Assets, changed));
	CHECK(changed);
	CheckFrozen(trace);
}

TEST_CASE("ProbeTraceCompatibility: empty snapshots and empty submesh materials keep admission limits")
{
	AssetManager assets;
	Scene scene;
	ProbeTraceScene trace;
	bool changed = false;
	CHECK(trace.GetTexels().empty());
	REQUIRE(trace.Update(scene, assets, changed));
	CHECK(changed);
	CHECK(trace.GetNodes().empty());
	CHECK(trace.GetTriangles().empty());
	CHECK(trace.GetMaterials().empty());
	const std::array<uint32_t, 4> white{0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u};
	CheckWords(ReadWords(trace).Texels, white);
	REQUIRE(trace.Update(scene, assets, changed));
	CHECK_FALSE(changed);

	auto mesh = CreateRef<MeshAsset>();
	mesh->Handle = AssetHandle(72101);
	mesh->Submeshes.emplace_back().VertexCount = 999;
	assets.AddMemoryAsset(mesh);
	auto material = CreateRef<MaterialAsset>();
	material->Handle = AssetHandle(72102);
	assets.AddMemoryAsset(material);
	auto entity = scene.CreateEntityWithUUID(UUID(1), "empty material user");
	auto& component = entity.AddComponent<MeshComponent>();
	component.Mesh = mesh->Handle;
	component.Materials = {material->Handle};
	REQUIRE(trace.Update(scene, assets, changed));
	CHECK(trace.GetTriangles().empty());
	const std::array<uint32_t, 24> defaultMaterial{
		0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u, 0, 0, 0, 0,
		0, 0x3f000000u, 0x3f000000u, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0};
	CheckWords(ReadWords(trace).Materials, defaultMaterial);
	material->RoughnessFactor = std::numeric_limits<float>::infinity();
	++material->Version;
	Testing::ScopedLogCapture logs;
	REQUIRE_FALSE(trace.Update(scene, assets, changed));
	CHECK_FALSE(changed);
	CheckWords(ReadWords(trace).Materials, defaultMaterial);
	CHECK(logs.Count(spdlog::level::err, "nonfinite or excessive material values") == 1);
	GraphicsDeviceLimits limits;
	limits.MaxStorageBufferBytes = sizeof(GPU::ProbeTraceTriangle) - 1;
	ProbeTraceScene restricted(limits);
	REQUIRE_FALSE(restricted.Update(scene, assets, changed));
	CHECK(logs.Count(spdlog::level::err, "ProbeTraceTriangles placeholder") == 1);
}

TEST_CASE("ProbeTraceCompatibility: nonaffine bottom rows retain position w and node bounds")
{
	AssetManager assets;
	Scene scene;
	auto mesh = CreateRef<MeshAsset>();
	mesh->Handle = AssetHandle(72201);
	mesh->Vertices.resize(3);
	mesh->Vertices[1].Position = {2, 0, 0};
	mesh->Vertices[2].Position = {0, 2, 0};
	mesh->Indices = {0, 1, 2};
	auto& submesh = mesh->Submeshes.emplace_back();
	submesh.IndexCount = 3;
	submesh.VertexCount = 1;
	submesh.Transform[0][3] = 0.25f;
	assets.AddMemoryAsset(mesh);
	auto entity = scene.CreateEntityWithUUID(UUID(1), "nonaffine surface");
	entity.AddComponent<MeshComponent>().Mesh = mesh->Handle;
	ProbeTraceScene trace;
	bool changed = false;
	REQUIRE(trace.Update(scene, assets, changed));
	const std::array<uint32_t, 12> node{0, 0, 0, 0x3f800000u, 0x40000000u, 0x40000000u, 0, 0x3fc00000u, 0, 1, 1, 0};
	const std::array<uint32_t, 36> triangle{
		0, 0, 0, 0x3f800000u, 0x40000000u, 0, 0, 0x3fc00000u, 0, 0x40000000u, 0, 0x3f800000u,
		0, 0x3f800000u, 0, 0, 0, 0x3f800000u, 0, 0, 0, 0x3f800000u, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	const auto words = ReadWords(trace);
	CheckWords(words.Nodes, node);
	CheckWords(words.Triangles, triangle);
}
