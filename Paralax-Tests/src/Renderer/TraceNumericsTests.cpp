#include <doctest/doctest.h>

#include "ExactTraceReference.h"
#include "GPUTestContext.h"
#include <Paralax/Core/EmbeddedFile.h>
#include <Paralax/Renderer/ShaderLibrary.h>
#include <Paralax/Renderer/TraceNumerics.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <set>
#include <vector>

namespace Paralax::EmbeddedTestShaders {
	extern const EmbeddedFile* const g_Entries;
	extern const size_t g_EntryCount;
}

using namespace Paralax;
using TraceReference::Exact;
using TraceReference::Interval;

namespace {
	std::span<const uint8_t> Code()
	{
		return FindEmbeddedFile(EmbeddedTestShaders::g_Entries, EmbeddedTestShaders::g_EntryCount, "TraceNumericsTest");
	}

	// A capability-off device that cannot create any module/pipeline. Observing GetDevice would itself be
	// a failure: fallback must precede even the first device access, not merely return a null shader later.
	class UnsupportedDevice final : public GraphicsDevice
	{
	public:
		GraphicsDeviceInfo Info;
		mutable uint32_t DeviceAccesses = 0;
		nvrhi::IDevice* GetDevice() const override
		{
			++DeviceAccesses;
			return nullptr;
		}
		nvrhi::IDevice* GetBackendDevice() const override
		{
			++DeviceAccesses;
			return nullptr;
		}
		const GraphicsDeviceInfo& GetInfo() const override { return Info; }
		uint32_t GetValidationErrorCount() const override { return 0; }
		void WaitForIdle() override {}
		void CollectGarbage() override {}
		void* GetNativeInstance() const override { return nullptr; }
		void* GetNativePhysicalDevice() const override { return nullptr; }
		void* GetNativeDevice() const override { return nullptr; }
		void* GetNativeGraphicsQueue() const override { return nullptr; }
		uint32_t GetGraphicsQueueFamily() const override { return 0; }
		void* GetVulkanProcAddress(const char*) const override { return nullptr; }
	};

	struct Input
	{
		glm::vec4 RowX{1, 0, 0, 0}, RowY{0, 1, 0, 0}, RowZ{0, 0, 1, 0};
		glm::vec4 Lower{0}, Upper{0}, AnchorMaximum{0, 0, 0, 1}, Offset{0}, Direction{0, 0, 1, 0};
	};
	static_assert(sizeof(Input) == 128);

