#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "TestUtilities.h"

int main(int argc, char** argv)
{
	Paralax::Testing::InitTestLogging();

	doctest::Context context;
	context.applyCommandLine(argc, argv);
	const int result = context.run();

	Paralax::Log::Shutdown();
	return result;
}
