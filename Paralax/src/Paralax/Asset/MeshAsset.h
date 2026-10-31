#pragma once

#include "Paralax/Asset/Asset.h"
#include "Paralax/Core/Base.h"
#include "Paralax/Math/AABB.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace Paralax {

	// Vertex layout shared by every mesh (48 bytes). Tangent.w is the bitangent sign (+1/-1).
	struct MeshVertex
	{
		glm::vec3 Position{0.0f};
		glm::vec3 Normal{0.0f, 1.0f, 0.0f};
		glm::vec4 Tangent{1.0f, 0.0f, 0.0f, 1.0f};
		glm::vec2 UV{0.0f};
	};
	static_assert(sizeof(MeshVertex) == 48, "MeshVertex layout must match the shaders");

	struct Submesh
	{
		std::string Name;
		uint32_t IndexOffset = 0; // First index in MeshAsset::Indices.
		uint32_t IndexCount = 0;
		uint32_t VertexOffset = 0; // Added to every index of this submesh.
		uint32_t VertexCount = 0;
		uint32_t MaterialIndex = 0; // Index into MeshAsset::Materials.
		glm::mat4 Transform{1.0f};  // Node transform relative to the mesh root.
		AABB Bounds;                // In mesh space (Transform applied).
	};

	// Triangle mesh with one or more submeshes sharing a vertex and index buffer.
	class MeshAsset : public Asset
	{
	public:
		AssetType GetType() const override { return AssetType::Mesh; }

		std::vector<MeshVertex> Vertices;
		std::vector<uint32_t> Indices;
		std::vector<Submesh> Submeshes;
		// Default material per material slot (submesh MaterialIndex); MeshComponent can override each slot.
		std::vector<AssetHandle> Materials;
		AABB Bounds;

		// Recomputes submesh and mesh bounds from the vertices.
		void ComputeBounds();
		// Validates indices against vertex ranges and material indices against Materials. Returns false and
		// describes the first problem in outError.
		bool Validate(std::string& outError) const;
	};

	// Computes per-vertex tangents (Tangent.xyz + handedness in w) from positions, normals and UVs for an indexed
	// triangle list. Degenerate UVs fall back to an arbitrary tangent perpendicular to the normal.
	void ComputeTangents(std::vector<MeshVertex>& vertices, const std::vector<uint32_t>& indices, uint32_t indexOffset, uint32_t indexCount, uint32_t vertexOffset, uint32_t vertexCount);

	// Procedural primitives (unit size, centred at the origin, Y up), single submesh with material slot 0.
	namespace MeshPrimitives {

		Ref<MeshAsset> CreateCube(float size = 1.0f);
		Ref<MeshAsset> CreateSphere(float radius = 0.5f, uint32_t segments = 48, uint32_t rings = 24);
		Ref<MeshAsset> CreatePlane(float size = 1.0f, uint32_t subdivisions = 1);
		Ref<MeshAsset> CreateCylinder(float radius = 0.5f, float height = 1.0f, uint32_t segments = 48);
		Ref<MeshAsset> CreateCapsule(float radius = 0.5f, float height = 2.0f, uint32_t segments = 48, uint32_t rings = 16);

	}

}
