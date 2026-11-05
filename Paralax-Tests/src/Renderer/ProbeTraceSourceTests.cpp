#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/MaterialAsset.h>
#include <Paralax/Asset/MeshAsset.h>
#include <Paralax/Asset/TextureAsset.h>
#include <Paralax/Renderer/GraphicsDevice.h>
#include <Paralax/Renderer/ProbeTraceScene.h>
#include <Paralax/Renderer/ProbeTraceSource.h>
#include <Paralax/Scene/Entity.h>

#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

using namespace Paralax;

namespace {

	template<typename T>
	constexpr bool NoncopyableView = !std::is_copy_constructible_v<T> && !std::is_copy_assignable_v<T> &&
									 !std::is_move_constructible_v<T> && !std::is_move_assignable_v<T>;
	static_assert(NoncopyableView<ProbeTraceSourceGeometry> && NoncopyableView<ProbeTraceSourceSnapshot> && NoncopyableView<ProbeTraceLegacyRepresentation>);
	static_assert(std::is_same_v<decltype(ProbeTraceSourceInstance::GeometryIndex), size_t>);
	static_assert(std::is_same_v<decltype(ProbeTraceSourceInstance::OriginalSubmeshOrdinal), uint64_t>);
	static_assert(std::is_same_v<decltype(std::declval<const ProbeTraceSourceSnapshot&>().GetGeometryCount()), size_t>);
	static_assert(std::is_same_v<decltype(&ProbeTraceSourceSnapshot::GetGeometry), const ProbeTraceSourceGeometry& (ProbeTraceSourceSnapshot::*)(size_t) const>);
	static_assert(std::is_same_v<decltype(std::declval<const ProbeTraceSourceGeometry&>().GetOriginalSubmeshOrdinal()), uint64_t>);

	struct SourceFixture
	{
		Scope<AssetManager> Assets = CreateScope<AssetManager>();
		Scene World;
		Ref<MeshAsset> Mesh = CreateRef<MeshAsset>();
		Ref<MaterialAsset> Opaque = CreateRef<MaterialAsset>(), Empty = CreateRef<MaterialAsset>(), Masked = CreateRef<MaterialAsset>(), Blend = CreateRef<MaterialAsset>();
		Ref<TextureAsset> Color = CreateRef<TextureAsset>();
		std::array<Entity, 3> Entities;

		void RebuildAssetManager()
		{
			Assets = CreateScope<AssetManager>();
			for (const Ref<Asset>& asset : {Ref<Asset>(Mesh), Ref<Asset>(Opaque), Ref<Asset>(Empty), Ref<Asset>(Masked), Ref<Asset>(Blend), Ref<Asset>(Color)})
				if (asset)
					Assets->AddMemoryAsset(asset);
		}