	std::vector<std::array<float, 12>> Run(const char* entry, std::span<const Input> input)
	{
		auto& graphics = *Testing::GetTestGraphicsDevice();
		auto* device = graphics.GetDevice();
		const auto errors = graphics.GetValidationErrorCount();
		auto shader = TraceNumericDomain::CreateShader(graphics, Code(), entry);
		REQUIRE(shader);
		nvrhi::BindingLayoutDesc layoutDesc;
		layoutDesc.visibility = nvrhi::ShaderType::Compute;
		layoutDesc.bindings = {nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0), nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0)};
		auto layout = device->createBindingLayout(layoutDesc);
		REQUIRE(layout);
		auto pipeline = device->createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(shader).addBindingLayout(layout));
		REQUIRE(pipeline);
		const size_t outputBytes = input.size() * 48;
		auto source = device->createBuffer(nvrhi::BufferDesc().setByteSize(input.size_bytes()).setStructStride(sizeof(Input)).setInitialState(nvrhi::ResourceStates::ShaderResource).setKeepInitialState(true));
		auto output = device->createBuffer(nvrhi::BufferDesc().setByteSize(outputBytes).setStructStride(16).setCanHaveUAVs(true).setInitialState(nvrhi::ResourceStates::UnorderedAccess).setKeepInitialState(true));
		auto staging = device->createBuffer(nvrhi::BufferDesc().setByteSize(outputBytes).setCpuAccess(nvrhi::CpuAccessMode::Read).setInitialState(nvrhi::ResourceStates::CopyDest).setKeepInitialState(true));
		REQUIRE(source);
		REQUIRE(output);
		REQUIRE(staging);
		nvrhi::BindingSetDesc desc;
		desc.bindings = {nvrhi::BindingSetItem::StructuredBuffer_SRV(0, source), nvrhi::BindingSetItem::StructuredBuffer_UAV(0, output)};
		auto bindings = device->createBindingSet(desc, layout);
		REQUIRE(bindings);
		auto commands = device->createCommandList();
		REQUIRE(commands);
		commands->open();
		commands->writeBuffer(source, input.data(), input.size_bytes());
		nvrhi::ComputeState state;
		state.pipeline = pipeline;
		state.bindings = {bindings};
		commands->setComputeState(state);
		commands->dispatch(static_cast<uint32_t>(input.size()), 1, 1);
		commands->copyBuffer(staging, 0, output, 0, outputBytes);
		commands->close();
		device->executeCommandList(commands);
		graphics.WaitForIdle();
		const void* mapped = device->mapBuffer(staging, nvrhi::CpuAccessMode::Read);
		REQUIRE(mapped);
		std::vector<std::array<float, 12>> result(input.size());
		std::memcpy(result.data(), mapped, outputBytes);
		device->unmapBuffer(staging);
		CHECK(graphics.GetValidationErrorCount() == errors);
		return result;
	}

	bool RequireRTE()
	{
		if (!Testing::RequireTestGraphicsDevice())
			return false;
		if (!Testing::GetTestGraphicsDevice()->GetInfo().SupportsFloat32RTE)
		{
			WARN_MESSAGE(false, "RTE32 is unsupported; optional kernels stay unavailable and legacy rendering remains supported");
			return false;
		}
		return true;
	}

	void CheckBits(float actual, float expected)
	{
		CHECK(std::bit_cast<uint32_t>(actual) == std::bit_cast<uint32_t>(expected));
	}

	Interval ReferenceRow(const Input& input, glm::vec4 row)
	{
		const auto x = TraceReference::Multiply({input.Lower.x, input.Upper.x}, {row.x, row.x});
		const auto y = TraceReference::Multiply({input.Lower.y, input.Upper.y}, {row.y, row.y});
		const auto z = TraceReference::Multiply({input.Lower.z, input.Upper.z}, {row.z, row.z});
		return TraceReference::Add(TraceReference::Add(TraceReference::Add(x, y), z), {row.w, row.w});
	}
}

TEST_CASE("TraceNumerics: unsupported capability selects fallback before module or device access")
{
	UnsupportedDevice graphics;
	REQUIRE_FALSE(Code().empty());
	CHECK_FALSE(TraceNumericDomain::CreateShader(graphics, Code(), "Bounds"));
	CHECK_FALSE(TraceNumericDomain::CreateShader(graphics, {}, nullptr));
	CHECK(graphics.DeviceAccesses == 0);
	graphics.Info.SupportsFloat32RTE = true;
	CHECK_FALSE(TraceNumericDomain::CreateShader(graphics, {}, "Bounds"));
	CHECK_FALSE(TraceNumericDomain::CreateShader(graphics, Code(), nullptr));
	CHECK_FALSE(TraceNumericDomain::CreateShader(graphics, Code().subspan(1, 20), "Bounds"));
	CHECK(graphics.DeviceAccesses == 0);
}

