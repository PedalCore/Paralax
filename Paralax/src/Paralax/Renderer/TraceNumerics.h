#pragma once

#include "Paralax/Renderer/GraphicsDevice.h"

#include <glm/glm.hpp>

#include <span>

namespace Paralax {

	enum class TraceQueryDomain
	{
		Certified,
		BypassAllBounds,
		Invalid
	};

	// Numerical admission for the optional trace representation. Value-only, thread-safe with independent
	// instances. Accumulate every accepted instance before selecting a representation: failure is sticky and
	// requires complete same-snapshot legacy fallback. This does not replace accepted-scene validation.
	class TraceNumericDomain
	{
	public:
		void Include(std::span<const glm::vec3> positions, std::span<const glm::vec3> normals, const glm::mat4& transform, const glm::mat3& normalTransform);
		bool IsEligible() const { return m_Eligible; }

		// Finite out-of-domain queries bypass every bound, including candidate gates. Nonfinite queries are
		// invalid; anchor and offset must remain separate and maximum is the original query limit.
		static TraceQueryDomain ClassifyQuery(const glm::vec3& anchor, const glm::vec3& offset, const glm::vec3& direction, float maximum);

		// Main-thread-only optional compute shader creation. The caller owns the returned handle and must
		// release it before graphics. code is borrowed trusted compiler-produced SPIR-V (four-byte aligned),
		// needed only during the call; this is not a validator for arbitrary bytecode. Unsupported RTE32 returns
		// null before accessing the device or bytecode. Missing/unaligned code, missing entry name or driver
		// rejection also returns null. Callers select complete fallback.
		static nvrhi::ShaderHandle CreateShader(GraphicsDevice& graphics, std::span<const uint8_t> code, const char* entryPoint);

	private:
		bool m_Eligible = true;
	};

}
