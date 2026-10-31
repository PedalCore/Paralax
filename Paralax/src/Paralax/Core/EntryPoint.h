#pragma once

// Defines main(). Include in exactly one translation unit of an executable that implements
// Paralax::CreateApplication().

#include "Paralax/Core/Application.h"

#include <cstdlib>

int main(int argc, char** argv)
{
	// Applications add their own log file via ApplicationSpecification::LogFile (after WorkingDirectory applies).
	Paralax::Log::Init();

	int exitCode = EXIT_SUCCESS;
	{
		Paralax::Scope<Paralax::Application> app = Paralax::CreateApplication(Paralax::CommandLine(argc, argv, Paralax::GetStandardFlagOptions()));
		if (app && app->IsInitialized())
		{
			app->Run();
			exitCode = app->GetExitCode();
		}
		else
		{
			exitCode = EXIT_FAILURE;
		}
	}

	Paralax::Log::Shutdown();
	return exitCode;
}