TEST_CASE("TraceNumerics: optimized artifacts isolate RTE32 and preserve scalar uncontracted operations")
{
	std::vector<uint32_t> words(Code().size() / 4);
	REQUIRE(words.size() >= 5);
	std::memcpy(words.data(), Code().data(), Code().size());
	REQUIRE(words[0] == 0x07230203);
	std::set<uint32_t> floatTypes, precise, entries, rounding;
	std::vector<std::pair<uint32_t, uint32_t>> arithmetic;
	bool capability = false;
	for (size_t at = 5; at < words.size();)
	{
		const uint32_t count = words[at] >> 16, op = words[at] & 65535;
		REQUIRE(count > 0);
		REQUIRE(at + count <= words.size());
		if (op == 17 && words[at + 1] == 4467)
			capability = true;
		if (op == 15)
			entries.insert(words[at + 2]);
		if (op == 16 && words[at + 2] == 4462)
		{
			REQUIRE(count == 4);
			CHECK(words[at + 3] == 32);
			rounding.insert(words[at + 1]);
		}
		if (op == 22)
		{
			CHECK(words[at + 2] == 32);
			floatTypes.insert(words[at + 1]);
		}
		if (op == 71)
		{
			CHECK(words[at + 2] != 0); // RelaxedPrecision
			if (words[at + 2] == 42)
				precise.insert(words[at + 1]);
		}
		if (op == 129 || op == 131 || op == 133)
			arithmetic.emplace_back(words[at + 1], words[at + 2]);
		CHECK_FALSE((op == 136 || (op >= 142 && op <= 148))); // division or compound float algebra
		if (op == 12)
			CHECK(words[at + 4] != 50); // GLSL.std.450 Fma
		at += count;
	}
	CHECK(capability);
	CHECK(entries.size() == 4);
	CHECK(entries == rounding);
	REQUIRE(arithmetic.size() > 20);
	for (const auto& [type, id] : arithmetic)
	{
		CHECK(floatTypes.contains(type));
		CHECK(precise.contains(id));
	}
	// Optional capability must not leak into any existing production shader module.
	for (auto name : ShaderLibrary::GetModuleNames())
	{
		CAPTURE(name);
		const auto code = ShaderLibrary::GetModule(name);
		words.resize(code.size() / 4);
		std::memcpy(words.data(), code.data(), code.size());
		for (size_t at = 5; at < words.size();)
		{
			const uint32_t count = words[at] >> 16, op = words[at] & 65535;
			REQUIRE(count > 0);
			REQUIRE(at + count <= words.size());
			if (op == 17)
				CHECK(words[at + 1] != 4467);
			if (op == 16)
				CHECK(words[at + 2] != 4462);
			at += count;
		}
	}
}

