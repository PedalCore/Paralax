#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Core/Base.h>

using namespace Paralax;
using Paralax::Testing::AssertionFailedError;
using Paralax::Testing::CaptureAssertion;
using Paralax::Testing::ScopedAssertionCapture;

// Conditions read volatile variables so they are not constant expressions; otherwise MSVC warns about constant
// conditional expressions (C4127) inside the assert macros.

namespace {

	volatile int s_Two = 2;
	volatile bool s_True = true;
	volatile bool s_False = false;

}

TEST_CASE("Assertion: passing asserts and verifies do not report a failure")
{
	const int two = s_Two;
	const bool truth = s_True;

	CHECK_FALSE(CaptureAssertion([&] { PX_CORE_ASSERT(two + two == 4); }).has_value());
	CHECK_FALSE(CaptureAssertion([&] { PX_ASSERT(truth, "message {}", 42); }).has_value());
	CHECK_FALSE(CaptureAssertion([&] { PX_CORE_VERIFY(truth); }).has_value());
	CHECK_FALSE(CaptureAssertion([&] { PX_VERIFY(truth, "message"); }).has_value());
}

TEST_CASE("Assertion: a failing verify reports expression, location and formatted message")
{
	const int two = s_Two;
	int expectedLine = 0;

	std::optional<AssertionInfo> info = CaptureAssertion([&] {
		expectedLine = __LINE__ + 1;
		PX_CORE_VERIFY(two + two == 5, "math is {} today", "broken");
	});

	REQUIRE(info.has_value());
	CHECK(std::string(info->Expression) == "two + two == 5");
	CHECK(std::string(info->File).find("AssertTests.cpp") != std::string::npos);
	CHECK(info->Line == expectedLine);
	CHECK(info->Message == "math is broken today");
}

TEST_CASE("Assertion: a failing verify without a message has an empty message")
{
	const bool falsehood = s_False;
	std::optional<AssertionInfo> info = CaptureAssertion([&] { PX_VERIFY(falsehood); });
	REQUIRE(info.has_value());
	CHECK(info->Message.empty());
	CHECK(std::string(info->Expression) == "falsehood");
}

TEST_CASE("Assertion: AssertionFailedError describes the failure")
{
	ScopedAssertionCapture capture;
	const bool falsehood = s_False;
	try
	{
		PX_VERIFY(falsehood, "value was {}", 7);
	}
	catch (const AssertionFailedError& e)
	{
		CHECK(std::string(e.what()) == "falsehood: value was 7");
		return;
	}
	FAIL("PX_VERIFY did not report a failure");
}

TEST_CASE("Assertion: verify always evaluates its condition exactly once")
{
	int evaluations = 0;
	PX_CORE_VERIFY(++evaluations == 1);
	CHECK(evaluations == 1);
}

TEST_CASE("Assertion: asserts are active outside Dist builds and compiled out in Dist")
{
	int evaluations = 0;
	std::optional<AssertionInfo> info = CaptureAssertion([&] { PX_CORE_ASSERT(++evaluations == 0, "fails"); });

#if defined(PX_ENABLE_ASSERTS)
	CHECK(info.has_value());
	CHECK(evaluations == 1);
#else
	CHECK_FALSE(info.has_value());
	CHECK(evaluations == 0);
#endif
}

TEST_CASE("Assertion: ScopedAssertionCapture installs a handler and restores the default on exit")
{
	REQUIRE_FALSE(Assertion::HasCustomHandler());
	{
		ScopedAssertionCapture capture;
		CHECK(Assertion::HasCustomHandler());
	}
	CHECK_FALSE(Assertion::HasCustomHandler());

	Assertion::SetHandler([](const AssertionInfo&) { throw AssertionFailedError(AssertionInfo{}); });
	CHECK(Assertion::HasCustomHandler());
	Assertion::ResetHandler();
	CHECK_FALSE(Assertion::HasCustomHandler());
}
