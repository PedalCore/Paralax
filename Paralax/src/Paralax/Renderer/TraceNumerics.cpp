#include "pxpch.h"
#include "Paralax/Renderer/TraceNumerics.h"

#include <bit>
#include <cmath>
#include <limits>

namespace Paralax {

	namespace {
		bool EligibleScalar(float value, uint32_t limitBits)
		{
			const auto bits = std::bit_cast<uint32_t>(value) & 0x7fffffffu;
			return bits == 0 || (bits >= 0x00800000u && bits <= limitBits);
		}

		bool Finite(float value)
		{
			return (std::bit_cast<uint32_t>(value) & 0x7fffffffu) < 0x7f800000u;
		}

		bool AdmittedRow(const glm::vec4& row, const glm::vec3& maximum)
		{
			// These stores forbid contraction/reassociation without changing the host rounding environment.
			// Promotion precedes multiplication: even the smallest admitted products are normal in binary64.
			volatile double x = std::abs(double(row.x)) * double(maximum.x);
			volatile double y = std::abs(double(row.y)) * double(maximum.y);
			volatile double z = std::abs(double(row.z)) * double(maximum.z);
			volatile double xy = x + y;
			volatile double xyz = xy + z;
			volatile double sum = xyz + std::abs(double(row.w));
			volatile double twice = 2.0 * sum;
			volatile double bound = twice + 32.0 * double(std::numeric_limits<float>::min());
			return std::isfinite(bound) && bound <= 1000000.0;
		}
	}

	void TraceNumericDomain::Include(std::span<const glm::vec3> positions, std::span<const glm::vec3> normals, const glm::mat4& transform, const glm::mat3& normalTransform)
	{
		if (!m_Eligible)
			return;
		m_Eligible = std::numeric_limits<float>::is_iec559 && std::numeric_limits<float>::digits == 24 && std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::digits == 53;
		glm::vec3 maxPosition(0), maxNormal(0);
		// Inspect every source scalar: extrema alone can hide an inadmissible subnormal inside a large box.
		for (const auto& position : positions)
			for (int axis = 0; axis < 3; ++axis)
			{
				m_Eligible &= EligibleScalar(position[axis], 0x49800000u);
				if (m_Eligible)
					maxPosition[axis] = std::max(maxPosition[axis], std::abs(position[axis]));
			}
		for (const auto& normal : normals)
			for (int axis = 0; axis < 3; ++axis)
			{
				m_Eligible &= EligibleScalar(normal[axis], 0x49800000u);
				if (m_Eligible)
					maxNormal[axis] = std::max(maxNormal[axis], std::abs(normal[axis]));
			}
		for (int row = 0; row < 4; ++row)
			for (int column = 0; column < 4; ++column)
				m_Eligible &= EligibleScalar(transform[column][row], 0x49800000u);
		for (int row = 0; row < 3; ++row)
			for (int column = 0; column < 3; ++column)
				m_Eligible &= EligibleScalar(normalTransform[column][row], 0x49800000u);
		if (!m_Eligible)
			return;
		for (int row = 0; row < 3; ++row)
		{
			m_Eligible &= AdmittedRow(glm::vec4(transform[0][row], transform[1][row], transform[2][row], transform[3][row]), maxPosition);
			m_Eligible &= AdmittedRow(glm::vec4(normalTransform[0][row], normalTransform[1][row], normalTransform[2][row], 0), maxNormal);
		}
	}

	TraceQueryDomain TraceNumericDomain::ClassifyQuery(const glm::vec3& anchor, const glm::vec3& offset, const glm::vec3& direction, float maximum)
	{
		bool finite = Finite(maximum);
		bool eligible = EligibleScalar(maximum, 0x4a800000u);
		for (int axis = 0; axis < 3; ++axis)
		{
			finite &= Finite(anchor[axis]) && Finite(offset[axis]) && Finite(direction[axis]);
			eligible &= EligibleScalar(anchor[axis], 0x4a800000u) && EligibleScalar(offset[axis], 0x4a800000u) && EligibleScalar(direction[axis], 0x40000000u);
		}
		if (!finite)
			return TraceQueryDomain::Invalid;
		return eligible && maximum >= 0 ? TraceQueryDomain::Certified : TraceQueryDomain::BypassAllBounds;
	}

	nvrhi::ShaderHandle TraceNumericDomain::CreateShader(GraphicsDevice& graphics, std::span<const uint8_t> code, const char* entryPoint)
	{
		if (!graphics.GetInfo().SupportsFloat32RTE)
			return nullptr;
		if (code.size() < 20 || code.size() % 4 != 0 || reinterpret_cast<uintptr_t>(code.data()) % alignof(uint32_t) != 0 || !entryPoint || !*entryPoint)
			return nullptr;
		nvrhi::ShaderDesc desc;
		desc.shaderType = nvrhi::ShaderType::Compute;
		desc.entryName = entryPoint;
		desc.debugName = "OptionalTraceNumerics";
		return graphics.GetDevice()->createShader(desc, code.data(), code.size());
	}

}