		SourceFixture()
		{
			Color->Handle = AssetHandle(73001);
			Color->Width = Color->Height = 1;
			Color->Format = TextureFormat::RGBA32F;
			Color->SRGB = false;
			Color->Mips.emplace_back(sizeof(glm::vec4));
			const glm::vec4 pixel(0.25f, 0.5f, 0.75f, 1);
			std::memcpy(Color->Mips[0].data(), &pixel, sizeof(pixel));
			Assets->AddMemoryAsset(Color);
			Opaque->Handle = AssetHandle(73002);
			Opaque->BaseColorFactor = {1, 0.5f, 0.25f, 1};
			Opaque->BaseColorTexture = Color->Handle;
			Empty->Handle = AssetHandle(73003);
			Empty->EmissiveFactor = {0.25f, 0.5f, 1};
			Masked->Handle = AssetHandle(73004);
			Masked->Alpha = AlphaMode::Mask;
			Masked->BaseColorTexture = Color->Handle;
			Blend->Handle = AssetHandle(73005);
			Blend->Alpha = AlphaMode::Blend;
			for (const auto& material : {Opaque, Empty, Masked, Blend})
				Assets->AddMemoryAsset(material);
			Mesh->Handle = AssetHandle(73006);
			Mesh->Vertices.resize(5);
			Mesh->Vertices[0].Position = glm::vec3(99);
			const std::array<glm::vec3, 4> positions{{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}, {2, 2, 0}}};
			for (size_t i = 0; i < positions.size(); ++i)
			{
				Mesh->Vertices[i + 1].Position = positions[i];
				Mesh->Vertices[i + 1].Normal = {0, 0, 1};
				Mesh->Vertices[i + 1].UV = glm::vec2(positions[i]) * 0.5f;
			}
			Mesh->Indices = {0, 1, 2, 1, 3, 2};
			Mesh->Submeshes.resize(4);
			Mesh->Submeshes[0].IndexCount = 999; // Excluded blend keeps the original ordinal gap.
			Mesh->Submeshes[1].IndexCount = 6;
			Mesh->Submeshes[1].VertexOffset = 1;
			Mesh->Submeshes[1].VertexCount = 1;
			Mesh->Submeshes[1].MaterialIndex = 1;
			Mesh->Submeshes[2].IndexOffset = 6;
			Mesh->Submeshes[2].MaterialIndex = 2;
			Mesh->Submeshes[3].IndexCount = 3;
			Mesh->Submeshes[3].VertexOffset = 1;
			Mesh->Submeshes[3].MaterialIndex = 1;
			Mesh->Materials = {Blend->Handle, Opaque->Handle, Empty->Handle};
			Assets->AddMemoryAsset(Mesh);
			for (size_t i = 0; i < Entities.size(); ++i)
			{
				Entities[i] = World.CreateEntityWithUUID(UUID(i + 1), "source instance");
				Entities[i].GetTransform().Translation.x = static_cast<float>(i) * 4;
				auto& component = Entities[i].AddComponent<MeshComponent>();
				component.Mesh = Mesh->Handle;
				if (i == 1)
				{
					Entities[i].GetTransform().Scale.x = -1;
					component.Materials = {AssetHandle(0), Masked->Handle};
				}
			}
		}
	};

	std::vector<uint32_t> TriangleWords(const std::vector<GPU::ProbeTraceTriangle>& triangles)
	{
		std::vector<uint32_t> result;
		auto append = [&](const glm::vec4& value) {
			for (int i = 0; i < 4; ++i)
				result.push_back(std::bit_cast<uint32_t>(value[i]));
		};
		for (const auto& triangle : triangles)
		{
			for (const auto& position : triangle.Position)
				append(position);
			for (const auto& normal : triangle.Normal)
				append(normal);
			append(triangle.UV01);
			append(triangle.UV2);
			for (int i = 0; i < 4; ++i)
				result.push_back(triangle.Material[i]);
		}
		return result;
	}

	std::array<uintptr_t, 3> GeometryAddresses(const ProbeTraceSourceSnapshot& snapshot)
	{
		REQUIRE(snapshot.GetGeometryCount() == 3);
		return {reinterpret_cast<uintptr_t>(&snapshot.GetGeometry(0)), reinterpret_cast<uintptr_t>(&snapshot.GetGeometry(1)), reinterpret_cast<uintptr_t>(&snapshot.GetGeometry(2))};
	}

}

