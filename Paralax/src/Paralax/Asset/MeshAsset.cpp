#include "pxpch.h"
#include "Paralax/Asset/MeshAsset.h"

#include <glm/gtc/constants.hpp>

#include <cmath>

namespace Paralax {

	void MeshAsset::ComputeBounds()
	{
		Bounds = AABB();
		for (Submesh& submesh : Submeshes)
		{
			AABB local;
			for (uint32_t i = 0; i < submesh.IndexCount; i++)
			{
				const uint32_t index = Indices[submesh.IndexOffset + i] + submesh.VertexOffset;
				local.Expand(Vertices[index].Position);
			}
			submesh.Bounds = local.Transformed(submesh.Transform);
			Bounds.Expand(submesh.Bounds);
		}
	}

	bool MeshAsset::Validate(std::string& outError) const
	{
		if (Indices.size() % 3 != 0)
		{
			outError = "index count is not a multiple of 3";
			return false;
		}
		for (size_t s = 0; s < Submeshes.size(); s++)
		{
			const Submesh& submesh = Submeshes[s];
			if (submesh.IndexCount % 3 != 0)
			{
				outError = fmt::format("submesh {} index count is not a multiple of 3", s);
				return false;
			}
			if (static_cast<uint64_t>(submesh.IndexOffset) + submesh.IndexCount > Indices.size())
			{
				outError = fmt::format("submesh {} index range exceeds the index buffer", s);
				return false;
			}
			if (static_cast<uint64_t>(submesh.VertexOffset) + submesh.VertexCount > Vertices.size())
			{
				outError = fmt::format("submesh {} vertex range exceeds the vertex buffer", s);
				return false;
			}
			for (uint32_t i = 0; i < submesh.IndexCount; i++)
			{
				if (Indices[submesh.IndexOffset + i] >= submesh.VertexCount)
				{
					outError = fmt::format("submesh {} index {} is out of range", s, i);
					return false;
				}
			}
			if (submesh.MaterialIndex >= std::max<size_t>(Materials.size(), 1))
			{
				outError = fmt::format("submesh {} references material slot {} of {}", s, submesh.MaterialIndex, Materials.size());
				return false;
			}
		}
		return true;
	}