TEST_CASE("TraceNumerics: whole snapshot admission examines hidden source values and guard boundaries")
{
	const glm::mat4 identity(1);
	const glm::mat3 normalIdentity(1);
	std::array<glm::vec3, 3> positions{glm::vec3(-1), glm::vec3(0), glm::vec3(1)};
	std::array<glm::vec3, 3> normals{glm::vec3(0), glm::vec3(0, 1, 0), glm::vec3(0)};
	TraceNumericDomain domain;
	domain.Include(positions, normals, identity, normalIdentity);
	CHECK(domain.IsEligible());
	positions[1].x = std::numeric_limits<float>::denorm_min();
	domain.Include(positions, normals, identity, normalIdentity);
	CHECK_FALSE(domain.IsEligible());
	positions[1].x = 0;
	domain.Include(positions, normals, identity, normalIdentity);
	CHECK_FALSE(domain.IsEligible()); // no later instance can repair a failed snapshot
	for (float invalid : {std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(), 2097152.0f})
		for (uint32_t field = 0; field < 4; ++field)
		{
			TraceNumericDomain candidate;
			auto p = positions, n = normals;
			auto matrix = identity;
			auto normal = normalIdentity;
			if (field == 0)
				p[1].z = invalid;
			if (field == 1)
				n[1].z = invalid;
			if (field == 2)
				matrix[0][3] = invalid;
			if (field == 3)
				normal[0][2] = invalid;
			candidate.Include(p, n, matrix, normal);
			CHECK_FALSE(candidate.IsEligible());
		}
	// Guard is intentionally conservative, including severe cancellation that legacy can still accept.
	for (float magnitude : {499999.0f, 500001.0f, 1048576.0f})
	{
		TraceNumericDomain candidate;
		auto matrix = identity;
		matrix[3].x = magnitude;
		std::array<glm::vec3, 1> zero{glm::vec3(0)};
		candidate.Include(zero, zero, matrix, normalIdentity);
		CHECK(candidate.IsEligible() == (magnitude < 500000.0f));
	}
	TraceNumericDomain cancellation;
	std::array<glm::vec3, 1> large{glm::vec3(1048576, 1048576, 0)};
	auto matrix = identity;
	matrix[0][0] = 1048576;
	matrix[1][0] = -1048576;
	cancellation.Include(large, normals, matrix, normalIdentity);
	CHECK_FALSE(cancellation.IsEligible());
	TraceNumericDomain nonAffine;
	matrix = identity;
	matrix[0][3] = 0.5f;
	nonAffine.Include(positions, normals, matrix, normalIdentity);
	CHECK(nonAffine.IsEligible()); // certificate uses xyz; existing acceptance still validates full vec4
	// Admission's headroom follows gamma7(double), gamma6(float) and at most 8N output-FTZ error.
	const double doubleEpsilon = std::ldexp(1.0, -52), floatEpsilon = std::ldexp(1.0, -23);
	const double gamma7 = 7 * doubleEpsilon / (1 - 7 * doubleEpsilon);
	const double gamma6 = 6 * floatEpsilon / (1 - 6 * floatEpsilon);
	CHECK((1 + gamma6) / (1 - gamma7) < 2);
	CHECK(8 * double(std::numeric_limits<float>::min()) < 32 * double(std::numeric_limits<float>::min()));
}

TEST_CASE("TraceNumerics: exact binary reference rounds ties and retains cancellation beyond double precision")
{
	CheckBits((Exact(1) + Exact(std::ldexp(1.0f, -24))).Rounded(), 1);
	CheckBits((Exact(std::nextafter(1.0f, 2.0f)) + Exact(std::ldexp(1.0f, -24))).Rounded(), std::bit_cast<float>(0x3f800002u));
	const float small = std::numeric_limits<float>::min();
	CheckBits(((Exact(1048576) + Exact(small)) - Exact(1048576)).Rounded(), small);
	CheckBits((Exact(small) * Exact(0.5f)).Rounded(), std::bit_cast<float>(0x00400000u));
	CheckBits((Exact(-1) - Exact(std::ldexp(1.0f, -24))).Rounded(), -1);
	CheckBits((Exact(-std::nextafter(1.0f, 2.0f)) - Exact(std::ldexp(1.0f, -24))).Rounded(), std::bit_cast<float>(0xbf800002u));
	CheckBits((Exact(small) * Exact(std::bit_cast<float>(0x3f7fffffu))).Rounded(), small);
	CheckBits((Exact(-small) * Exact(std::bit_cast<float>(0x3f7fffffu))).Rounded(), -small);
	CheckBits((Exact(std::numeric_limits<float>::denorm_min()) * Exact(0.5f)).Rounded(), 0);
	// Carry/borrow through many zero/all-one limbs, and independent decimal integer products.
	const Exact enormous(std::ldexp(1.0f, 100)), tiny(std::ldexp(1.0f, -100));
	CHECK(((enormous - tiny) + tiny).Compare(enormous) == 0);
	CHECK(((tiny - enormous) + enormous).Compare(tiny) == 0);
	CHECK(((enormous + tiny) - enormous).Compare(tiny) == 0);
	CHECK((Exact(12345) * Exact(-6789)).Compare(-Exact(83810208.0f) + Exact(3)) == 0);
	const std::array<Interval, 3> point{{{1, 1}, {0, 0}, {0, 0}}};
	CHECK(TraceReference::Intersects(point, {0, 0, 0}, {0, 0, 0}, {1, 0, 0}, 1));
	CHECK_FALSE(TraceReference::Intersects(point, {0, 0, 0}, {0, 0, 0}, {1, 0, 0}, std::nextafter(1.0f, 0.0f)));
}