TEST_CASE("ProbeTraceSource: captures unique geometry and canonical identities without legacy expansion")
{
	SourceFixture fixture;
	ProbeTraceSource source;
	CHECK(source.GetCurrentSnapshot() == nullptr);
	CHECK(source.GetCurrentLegacy() == nullptr);
	CHECK_FALSE(source.MaterializeLegacy());
	bool changed = false;
	REQUIRE(source.Update(fixture.World, *fixture.Assets, changed));
	CHECK(changed);
	const auto* snapshot = source.GetCurrentSnapshot();
	REQUIRE(snapshot);
	CHECK(snapshot->GetGeneration() == 1);
	CHECK(snapshot->GetExpandedTriangleCount() == 9);
	REQUIRE(snapshot->GetGeometryCount() == 3);
	REQUIRE(snapshot->GetInstances().size() == 9);
	const std::array<uint64_t, 3> ordinals{1, 2, 3};
	const std::array<uint32_t, 3> ranks{0, 2, 2};
	for (size_t i = 0; i < 9; ++i)
	{
		const auto& instance = snapshot->GetInstances()[i];
		CHECK(instance.EntityUUID == i / 3 + 1);
		CHECK(instance.OriginalSubmeshOrdinal == ordinals[i % 3]);
		CHECK(instance.GeometryIndex == i % 3);
		CHECK(instance.FirstCanonicalRank == i / 3 * 3 + ranks[i % 3]);
		CHECK(instance.Mirrored == (i / 3 == 1));
	}
	const auto& geometry = snapshot->GetGeometry(0);
	CHECK(geometry.GetMeshHandle() == uint64_t(fixture.Mesh->Handle));
	CHECK(geometry.GetMeshVersion() == fixture.Mesh->Version);
	CHECK(geometry.GetIndexOffset() == 0);
	CHECK(geometry.GetIndexCount() == 6);
	CHECK(geometry.GetVertexOffset() == 1);
	CHECK(geometry.GetVertexCount() == 1);
	REQUIRE(geometry.GetTriangles().size() == 2);
	CHECK(geometry.GetTriangles()[0].OriginalTriangleOrdinal == 0);
	CHECK(geometry.GetTriangles()[1].OriginalTriangleOrdinal == 1);
	CHECK(geometry.GetTriangles()[0].Position[1] == glm::vec3(2, 0, 0));
	CHECK(geometry.GetTriangles()[0].UV[1] == glm::vec2(1, 0));
	const auto first = source.GetStatistics();
	CHECK(first.UniqueGeometryCopies == 3);
	CHECK(first.CopiedLocalTriangles == 3);
	CHECK(first.ReusedGeometryReferences == 6);
	CHECK(first.ValidationVertexVisits == 27);
	CHECK(first.LegacyExpandedTriangles == 0);
	CHECK(first.LegacySortCalls == 0);
	CHECK(first.LegacyBuiltNodes == 0);
	CHECK(source.GetCurrentLegacy() == nullptr);
	const auto addresses = GeometryAddresses(*snapshot);
	REQUIRE(source.Update(fixture.World, *fixture.Assets, changed));
	CHECK_FALSE(changed);
	REQUIRE(source.GetCurrentSnapshot());
	CHECK(GeometryAddresses(*source.GetCurrentSnapshot()) == addresses);
	const auto cached = source.GetStatistics();
	CHECK(cached.AcceptedChanges == 1);
	CHECK(cached.AcceptedCacheHits == 1);
	CHECK(cached.ValidationVertexVisits == first.ValidationVertexVisits);
	CHECK(cached.UniqueGeometryCopies == first.UniqueGeometryCopies);
	CHECK(cached.LegacyExpandedTriangles == 0);
	Testing::ScopedAssertionCapture assertions;
	CHECK_THROWS_AS(source.GetCurrentSnapshot()->GetGeometry(3), Testing::AssertionFailedError);
}

TEST_CASE("ProbeTraceSource: materializes only captured values after original assets change and release")
{
	SourceFixture fixture;
	ProbeTraceSource source;
	ProbeTraceScene eager;
	bool changed = false;
	REQUIRE(source.Update(fixture.World, *fixture.Assets, changed));
	REQUIRE(eager.Update(fixture.World, *fixture.Assets, changed));
	const auto expected = TriangleWords(eager.GetTriangles());
	const auto* snapshot = source.GetCurrentSnapshot();
	REQUIRE(snapshot);
	const auto* positions = snapshot->GetGeometry(0).GetTriangles().data();
	fixture.Mesh->Vertices[1].Position = glm::vec3(99);
	fixture.Mesh->Indices.clear();
	fixture.Mesh->Submeshes.clear();
	fixture.Opaque->BaseColorFactor = glm::vec4(0);
	fixture.Empty->EmissiveFactor = glm::vec3(99);
	fixture.Color->Mips[0].assign(sizeof(glm::vec4), 0);
	fixture.Assets.reset();
	fixture.Mesh.reset();
	fixture.Opaque.reset();
	fixture.Color.reset();
	REQUIRE(source.MaterializeLegacy());
	CHECK(source.GetCurrentSnapshot() == snapshot);
	CHECK(snapshot->GetGeometry(0).GetTriangles().data() == positions);
	CHECK(snapshot->GetGeometry(0).GetTriangles()[0].Position[0] == glm::vec3(0));
	CHECK(snapshot->GetMaterials()[0].BaseColor == glm::vec4(1, 0.5f, 0.25f, 1));
	CHECK(snapshot->GetTexels()[1] == glm::vec4(0.25f, 0.5f, 0.75f, 1));
	CHECK(snapshot->GetGeneration() == 1);
	const auto* legacy = source.GetCurrentLegacy();
	REQUIRE(legacy);
	CHECK(legacy->GetGeneration() == snapshot->GetGeneration());
	CHECK(TriangleWords(legacy->GetTriangles()) == expected);
	CHECK(legacy->GetNodes()[0].Minimum == glm::vec4(0, 0, 0, 1));
	CHECK(legacy->GetNodes()[0].Maximum == glm::vec4(10, 2, 0, 1));
	CHECK(legacy->GetNodes()[0].Range.z == legacy->GetNodes().size());
	const auto built = source.GetStatistics();
	CHECK(built.LegacyExpandedTriangles == 9);
	CHECK(built.LegacySortCalls == 2);
	CHECK(built.LegacyBuiltNodes == 5);
	REQUIRE(source.MaterializeLegacy());
	CHECK(source.GetCurrentLegacy() == legacy);
	CHECK(source.GetStatistics().LegacyExpandedTriangles == built.LegacyExpandedTriangles);
	CHECK(source.GetStatistics().LegacySortCalls == built.LegacySortCalls);
	CHECK(source.GetStatistics().AcceptedChanges == 1);
}