	void ComputeTangents(std::vector<MeshVertex>& vertices, const std::vector<uint32_t>& indices, uint32_t indexOffset, uint32_t indexCount, uint32_t vertexOffset, uint32_t vertexCount)
	{
		// Scratch space covers only this submesh's vertices (indexed relative to vertexOffset).
		std::vector<glm::vec3> tangents(vertexCount, glm::vec3(0.0f));
		std::vector<glm::vec3> bitangents(vertexCount, glm::vec3(0.0f));

		for (uint32_t i = 0; i + 2 < indexCount; i += 3)
		{
			const uint32_t i0 = indices[indexOffset + i] + vertexOffset;
			const uint32_t i1 = indices[indexOffset + i + 1] + vertexOffset;
			const uint32_t i2 = indices[indexOffset + i + 2] + vertexOffset;

			const glm::vec3 edge1 = vertices[i1].Position - vertices[i0].Position;
			const glm::vec3 edge2 = vertices[i2].Position - vertices[i0].Position;
			const glm::vec2 duv1 = vertices[i1].UV - vertices[i0].UV;
			const glm::vec2 duv2 = vertices[i2].UV - vertices[i0].UV;

			const float determinant = duv1.x * duv2.y - duv2.x * duv1.y;
			if (std::abs(determinant) < 1e-12f)
				continue;
			const float inverse = 1.0f / determinant;
			const glm::vec3 tangent = (edge1 * duv2.y - edge2 * duv1.y) * inverse;
			const glm::vec3 bitangent = (edge2 * duv1.x - edge1 * duv2.x) * inverse;

			for (uint32_t index : {i0, i1, i2})
			{
				tangents[index - vertexOffset] += tangent;
				bitangents[index - vertexOffset] += bitangent;
			}
		}

		for (uint32_t i = 0; i < indexCount; i++)
		{
			const uint32_t index = indices[indexOffset + i] + vertexOffset;
			MeshVertex& vertex = vertices[index];
			const glm::vec3 n = glm::normalize(vertex.Normal);

			// Gram-Schmidt orthogonalisation against the normal.
			const uint32_t local = index - vertexOffset;
			glm::vec3 t = tangents[local] - n * glm::dot(n, tangents[local]);
			if (glm::dot(t, t) < 1e-12f)
			{
				// No usable UV gradient: any vector perpendicular to the normal will do.
				const glm::vec3 axis = std::abs(n.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
				t = glm::cross(axis, n);
			}
			t = glm::normalize(t);
			// glTF convention (matching the primitives): B = cross(N, T) * w points towards decreasing v.
			const float handedness = glm::dot(glm::cross(n, t), bitangents[local]) < 0.0f ? 1.0f : -1.0f;
			vertex.Tangent = glm::vec4(t, handedness);
		}
	}

	namespace MeshPrimitives {

		namespace {

			Ref<MeshAsset> Finish(std::vector<MeshVertex> vertices, std::vector<uint32_t> indices, const char* name, bool computeTangents)
			{
				auto mesh = CreateRef<MeshAsset>();
				Submesh submesh;
				submesh.Name = name;
				submesh.IndexCount = static_cast<uint32_t>(indices.size());
				submesh.VertexCount = static_cast<uint32_t>(vertices.size());
				mesh->Vertices = std::move(vertices);
				mesh->Indices = std::move(indices);
				if (computeTangents)
					ComputeTangents(mesh->Vertices, mesh->Indices, 0, submesh.IndexCount, 0, submesh.VertexCount);
				mesh->Submeshes.push_back(submesh);
				mesh->Materials.push_back(0);
				mesh->ComputeBounds();
				return mesh;
			}

		}

		Ref<MeshAsset> CreateCube(float size)
		{
			const float h = size * 0.5f;
			struct Face
			{
				glm::vec3 Normal, U, V;
			};
			// U/V span each face so that (U x V) == Normal (counter-clockwise when viewed from outside).
			const Face faces[6] = {
				{{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
				{{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
				{{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},
				{{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
				{{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
				{{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
			};

			std::vector<MeshVertex> vertices;
			std::vector<uint32_t> indices;
			for (const Face& face : faces)
			{
				const uint32_t base = static_cast<uint32_t>(vertices.size());
				const glm::vec2 corners[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
				for (const glm::vec2& c : corners)
				{
					MeshVertex vertex;
					vertex.Position = (face.Normal + face.U * c.x + face.V * c.y) * h;
					vertex.Normal = face.Normal;
					vertex.Tangent = glm::vec4(face.U, 1.0f);
					vertex.UV = {(c.x + 1.0f) * 0.5f, 1.0f - (c.y + 1.0f) * 0.5f};
					vertices.push_back(vertex);
				}
				indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
			}
			return Finish(std::move(vertices), std::move(indices), "Cube", false);
		}

		Ref<MeshAsset> CreateSphere(float radius, uint32_t segments, uint32_t rings)
		{
			segments = std::max(segments, 3u);
			rings = std::max(rings, 2u);

			std::vector<MeshVertex> vertices;
			std::vector<uint32_t> indices;
			for (uint32_t ring = 0; ring <= rings; ring++)
			{
				const float v = static_cast<float>(ring) / static_cast<float>(rings);
				const float phi = v * glm::pi<float>();
				for (uint32_t segment = 0; segment <= segments; segment++)
				{
					const float u = static_cast<float>(segment) / static_cast<float>(segments);
					const float theta = u * glm::two_pi<float>();
					const glm::vec3 normal(std::sin(phi) * std::cos(theta), std::cos(phi), -std::sin(phi) * std::sin(theta));

					MeshVertex vertex;
					vertex.Position = normal * radius;
					vertex.Normal = normal;
					vertex.Tangent = glm::vec4(-std::sin(theta), 0.0f, -std::cos(theta), 1.0f);
					vertex.UV = {u, v};
					vertices.push_back(vertex);
				}
			}

			const uint32_t stride = segments + 1;
			for (uint32_t ring = 0; ring < rings; ring++)
			{
				for (uint32_t segment = 0; segment < segments; segment++)
				{
					const uint32_t a = ring * stride + segment;
					const uint32_t b = a + stride;
					if (ring != 0)
						indices.insert(indices.end(), {a, b, a + 1});
					if (ring != rings - 1)
						indices.insert(indices.end(), {a + 1, b, b + 1});
				}
			}
			return Finish(std::move(vertices), std::move(indices), "Sphere", false);
		}

		Ref<MeshAsset> CreatePlane(float size, uint32_t subdivisions)
		{
			subdivisions = std::max(subdivisions, 1u);
			std::vector<MeshVertex> vertices;
			std::vector<uint32_t> indices;
			for (uint32_t z = 0; z <= subdivisions; z++)
			{
				for (uint32_t x = 0; x <= subdivisions; x++)
				{
					const float u = static_cast<float>(x) / static_cast<float>(subdivisions);
					const float v = static_cast<float>(z) / static_cast<float>(subdivisions);
					MeshVertex vertex;
					vertex.Position = {(u - 0.5f) * size, 0.0f, (v - 0.5f) * size};
					vertex.Normal = {0.0f, 1.0f, 0.0f};
					vertex.Tangent = {1.0f, 0.0f, 0.0f, 1.0f};
					vertex.UV = {u, v};
					vertices.push_back(vertex);
				}
			}
			const uint32_t stride = subdivisions + 1;
			for (uint32_t z = 0; z < subdivisions; z++)
			{
				for (uint32_t x = 0; x < subdivisions; x++)
				{
					const uint32_t a = z * stride + x;
					indices.insert(indices.end(), {a, a + stride, a + 1, a + 1, a + stride, a + stride + 1});
				}
			}
			return Finish(std::move(vertices), std::move(indices), "Plane", false);
		}

		Ref<MeshAsset> CreateCylinder(float radius, float height, uint32_t segments)
		{
			segments = std::max(segments, 3u);
			const float h = height * 0.5f;
			std::vector<MeshVertex> vertices;
			std::vector<uint32_t> indices;

			// Side.
			for (uint32_t segment = 0; segment <= segments; segment++)
			{
				const float u = static_cast<float>(segment) / static_cast<float>(segments);
				const float theta = u * glm::two_pi<float>();
				const glm::vec3 normal(std::cos(theta), 0.0f, -std::sin(theta));
				for (int side = 0; side < 2; side++)
				{
					MeshVertex vertex;
					vertex.Position = normal * radius + glm::vec3(0.0f, side == 0 ? -h : h, 0.0f);
					vertex.Normal = normal;
					vertex.UV = {u, side == 0 ? 1.0f : 0.0f};
					vertices.push_back(vertex);
				}
			}
			for (uint32_t segment = 0; segment < segments; segment++)
			{
				const uint32_t a = segment * 2;
				indices.insert(indices.end(), {a, a + 2, a + 1, a + 1, a + 2, a + 3});
			}

			// Caps.
			for (int cap = 0; cap < 2; cap++)
			{
				const float y = cap == 0 ? -h : h;
				const glm::vec3 normal(0.0f, cap == 0 ? -1.0f : 1.0f, 0.0f);
				const uint32_t center = static_cast<uint32_t>(vertices.size());
				MeshVertex centerVertex;
				centerVertex.Position = {0.0f, y, 0.0f};
				centerVertex.Normal = normal;
				centerVertex.UV = {0.5f, 0.5f};
				vertices.push_back(centerVertex);
				for (uint32_t segment = 0; segment <= segments; segment++)
				{
					const float theta = static_cast<float>(segment) / static_cast<float>(segments) * glm::two_pi<float>();
					MeshVertex vertex;
					vertex.Position = {std::cos(theta) * radius, y, -std::sin(theta) * radius};
					vertex.Normal = normal;
					vertex.UV = {std::cos(theta) * 0.5f + 0.5f, std::sin(theta) * 0.5f + 0.5f};
					vertices.push_back(vertex);
				}
				for (uint32_t segment = 0; segment < segments; segment++)
				{
					const uint32_t a = center + 1 + segment;
					if (cap == 0)
						indices.insert(indices.end(), {center, a + 1, a});
					else
						indices.insert(indices.end(), {center, a, a + 1});
				}
			}
			return Finish(std::move(vertices), std::move(indices), "Cylinder", true);
		}

		Ref<MeshAsset> CreateCapsule(float radius, float height, uint32_t segments, uint32_t rings)
		{
			segments = std::max(segments, 3u);
			rings = std::max(rings, 2u) & ~1u; // Even, so the equator splits the hemispheres.
			const float halfCylinder = std::max(height * 0.5f - radius, 0.0f);

			std::vector<MeshVertex> vertices;
			std::vector<uint32_t> indices;
			// A sphere split at the equator, with the halves pushed apart by the cylinder height. The equator ring is
			// duplicated so the cylinder section has its own vertices.
			for (uint32_t ring = 0; ring <= rings + 1; ring++)
			{
				const uint32_t sphereRing = ring <= rings / 2 ? ring : ring - 1;
				const float phi = static_cast<float>(sphereRing) / static_cast<float>(rings) * glm::pi<float>();
				const float offset = ring <= rings / 2 ? halfCylinder : -halfCylinder;
				for (uint32_t segment = 0; segment <= segments; segment++)
				{
					const float u = static_cast<float>(segment) / static_cast<float>(segments);
					const float theta = u * glm::two_pi<float>();
					const glm::vec3 normal(std::sin(phi) * std::cos(theta), std::cos(phi), -std::sin(phi) * std::sin(theta));
					MeshVertex vertex;
					vertex.Position = normal * radius + glm::vec3(0.0f, offset, 0.0f);
					vertex.Normal = normal;
					vertex.UV = {u, static_cast<float>(ring) / static_cast<float>(rings + 1)};
					vertices.push_back(vertex);
				}
			}

			const uint32_t stride = segments + 1;
			for (uint32_t ring = 0; ring <= rings; ring++)
			{
				for (uint32_t segment = 0; segment < segments; segment++)
				{
					const uint32_t a = ring * stride + segment;
					const uint32_t b = a + stride;
					if (ring != 0)
						indices.insert(indices.end(), {a, b, a + 1});
					if (ring != rings)
						indices.insert(indices.end(), {a + 1, b, b + 1});
				}
			}
			return Finish(std::move(vertices), std::move(indices), "Capsule", true);
		}

	}

}