TEST_CASE("TraceNumerics: GPU arithmetic matches exact rounding through underflow and nesting")
{
	if (!RequireRTE())
		return;
	const std::array<uint32_t, 19> patterns{0, 0x80000000u, 0x00800000u, 0x00800001u, 0x00800002u, 0x80800000u, 0x80800001u, 0x80800002u, 0x3f000000u, 0xbf000000u, 0x3f7fffffu, 0x3f800000u, 0x3f800001u, 0xbf800000u, 0x33800000u, 0x49800000u, 0xc9800000u, 0x7fffffu, 1};
	std::vector<Input> input;
	for (auto a : patterns)
		for (auto b : patterns)
		{
			// Raw subnormals are tested in Wrap; arithmetic operands must themselves be normal or zero.
			Input value;
			value.Lower = glm::vec4(std::bit_cast<float>((a & 0x7fffffffu) < 0x00800000u ? a & 0x80000000u : a), std::bit_cast<float>((b & 0x7fffffffu) < 0x00800000u ? b & 0x80000000u : b), std::bit_cast<float>(a), 0);
			value.Upper = value.Lower;
			input.push_back(value);
		}
	const size_t nestedStart = input.size();
	for (float scale : {1.0f, std::numeric_limits<float>::min(), std::bit_cast<float>(0x00800001u)})
	{
		Input child;
		child.Lower = glm::vec4(-2 * scale, -4 * scale, 0, 0);
		child.Upper = glm::vec4(3 * scale, 5 * scale, 0, 0);
		input.push_back(child);
		Input parent = child;
		parent.Lower = glm::vec4(-3 * scale, -5 * scale, 0, 0);
		parent.Upper = glm::vec4(4 * scale, 6 * scale, 0, 0);
		input.push_back(parent);
	}
	// Equality and strict nesting cross the discontinuous widening region at both +/-N and E.
	for (uint32_t bits : {0x00800000u, 0x00800001u, 0x00800002u})
	{
		const float edge = std::bit_cast<float>(bits), next = std::bit_cast<float>(bits + 1);
		Input child;
		child.Lower = glm::vec4(-edge, -0.5f, 0, 0);
		child.Upper = glm::vec4(edge, 1.0f, 0, 0);
		input.push_back(child);
		child.Lower.x = -next;
		child.Upper.x = next;
		input.push_back(child);
	}
	const auto output = Run("Arithmetic", input);
	for (size_t i = 0; i < input.size(); ++i)
	{
		CAPTURE(i);
		Interval a{input[i].Lower.x, input[i].Upper.x}, b{input[i].Lower.y, input[i].Upper.y};
		const std::array<Interval, 4> expected{TraceReference::Add(a, b), TraceReference::Subtract(a, b), TraceReference::Multiply(a, b), TraceReference::Wrap(input[i].Lower.z)};
		for (size_t op = 0; op < expected.size(); ++op)
		{
			CheckBits(output[i][op * 2], expected[op].Lo);
			CheckBits(output[i][op * 2 + 1], expected[op].Hi);
		}
		const std::array<Exact, 3> nominal{Exact(a.Lo) * Exact(b.Lo), Exact(a.Lo) + Exact(b.Lo), Exact(a.Lo) - Exact(b.Lo)};
		for (size_t op = 0; op < nominal.size(); ++op)
		{
			const auto rounded = nominal[op].Rounded();
			if (output[i][8 + op] == 0 && std::abs(rounded) <= std::numeric_limits<float>::min())
				continue; // signed zero and permitted underflow flush do not alter Wrap's interval
			CheckBits(output[i][8 + op], rounded);
		}
		if (i >= nestedStart && (i - nestedStart) % 2 == 0)
			for (size_t op = 0; op < 3; ++op)
			{
				CHECK(output[i + 1][op * 2] <= output[i][op * 2]);
				CHECK(output[i + 1][op * 2 + 1] >= output[i][op * 2 + 1]);
			}
	}
}