TEST_CASE("ProbeTraceSource: transform material and shadow edits reuse geometry while version changes replace it")
{
	SourceFixture fixture;
	ProbeTraceSource source;
	bool changed = false;
	REQUIRE(source.Update(fixture.World, *fixture.Assets, changed));
	const auto original = GeometryAddresses(*source.GetCurrentSnapshot());
	fixture.Entities[0].GetTransform().Translation.x = -4;
	REQUIRE(source.Update(fixture.World, *fixture.Assets, changed));
	CHECK(changed);
	CHECK(GeometryAddresses(*source.GetCurrentSnapshot()) == original);
	fixture.Opaque->RoughnessFactor = 0.75f;
	++fixture.Opaque->Version;
	REQUIRE(source.Update(fixture.World, *fixture.Assets, changed));
	CHECK(GeometryAddresses(*source.GetCurrentSnapshot()) == original);
	CHECK(source.GetCurrentSnapshot()->GetMaterials()[0].Parameters.y == 0.75f);
	fixture.Entities[0].GetComponent<MeshComponent>().CastShadows = false;
	REQUIRE(source.Update(fixture.World, *fixture.Assets, changed));
	CHECK(GeometryAddresses(*source.GetCurrentSnapshot()) == original);
	CHECK_FALSE(source.GetCurrentSnapshot()->GetInstances()[0].CastShadows);
	CHECK(source.GetStatistics().UniqueGeometryCopies == 3);
	CHECK(source.GetStatistics().ValidationVertexVisits == 108);
	CHECK(source.GetCurrentSnapshot()->GetGeneration() == 4);
	std::swap(fixture.Mesh->Indices[0], fixture.Mesh->Indices[1]);
	++fixture.Mesh->Version;
	REQUIRE(source.Update(fixture.World, *fixture.Assets, changed));
	CHECK(source.GetCurrentSnapshot()->GetGeneration() == 5);
	const auto replaced = GeometryAddresses(*source.GetCurrentSnapshot());
	for (size_t i = 0; i < original.size(); ++i)
		CHECK(replaced[i] != original[i]);
	CHECK(source.GetCurrentSnapshot()->GetGeometry(0).GetTriangles()[0].Position[0] == glm::vec3(2, 0, 0));
	CHECK(source.GetStatistics().UniqueGeometryCopies == 6);
	CHECK(source.GetStatistics().CopiedLocalTriangles == 6);
	CHECK(source.GetStatistics().LegacyExpandedTriangles == 0);
	CHECK(source.GetCurrentLegacy() == nullptr);
}

