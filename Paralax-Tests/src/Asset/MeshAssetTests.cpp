#include <doctest/doctest.h>

#include <Paralax/Asset/MeshAsset.h>

#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/matrix_transform.hpp>

using namespace Paralax;

namespace {

	// Every triangle must wind counter-clockwise when viewed from the side its vertex normals point to.
	void CheckPrimitive(const MeshAsset& mesh, const glm::vec3& expectedExtents)
	{
		std::string error;
		REQUIRE(mesh.Validate(error));
		REQUIRE(mesh.Submeshes.size() == 1);
		REQUIRE(mesh.Materials.size() == 1);
		CHECK(glm::all(glm::epsilonEqual(mesh.Bounds.GetExtents(), expectedExtents, 1e-3f)));
		CHECK(glm::all(glm::epsilonEqual(mesh.Bounds.GetCenter(), glm::vec3(0.0f), 1e-3f)));

		size_t degenerate = 0;
		for (size_t i = 0; i < mesh.Indices.size(); i += 3)
		{
			const MeshVertex& a = mesh.Vertices[mesh.Indices[i]];
			const MeshVertex& b = mesh.Vertices[mesh.Indices[i + 1]];
			const MeshVertex& c = mesh.Vertices[mesh.Indices[i + 2]];
			const glm::vec3 faceNormal = glm::cross(b.Position - a.Position, c.Position - a.Position);
			if (glm::length(faceNormal) < 1e-7f)
			{
				degenerate++;
				continue;
			}
			REQUIRE(glm::dot(faceNormal, a.Normal + b.Normal + c.Normal) > 0.0f);
		}
		CHECK(degenerate == 0);

		for (const MeshVertex& vertex : mesh.Vertices)
		{
			REQUIRE(glm::length(vertex.Normal) == doctest::Approx(1.0).epsilon(1e-3));
			REQUIRE(glm::length(glm::vec3(vertex.Tangent)) == doctest::Approx(1.0).epsilon(1e-3));
			REQUIRE(std::abs(glm::dot(glm::vec3(vertex.Tangent), vertex.Normal)) < 1e-3f);
			REQUIRE(std::abs(vertex.Tangent.w) == 1.0f);
		}
	}

}

TEST_CASE("MeshPrimitives: all primitives are valid, outward-facing and correctly bounded")
{
	SUBCASE("Cube")
	{
		CheckPrimitive(*MeshPrimitives::CreateCube(2.0f), glm::vec3(1.0f));
	}
	SUBCASE("Sphere")
	{
		CheckPrimitive(*MeshPrimitives::CreateSphere(0.5f), glm::vec3(0.5f));
	}
	SUBCASE("Plane")
	{
		CheckPrimitive(*MeshPrimitives::CreatePlane(4.0f, 3), glm::vec3(2.0f, 0.0f, 2.0f));
	}
	SUBCASE("Cylinder")
	{
		CheckPrimitive(*MeshPrimitives::CreateCylinder(0.5f, 2.0f), glm::vec3(0.5f, 1.0f, 0.5f));
	}
	SUBCASE("Capsule")
	{
		CheckPrimitive(*MeshPrimitives::CreateCapsule(0.5f, 2.0f), glm::vec3(0.5f, 1.0f, 0.5f));
	}
}

TEST_CASE("MeshAsset: Validate catches out-of-range indices, ranges and material slots")
{
	Ref<MeshAsset> mesh = MeshPrimitives::CreateCube();
	std::string error;

	mesh->Indices[0] = 9999;
	CHECK_FALSE(mesh->Validate(error));
	CHECK(error.find("out of range") != std::string::npos);

	mesh = MeshPrimitives::CreateCube();
	mesh->Submeshes[0].MaterialIndex = 3;
	CHECK_FALSE(mesh->Validate(error));

	mesh = MeshPrimitives::CreateCube();
	mesh->Submeshes[0].IndexCount += 3;
	CHECK_FALSE(mesh->Validate(error));

	mesh = MeshPrimitives::CreateCube();
	mesh->Indices.pop_back();
	CHECK_FALSE(mesh->Validate(error));
}

TEST_CASE("AABB: expansion and transformation")
{
	AABB box;
	CHECK_FALSE(box.IsValid());
	box.Expand(glm::vec3(-1.0f, 0.0f, 2.0f));
	box.Expand(glm::vec3(1.0f, 2.0f, 4.0f));
	CHECK(box.IsValid());
	CHECK(box.GetCenter() == glm::vec3(0.0f, 1.0f, 3.0f));

	const AABB moved = box.Transformed(glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f)), glm::vec3(2.0f)));
	CHECK(moved.Min == glm::vec3(8.0f, 0.0f, 4.0f));
	CHECK(moved.Max == glm::vec3(12.0f, 4.0f, 8.0f));
	CHECK_FALSE(AABB().Transformed(glm::mat4(1.0f)).IsValid());
}

TEST_CASE("MeshAsset: Validate requires whole triangles in every submesh")
{
	Ref<MeshAsset> mesh = MeshPrimitives::CreateCube();
	Submesh extra = mesh->Submeshes[0];
	mesh->Submeshes[0].IndexCount -= 1;
	extra.IndexOffset = mesh->Submeshes[0].IndexCount;
	extra.IndexCount = 1;
	mesh->Submeshes.push_back(extra);
	std::string error;
	CHECK_FALSE(mesh->Validate(error));
	CHECK(error.find("multiple of 3") != std::string::npos);
}