TEST_CASE("TraceNumerics: transformed GPU bounds contain exact vertices and nested boxes")
{
	if (!RequireRTE())
		return;
	std::mt19937 random(94182);
	std::vector<Input> input;
	for (uint32_t i = 0; i < 160; ++i)
	{
		Input child;
		for (int axis = 0; axis < 3; ++axis)
		{
			child.Lower[axis] = float(int(random() % 129) - 64) * 0.125f;
			child.Upper[axis] = child.Lower[axis] + float(random() % 17) * 0.125f;
		}
		for (auto* row : {&child.RowX, &child.RowY, &child.RowZ})
			for (int column = 0; column < 4; ++column)
				(*row)[column] = float(int(random() % 129) - 64) * 0.25f;
		if (i % 4 == 0)
			child.RowX = glm::vec4(std::numeric_limits<float>::min(), -std::numeric_limits<float>::min(), 0, 0);
		if (i % 4 == 1)
			child.RowY = glm::vec4(0.0000001f, -0.0000001f, 0, 499900);
		Input parent = child;
		parent.Lower -= glm::vec4(1);
		parent.Upper += glm::vec4(1);
		input.push_back(child);
		input.push_back(parent);
	}
	const auto output = Run("Bounds", input);
	for (size_t i = 0; i < input.size(); ++i)
	{
		CAPTURE(i);
		const std::array<glm::vec4, 3> rows{input[i].RowX, input[i].RowY, input[i].RowZ};
		for (size_t axis = 0; axis < 3; ++axis)
		{
			const auto expected = ReferenceRow(input[i], rows[axis]);
			CheckBits(output[i][axis * 2], expected.Lo);
			CheckBits(output[i][axis * 2 + 1], expected.Hi);
			CHECK(std::abs(output[i][axis * 2]) < std::ldexp(1.0f, 47));
			for (uint32_t corner = 0; corner < 8; ++corner)
			{
				Exact position(rows[axis].w);
				for (int column = 0; column < 3; ++column)
					position = position + Exact(rows[axis][column]) * Exact((corner & (1u << column)) ? input[i].Upper[column] : input[i].Lower[column]);
				CHECK(Exact(expected.Lo).Compare(position) <= 0);
				CHECK(Exact(expected.Hi).Compare(position) >= 0);
			}
			CHECK(output[i][8 + axis] >= expected.Lo);
			CHECK(output[i][8 + axis] <= expected.Hi);
			if (i % 2 == 0)
			{
				CHECK(output[i + 1][axis * 2] <= output[i][axis * 2]);
				CHECK(output[i + 1][axis * 2 + 1] >= output[i][axis * 2 + 1]);
			}
		}
		if (i % 2 == 0 && output[i + 1][7] == 0)
			CHECK(output[i][7] == 0);
	}
}