TEST_CASE("ProbeTraceSource: changed packing remaps every instance while retaining geometry identity")
{
	SourceFixture fixture;
	ProbeTraceSource source;
	bool changed = false;
	REQUIRE(source.Update(fixture.World, *fixture.Assets, changed));
	const auto original = GeometryAddresses(*source.GetCurrentSnapshot());
	fixture.World.DestroyEntity(fixture.Entities[0]);
	REQUIRE(source.Update(fixture.World, *fixture.Assets, changed));
	const auto* snapshot = source.GetCurrentSnapshot();
	REQUIRE(snapshot);
	CHECK(GeometryAddresses(*snapshot) == original);
	REQUIRE(snapshot->GetInstances().size() == 6);
	const std::array<uint32_t, 6> materials{0, 1, 0, 2, 1, 2};
	for (size_t i = 0; i < materials.size(); ++i)
		CHECK(snapshot->GetInstances()[i].MaterialIndex == materials[i]);
	CHECK(snapshot->GetMaterials()[0].Parameters.w == 1);
	CHECK(snapshot->GetMaterials()[2].Parameters.w == 0);
	CHECK(snapshot->GetMaterials()[0].Textures[0] == snapshot->GetMaterials()[2].Textures[0]);
	CHECK(snapshot->GetTexels().size() == 2);
	REQUIRE(source.MaterializeLegacy());
	CHECK(source.GetCurrentLegacy()->GetTriangles().size() == 6);
	for (const auto& triangle : source.GetCurrentLegacy()->GetTriangles())
		CHECK((triangle.Material.x == 0 || triangle.Material.x == 2));
}

TEST_CASE("ProbeTraceSource: rejection hides retained data and does not advance accepted generation")
{
	SourceFixture fixture;
	ProbeTraceSource source;
	bool changed = false;
	REQUIRE(source.Update(fixture.World, *fixture.Assets, changed));
	REQUIRE(source.MaterializeLegacy());
	const auto original = GeometryAddresses(*source.GetCurrentSnapshot());
	const auto originalVersion = fixture.Mesh->Version;
	fixture.Mesh->Indices[0] = std::numeric_limits<uint32_t>::max();
	++fixture.Mesh->Version;
	REQUIRE_FALSE(source.Update(fixture.World, *fixture.Assets, changed));
	CHECK_FALSE(changed);
	CHECK(source.GetCurrentSnapshot() == nullptr);
	CHECK(source.GetCurrentLegacy() == nullptr);
	CHECK_FALSE(source.MaterializeLegacy());
	CHECK(source.GetStatistics().AcceptedChanges == 1);
	const auto visits = source.GetStatistics().ValidationVertexVisits;
	fixture.Entities[2].GetTransform().Translation.y = 8;
	REQUIRE_FALSE(source.Update(fixture.World, *fixture.Assets, changed));
	CHECK(source.GetStatistics().RejectedCacheHits == 1);
	CHECK(source.GetStatistics().ValidationVertexVisits == visits);
	// Restore the exact accepted dependency key to exercise the pre-existing unchanged-recovery path.
	fixture.Entities[2].GetTransform().Translation.y = 0;
	fixture.Mesh->Indices[0] = 0;
	fixture.Mesh->Version = originalVersion;
	REQUIRE(source.Update(fixture.World, *fixture.Assets, changed));
	CHECK_FALSE(changed);
	REQUIRE(source.GetCurrentSnapshot());
	CHECK(source.GetCurrentSnapshot()->GetGeneration() == 1);
	CHECK(GeometryAddresses(*source.GetCurrentSnapshot()) == original);
	CHECK(source.GetCurrentLegacy() != nullptr);
	++fixture.Mesh->Version;
	REQUIRE(source.Update(fixture.World, *fixture.Assets, changed));
	CHECK(changed);
	CHECK(source.GetCurrentSnapshot()->GetGeneration() == 2);
	CHECK(source.GetCurrentLegacy() == nullptr);
}

TEST_CASE("ProbeTraceSource: failed candidates release assets and removed geometry has no retained archive")
{
	SourceFixture fixture;
	ProbeTraceSource source;
	bool changed = false;
	REQUIRE(source.Update(fixture.World, *fixture.Assets, changed));
	auto rejectedMesh = CreateRef<MeshAsset>();
	rejectedMesh->Handle = AssetHandle(73101);
	rejectedMesh->Submeshes.emplace_back().IndexCount = 3;
	fixture.Assets->AddMemoryAsset(rejectedMesh);
	auto bad = fixture.World.CreateEntityWithUUID(UUID(4), "rejected candidate");
	bad.AddComponent<MeshComponent>().Mesh = rejectedMesh->Handle;
	std::weak_ptr<MeshAsset> rejectedLifetime = rejectedMesh;
	REQUIRE_FALSE(source.Update(fixture.World, *fixture.Assets, changed));
	fixture.World.DestroyEntity(bad);
	fixture.RebuildAssetManager();
	rejectedMesh.reset();
	CHECK(rejectedLifetime.expired());
	REQUIRE(source.Update(fixture.World, *fixture.Assets, changed));
	CHECK_FALSE(changed);
	std::weak_ptr<MeshAsset> oldLifetime = fixture.Mesh;
	for (auto entity : fixture.Entities)
		fixture.World.DestroyEntity(entity);
	fixture.Mesh.reset();
	fixture.RebuildAssetManager();
	CHECK_FALSE(oldLifetime.expired()); // Current accepted identity still owns its source.
	REQUIRE(source.Update(fixture.World, *fixture.Assets, changed));
	CHECK(changed);
	CHECK(oldLifetime.expired());
	CHECK(source.GetCurrentSnapshot()->GetGeometryCount() == 0);
}

