#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include "TestUtilities.h"

int main(int argc, char** argv)
{
	Paralax::Testing::InitTestLogging();

	doctest::Context context;
	context.applyCommandLine(argc, argv);
	const int result = context.run();

	// Destroy the shared GPU device deterministically, while logging is still available and before leak checks.
	Paralax::Testing::ReleaseTestGraphicsDevice();

	Paralax::Log::Shutdown();
	std::error_code error;
	std::filesystem::remove_all(Paralax::Testing::GetTestRoot(), error);
	return result;
}