TEST_CASE("TraceNumerics: GPU gate preserves exact rational intersections and uniform query bypass")
{
	if (!RequireRTE())
		return;
	std::vector<Input> input;
	std::mt19937 random(31771);
	for (uint32_t i = 0; i < 512; ++i)
	{
		Input value;
		for (int axis = 0; axis < 3; ++axis)
		{
			value.Lower[axis] = float(int(random() % 33) - 16) * 0.25f;
			value.Upper[axis] = value.Lower[axis] + float(random() % 17) * 0.25f;
			value.AnchorMaximum[axis] = float(int(random() % 33) - 16) * 0.25f;
			value.Offset[axis] = float(int(random() % 9) - 4) * 0.125f;
			value.Direction[axis] = float(int(random() % 9) - 4) * 0.5f;
		}
		value.AnchorMaximum.w = float(random() % 17) * 0.5f;
		if (i % 3 == 0)
		{
			value.AnchorMaximum = glm::vec4(glm::vec3(value.Lower), 0);
			value.Offset = glm::vec4(0);
			value.Direction = glm::vec4(0);
		}
		input.push_back(value);
		value.Lower -= glm::vec4(0.5f);
		value.Upper += glm::vec4(0.5f);
		input.push_back(value);
	}
	const size_t nestedCount = input.size();
	// Closed endpoint, split origin cancellation, tiny off-axis component and next-representable T.
	Input point;
	point.Lower = point.Upper = glm::vec4(1, 0, 0, 0);
	point.Direction = glm::vec4(1, 0, 0, 0);
	input.push_back(point);
	point.AnchorMaximum.w = std::nextafter(1.0f, 0.0f);
	input.push_back(point);
	point.AnchorMaximum = glm::vec4(4194304, 0, 0, 1);
	point.Offset.x = -4194304;
	input.push_back(point);
	point.Direction.y = std::numeric_limits<float>::min();
	point.Lower.y = point.Upper.y = std::numeric_limits<float>::min();
	input.push_back(point);
	// Equality of the widened separating intervals is intentionally retained, even though the exact
	// segment falls just short. This distinguishes the strict contract from non-strict separation.
	const size_t closedWidenedContact = input.size();
	point = Input{};
	point.Lower = point.Upper = glm::vec4(std::bit_cast<float>(0x00800003u), 0, 0, 0);
	point.Direction = glm::vec4(std::numeric_limits<float>::min(), 0, 0, 0);
	input.push_back(point);
	const size_t crossAxisSeparated = input.size();
	point = Input{};
	point.Lower = glm::vec4(0.8f, 0.1f, -1, 0);
	point.Upper = glm::vec4(0.9f, 0.2f, 1, 0);
	point.Direction = glm::vec4(1, 1, 0, 0);
	input.push_back(point);
	for (float outside : {std::numeric_limits<float>::denorm_min(), 8388608.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
		for (uint32_t field = 0; field < 4; ++field)
		{
			Input value;
			value.Lower = value.Upper = glm::vec4(100);
			if (field == 0)
				value.AnchorMaximum.x = outside;
			if (field == 1)
				value.Offset.y = outside;
			if (field == 2)
				value.Direction.z = outside;
			if (field == 3)
				value.AnchorMaximum.w = outside;
			input.push_back(value);
		}
	const auto output = Run("Gate", input);
	CHECK(output[closedWidenedContact][1] == 1);
	CHECK(output[crossAxisSeparated][1] == 0); // all coordinate projections overlap; a cross axis separates
	size_t hits = 0, rejected = 0;
	for (size_t i = 0; i < input.size(); ++i)
	{
		CAPTURE(i);
		const auto& value = input[i];
		const auto domain = TraceNumericDomain::ClassifyQuery(glm::vec3(value.AnchorMaximum), glm::vec3(value.Offset), glm::vec3(value.Direction), value.AnchorMaximum.w);
		CHECK(output[i][0] == float(domain));
		if (domain == TraceQueryDomain::Certified)
		{
			std::array<Interval, 3> box;
			std::array<float, 3> anchor, offset, direction;
			for (int axis = 0; axis < 3; ++axis)
			{
				box[axis] = {value.Lower[axis], value.Upper[axis]};
				anchor[axis] = value.AnchorMaximum[axis];
				offset[axis] = value.Offset[axis];
				direction[axis] = value.Direction[axis];
			}
			if (TraceReference::Intersects(box, anchor, offset, direction, value.AnchorMaximum.w))
			{
				++hits;
				CHECK(output[i][1] == 1);
			}
			rejected += output[i][1] == 0;
		}
		else
			CHECK(output[i][1] == (domain == TraceQueryDomain::BypassAllBounds ? 1 : 0));
		if (i < nestedCount && i % 2 == 0 && output[i + 1][1] == 0)
			CHECK(output[i][1] == 0);
	}
	CHECK(hits > 300);
	CHECK(rejected > 300);
}

TEST_CASE("TraceNumerics: query limits and adjacent inputs have explicit whole-query outcomes")
{
	std::vector<Input> input;
	std::vector<TraceQueryDomain> expected;
	for (int field = 0; field < 4; ++field)
		for (int axis = 0; axis < (field == 3 ? 1 : 3); ++axis)
			for (float sign : {-1.0f, 1.0f})
			{
				const float limit = field == 2 ? 2.0f : 4194304.0f;
				const std::array<float, 9> values{0.0f, -0.0f, std::numeric_limits<float>::min(), std::nextafter(std::numeric_limits<float>::min(), 0.0f), std::nextafter(limit, 0.0f), limit, std::nextafter(limit, std::numeric_limits<float>::infinity()), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()};
				for (size_t i = 0; i < values.size(); ++i)
				{
					Input value;
					value.Lower = value.Upper = glm::vec4(100);
					const float scalar = sign * values[i];
					if (field == 0)
						value.AnchorMaximum[axis] = scalar;
					if (field == 1)
						value.Offset[axis] = scalar;
					if (field == 2)
						value.Direction[axis] = scalar;
					if (field == 3)
						value.AnchorMaximum.w = scalar;
					TraceQueryDomain result = TraceQueryDomain::Certified;
					if (i == 3 || i == 6 || (field == 3 && sign < 0 && i >= 2))
						result = TraceQueryDomain::BypassAllBounds;
					if (i >= 7)
						result = TraceQueryDomain::Invalid;
					input.push_back(value);
					expected.push_back(result);
					CAPTURE(field);
					CAPTURE(axis);
					CAPTURE(i);
					CAPTURE(sign);
					CHECK(TraceNumericDomain::ClassifyQuery(glm::vec3(value.AnchorMaximum), glm::vec3(value.Offset), glm::vec3(value.Direction), value.AnchorMaximum.w) == result);
				}
			}
	if (!RequireRTE())
		return;
	const auto output = Run("Gate", input);
	for (size_t i = 0; i < input.size(); ++i)
	{
		CAPTURE(i);
		CHECK(output[i][0] == float(expected[i]));
		if (expected[i] != TraceQueryDomain::Certified)
			CHECK(output[i][1] == (expected[i] == TraceQueryDomain::BypassAllBounds ? 1 : 0));
	}
}

TEST_CASE("TraceNumerics: GPU unions enclose both children without arithmetic widening")
{
	if (!RequireRTE())
		return;
	std::vector<Input> input;
	for (float magnitude : {std::numeric_limits<float>::min(), std::bit_cast<float>(0x00800001u), 1.0f, 1048576.0f})
	{
		Input value;
		value.Lower = glm::vec4(-magnitude, 0, magnitude, 0);
		value.Upper = glm::vec4(0, magnitude, magnitude, 0);
		value.RowX = glm::vec4(0, -magnitude, -magnitude, 0);
		value.RowY = glm::vec4(magnitude, 0, -magnitude, 0);
		input.push_back(value);
		std::swap(value.Lower, value.RowX);
		std::swap(value.Upper, value.RowY);
		input.push_back(value);
		value.RowX = value.Lower;
		value.RowY = value.Upper;
		input.push_back(value);
	}
	const auto output = Run("UnionBounds", input);
	for (size_t i = 0; i < input.size(); ++i)
		for (int axis = 0; axis < 3; ++axis)
		{
			CheckBits(output[i][axis * 2], std::min(input[i].Lower[axis], input[i].RowX[axis]));
			CheckBits(output[i][axis * 2 + 1], std::max(input[i].Upper[axis], input[i].RowY[axis]));
		}
}