TEST_CASE("ProbeTraceSource: empty submesh counts do not inherit the expanded triangle allowance")
{
	AssetManager assets;
	Scene world;
	auto mesh = CreateRef<MeshAsset>();
	mesh->Handle = AssetHandle(73201);
	mesh->Vertices.resize(3);
	mesh->Vertices[1].Position.x = 1;
	mesh->Vertices[2].Position.y = 1;
	mesh->Indices = {0, 1, 2};
	mesh->Submeshes.resize(7);
	mesh->Submeshes.back().IndexCount = 3;
	assets.AddMemoryAsset(mesh);
	world.CreateEntityWithUUID(UUID(1), "many empty submeshes").AddComponent<MeshComponent>().Mesh = mesh->Handle;
	GraphicsDeviceLimits limits;
	limits.MaxStorageBufferBytes = sizeof(GPU::ProbeTraceTriangle);
	ProbeTraceSource source(limits);
	bool changed = false;
	REQUIRE(source.Update(world, assets, changed));
	const auto* snapshot = source.GetCurrentSnapshot();
	REQUIRE(snapshot);
	CHECK(snapshot->GetGeometryCount() == 7);
	CHECK(snapshot->GetInstances().size() == 7);
	CHECK(snapshot->GetExpandedTriangleCount() == 1);
	CHECK(snapshot->GetMaterials().size() == 1);
	CHECK(source.GetStatistics().CopiedLocalTriangles == 1);
	for (const auto& instance : snapshot->GetInstances())
		CHECK(instance.FirstCanonicalRank == 0);
	REQUIRE(source.MaterializeLegacy());
	CHECK(source.GetCurrentLegacy()->GetTriangles().size() == 1);
	CHECK(source.GetCurrentLegacy()->GetNodes().size() == 1);
}

TEST_CASE("ProbeTraceSource: expired offending identity retries replacement at the same address handle and version")
{
	// Separate control blocks over the same storage deterministically exercise pointer reuse.
	alignas(MeshAsset) std::array<std::byte, sizeof(MeshAsset)> storage{};
	Scene world;
	ProbeTraceSource source;
	const AssetHandle handle(73301);
	world.CreateEntityWithUUID(UUID(1), "recycled asset").AddComponent<MeshComponent>().Mesh = handle;
	auto construct = [&]() {
		auto mesh = Ref<MeshAsset>(std::construct_at(reinterpret_cast<MeshAsset*>(storage.data())), [](MeshAsset* value) { std::destroy_at(value); });
		mesh->Handle = handle;
		mesh->Submeshes.emplace_back().IndexCount = 3;
		return mesh;
	};
	bool changed = true;
	std::weak_ptr<MeshAsset> oldLifetime;
	uintptr_t oldAddress = 0;
	uint32_t oldVersion = 0;
	{
		AssetManager assets;
		auto mesh = construct();
		assets.AddMemoryAsset(mesh);
		oldLifetime = mesh;
		oldAddress = reinterpret_cast<uintptr_t>(mesh.get());
		oldVersion = mesh->Version;
		REQUIRE_FALSE(source.Update(world, assets, changed));
		CHECK_FALSE(changed);
	}
	REQUIRE(oldLifetime.expired());
	AssetManager assets;
	auto mesh = construct();
	mesh->Vertices.resize(3);
	mesh->Vertices[1].Position.x = 1;
	mesh->Vertices[2].Position.y = 1;
	mesh->Indices = {0, 1, 2};
	assets.AddMemoryAsset(mesh);
	REQUIRE(reinterpret_cast<uintptr_t>(mesh.get()) == oldAddress);
	REQUIRE(mesh->Version == oldVersion);
	REQUIRE(source.Update(world, assets, changed));
	CHECK(changed);
	CHECK(source.GetCurrentSnapshot()->GetGeneration() == 1);
	CHECK(source.GetStatistics().RejectedCacheHits == 0);
	CHECK(source.GetStatistics().ValidationVertexVisits == 3);
}

