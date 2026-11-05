#pragma once

#include <glm/glm.hpp>

namespace Paralax {

	namespace GPU {

		// Mirrors ProbeGI.slang. Preorder nodes use an escape index for bounded, stackless traversal.
		struct ProbeTraceNode
		{
			glm::vec4 Minimum{0.0f};
			glm::vec4 Maximum{0.0f};
			glm::uvec4 Range{0}; // first triangle, triangle count (zero for internal nodes), escape node, unused
		};
		struct ProbeTraceTriangle
		{
			glm::vec4 Position[3]{};
			glm::vec4 Normal[3]{};
			glm::vec4 UV01{0.0f};
			glm::vec4 UV2{0.0f};
			glm::uvec4 Material{0}; // x: material index; y: exclude from direct-light shadow rays
		};
		struct ProbeTraceMaterial
		{
			glm::vec4 BaseColor{1.0f};
			glm::vec4 Emissive{0.0f};   // w: double-sided
			glm::vec4 Parameters{0.0f}; // metallic, roughness, alpha cutoff, alpha mode
			glm::uvec4 Textures[3]{};   // base colour, emissive, metal/rough: offset, width, height, unused
		};
		static_assert(sizeof(ProbeTraceNode) == 48);
		static_assert(sizeof(ProbeTraceTriangle) == 144);
		static_assert(sizeof(ProbeTraceMaterial) == 96);

	}

}