TEST_CASE("ProbeTraceSource: unrelated expired dependencies preserve cause-specific rejection cache")
{
	SourceFixture fixture;
	ProbeTraceSource source;
	fixture.Mesh->Indices[0] = std::numeric_limits<uint32_t>::max();
	bool changed = false;
	REQUIRE_FALSE(source.Update(fixture.World, *fixture.Assets, changed));
	const auto visits = source.GetStatistics().ValidationVertexVisits;
	std::weak_ptr<MaterialAsset> unrelated = fixture.Empty;
	const auto handle = fixture.Empty->Handle;
	fixture.Empty = CreateRef<MaterialAsset>();
	fixture.Empty->Handle = handle;
	fixture.RebuildAssetManager();
	REQUIRE(unrelated.expired());
	REQUIRE_FALSE(source.Update(fixture.World, *fixture.Assets, changed));
	CHECK(source.GetStatistics().RejectedCacheHits == 1);
	CHECK(source.GetStatistics().ValidationVertexVisits == visits);
}

TEST_CASE("ProbeTraceSource: excluded blend-only expiry preserves budget rejection cache")
{
	SourceFixture fixture;
	GraphicsDeviceLimits limits;
	limits.MaxStorageBufferBytes = sizeof(GPU::ProbeTraceTriangle);
	ProbeTraceSource source(limits);
	bool changed = false;
	REQUIRE_FALSE(source.Update(fixture.World, *fixture.Assets, changed));
	const auto first = source.GetStatistics();
	std::weak_ptr<MaterialAsset> excluded = fixture.Blend;
	const auto handle = fixture.Blend->Handle;
	fixture.Blend = CreateRef<MaterialAsset>();
	fixture.Blend->Handle = handle;
	fixture.Blend->Alpha = AlphaMode::Blend;
	fixture.RebuildAssetManager();
	REQUIRE(excluded.expired());
	REQUIRE_FALSE(source.Update(fixture.World, *fixture.Assets, changed));
	CHECK(source.GetStatistics().RejectedCacheHits == 1);
	CHECK(source.GetStatistics().ValidationVertexVisits == first.ValidationVertexVisits);
	CHECK(source.GetStatistics().UniqueGeometryCopies == first.UniqueGeometryCopies);
}

TEST_CASE("ProbeTraceSource: empty budget dependency sets remain cacheable")
{
	AssetManager assets;
	Scene world;
	GraphicsDeviceLimits limits;
	limits.MaxStorageBufferBytes = sizeof(glm::vec4) - 1;
	ProbeTraceSource source(limits);
	bool changed = true;
	REQUIRE_FALSE(source.Update(world, assets, changed));
	CHECK_FALSE(changed);
	REQUIRE_FALSE(source.Update(world, assets, changed));
	CHECK(source.GetStatistics().RejectedCacheHits == 1);
	CHECK(source.GetStatistics().AcceptedChanges == 0);
	CHECK(source.GetCurrentSnapshot() == nullptr);
}

TEST_CASE("ProbeTraceSource: scene replacement at the same address advances identity and generation")
{
	AssetManager assets;
	std::optional<Scene> world(std::in_place);
	ProbeTraceSource source;
	bool changed = false;
	REQUIRE(source.Update(*world, assets, changed));
	const auto token = source.GetCurrentSnapshot()->GetSceneIdentity();
	const auto address = reinterpret_cast<uintptr_t>(&*world);
	world.emplace();
	REQUIRE(reinterpret_cast<uintptr_t>(&*world) == address);
	REQUIRE(source.Update(*world, assets, changed));
	CHECK(changed);
	CHECK(source.GetCurrentSnapshot()->GetSceneIdentity() != token);
	CHECK(source.GetCurrentSnapshot()->GetGeneration() == 2);
	CHECK(source.GetStatistics().AcceptedCacheHits == 0);
}
