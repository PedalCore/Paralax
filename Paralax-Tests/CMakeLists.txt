file(GLOB_RECURSE PX_TEST_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
# Editor tests only exist when the editor is built.
if(NOT TARGET Paralax-EditorCore)
	list(FILTER PX_TEST_SOURCES EXCLUDE REGEX "/src/Editor/")
endif()

add_executable(Paralax-Tests ${PX_TEST_SOURCES})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${PX_TEST_SOURCES})

target_include_directories(Paralax-Tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(Paralax-Tests PRIVATE Paralax::Paralax doctest::doctest nvrhi)
if(TARGET Paralax-EditorCore)
	target_link_libraries(Paralax-Tests PRIVATE Paralax-EditorCore)
	# project.export copies the runtime from beside the running executable.
	add_dependencies(Paralax-Tests Paralax-Runtime)
endif()
target_compile_definitions(Paralax-Tests PRIVATE DOCTEST_CONFIG_SUPER_FAST_ASSERTS
	# A FLAC sample from the pinned miniaudio sources, for the decoder tests (not copied into this repository).
	PX_TEST_FLAC_FILE="${miniaudio_SOURCE_DIR}/data/16-44100-stereo.flac")
# Tests that read files from the source tree get its absolute path here, never from __FILE__: compiler caches such as
# ccache (base_dir) make __FILE__ relative to the build directory. Only these sources get it, so the other test
# sources stay identical across worktrees and share cache hits.
set_source_files_properties(
	"${CMAKE_CURRENT_SOURCE_DIR}/src/Renderer/SceneRendererTests.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/Scripting/FeatureTestCoverageTests.cpp"
	PROPERTIES COMPILE_DEFINITIONS "PX_SOURCE_ROOT=\"${PROJECT_SOURCE_DIR}\"")
px_configure_target(Paralax-Tests)

# doctest_discover_tests turns the test list into a CMake list, so a ';' in a test name registers two CTest tests
# that match no case and pass without running anything. Refuse such names here instead, whichever doctest macro
# declares the case (the name is the first string literal of the macro's arguments).
foreach(source IN LISTS PX_TEST_SOURCES)
	file(STRINGS "${source}" PX_TEST_NAMES_WITH_SEMICOLON REGEX "(TEST_CASE(_FIXTURE|_TEMPLATE)?|SCENARIO)\\([^\"]*\"[^\"]*;")
	if(PX_TEST_NAMES_WITH_SEMICOLON)
		message(FATAL_ERROR "Test case names must not contain ';' (CTest could not run them): ${source}: ${PX_TEST_NAMES_WITH_SEMICOLON}")
	endif()
endforeach()

# Register each doctest test case as its own CTest test so failures are reported individually.
include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")
doctest_discover_tests(Paralax-Tests
	WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
	ADD_LABELS 1)

# Windowed smoke test: real Vulkan swapchain presentation under a virtual X server, with validation enabled
# (validation errors make the editor exit non-zero). Only registered where xvfb-run is available (Linux).
find_program(PX_XVFB_RUN xvfb-run)
# X11/xcb are uninstrumented and report spurious lock errors under ThreadSanitizer; the windowed tests cover
# windowing and presentation (not engine threading), so they are not registered in TSan builds.
if(PX_SANITIZERS MATCHES "thread")
	set(PX_REGISTER_WINDOWED_TESTS OFF)
else()
	set(PX_REGISTER_WINDOWED_TESTS ON)
endif()
if(PX_XVFB_RUN AND PX_REGISTER_WINDOWED_TESTS AND TARGET Paralax-Editor)
	add_test(NAME "Smoke: windowed editor presents 120 frames under Xvfb with validation"
		COMMAND "${PX_XVFB_RUN}" -a -s "-screen 0 1280x720x24" $<TARGET_FILE:Paralax-Editor> --frames 120 --validation
		WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
	set_tests_properties("Smoke: windowed editor presents 120 frames under Xvfb with validation" PROPERTIES
		TIMEOUT 300
		LABELS "smoke;gpu"
		ENVIRONMENT "LSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/tools/sanitizers/lsan.supp")
endif()

# Agent control end to end: a Python agent launches the headless editor and drives it over JSON-RPC and MCP.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND AND TARGET Paralax-Editor)
	add_test(NAME "Smoke: an agent drives the headless editor over JSON-RPC and MCP"
		COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/agent/tests/agent_smoke_test.py"
			$<TARGET_FILE:Paralax-Editor> "${CMAKE_CURRENT_BINARY_DIR}/TestOutput/Agent")
	# PYTHONDONTWRITEBYTECODE keeps __pycache__ out of the source tree.
	set_tests_properties("Smoke: an agent drives the headless editor over JSON-RPC and MCP" PROPERTIES
		TIMEOUT 300
		LABELS "smoke;gpu"
		ENVIRONMENT "LSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/tools/sanitizers/lsan.supp;PYTHONDONTWRITEBYTECODE=1")

	# An agent builds a small game from nothing (scripts, physics, audio, input), plays and iterates on it, and
	# exports it.
	add_test(NAME "Smoke: an agent builds, playtests and exports a small game"
		COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/agent/tests/agent_game_test.py"
			$<TARGET_FILE:Paralax-Editor> "${CMAKE_CURRENT_BINARY_DIR}/TestOutput/AgentGame")
	set_tests_properties("Smoke: an agent builds, playtests and exports a small game" PROPERTIES
		TIMEOUT 600
		LABELS "smoke;gpu"
		ENVIRONMENT "LSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/tools/sanitizers/lsan.supp;PYTHONDONTWRITEBYTECODE=1")

	# "Build me Tetris": tools/agent/examples/tetris built through commands alone, then played with injected input.
	add_test(NAME "Smoke: an agent builds Tetris through commands and plays it"
		COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/agent/tests/tetris_test.py"
			$<TARGET_FILE:Paralax-Editor> "${CMAKE_CURRENT_BINARY_DIR}/TestOutput/Tetris")
	set_tests_properties("Smoke: an agent builds Tetris through commands and plays it" PROPERTIES
		TIMEOUT 900
		LABELS "smoke;gpu"
		ENVIRONMENT "LSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/tools/sanitizers/lsan.supp;PYTHONDONTWRITEBYTECODE=1")
	# Importing a multi-object glTF scene (as Blender exports one) as editable entities with model.create_prefab.
	add_test(NAME "Smoke: an agent imports a glTF scene as entities and views it through its camera"
		COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/agent/tests/model_import_test.py"
			$<TARGET_FILE:Paralax-Editor> "${CMAKE_CURRENT_BINARY_DIR}/TestOutput/ModelImport")
	set_tests_properties("Smoke: an agent imports a glTF scene as entities and views it through its camera" PROPERTIES
		TIMEOUT 600
		LABELS "smoke;gpu"
		ENVIRONMENT "LSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/tools/sanitizers/lsan.supp;PYTHONDONTWRITEBYTECODE=1")
	# The physics counterpart: tools/agent/examples/gem_roller (bodies, triggers, a moving platform, fixed-rate control).
	add_test(NAME "Smoke: an agent builds the Gem Roller physics game through commands and plays it"
		COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/agent/tests/gem_roller_test.py"
			$<TARGET_FILE:Paralax-Editor> "${CMAKE_CURRENT_BINARY_DIR}/TestOutput/GemRoller")
	set_tests_properties("Smoke: an agent builds the Gem Roller physics game through commands and plays it" PROPERTIES
		TIMEOUT 900
		LABELS "smoke;gpu"
		ENVIRONMENT "LSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/tools/sanitizers/lsan.supp;PYTHONDONTWRITEBYTECODE=1")
endif()

# The feature test scene: the runtime plays it headless with validation. Its script asserts the whole scripting API
# and fails the run (exit code 4) on any script error.
if(TARGET Paralax-Runtime)
	add_test(NAME "FeatureTest: the runtime plays the feature scene headless without errors"
		COMMAND $<TARGET_FILE:Paralax-Runtime> --project "${CMAKE_CURRENT_SOURCE_DIR}/FeatureTest" --headless --frames 60
			--fixed-timestep 0.0166667 --validation --fail-on-script-error --screenshot "${CMAKE_CURRENT_BINARY_DIR}/TestOutput/FeatureTest.png"
		WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
	set_tests_properties("FeatureTest: the runtime plays the feature scene headless without errors" PROPERTIES
		TIMEOUT 300
		LABELS "smoke;gpu"
		ENVIRONMENT "LSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/tools/sanitizers/lsan.supp")

	# The converse, which proves the check above can fail: cut short, the feature script's OnDestroy reports that it
	# did not finish, and the run must exit with exactly the script error code (not merely fail, or crash). The runtime
	# runs in its own directory (which the wrapper creates, so deleting TestOutput is harmless) so that its
	# Logs/Runtime.log does not interleave with the other runtime tests' when run in parallel.
	add_test(NAME "FeatureTest: a run too short to finish the feature scene exits with the script error code"
		COMMAND "${CMAKE_COMMAND}" -DEXPECTED_EXIT_CODE=4 "-DEXPECTED_OUTPUT=did not run to completion" -DTIMEOUT_SECONDS=240
			"-DWORKING_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/TestOutput/FeatureTestShortRun"
			-P "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ExpectExitCode.cmake" --
			$<TARGET_FILE:Paralax-Runtime> --project "${CMAKE_CURRENT_SOURCE_DIR}/FeatureTest" --headless --frames 10
			--fixed-timestep 0.0166667 --fail-on-script-error
		WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
	set_tests_properties("FeatureTest: a run too short to finish the feature scene exits with the script error code" PROPERTIES
		TIMEOUT 300
		LABELS "smoke;gpu"
		ENVIRONMENT "LSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/tools/sanitizers/lsan.supp")
endif()

# Export end to end: the editor exports the feature test as a standalone game (to a path relative to the working
# directory, as a user would type it), which then runs from its own folder with no --project: it finds the bundled
# project beside the executable. Each run replaces the previous export.
if(TARGET Paralax-Editor)
	add_test(NAME "Export: the editor exports the feature test as a standalone game"
		COMMAND $<TARGET_FILE:Paralax-Editor> --headless --project "${CMAKE_CURRENT_SOURCE_DIR}/FeatureTest" --export ExportedFeatureTest
		WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
	add_test(NAME "Export: the exported feature test plays from its own folder without errors"
		COMMAND "${CMAKE_CURRENT_BINARY_DIR}/ExportedFeatureTest/FeatureTest$<TARGET_FILE_SUFFIX:Paralax-Runtime>" --headless --frames 60
			--fixed-timestep 0.0166667 --validation --fail-on-script-error
		WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
	set_tests_properties("Export: the editor exports the feature test as a standalone game" PROPERTIES
		TIMEOUT 300
		LABELS "smoke;gpu"
		FIXTURES_SETUP ExportedFeatureTest
		ENVIRONMENT "LSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/tools/sanitizers/lsan.supp")
	set_tests_properties("Export: the exported feature test plays from its own folder without errors" PROPERTIES
		TIMEOUT 300
		LABELS "smoke;gpu"
		FIXTURES_REQUIRED ExportedFeatureTest
		ENVIRONMENT "LSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/tools/sanitizers/lsan.supp")
endif()

# Non-ASCII paths on the command line (Unicode on Windows, where argv is in the ANSI code page): the editor creates,
# opens and exports projects at such paths, and the runtime plays the project and the export from them. The work
# directory is outside TestOutput, which CI uploads, because it holds an exported game.
if(TARGET Paralax-Editor)
	set(PX_NON_ASCII_TEST_ARGUMENTS
		"-DEDITOR=$<TARGET_FILE:Paralax-Editor>"
		"-DRUNTIME=$<TARGET_FILE:Paralax-Runtime>"
		"-DEXECUTABLE_SUFFIX=$<TARGET_FILE_SUFFIX:Paralax-Runtime>"
		"-DFEATURE_TEST=${CMAKE_CURRENT_SOURCE_DIR}/FeatureTest"
		"-DWORK=${CMAKE_CURRENT_BINARY_DIR}/NonAsciiCommandLine")
	add_test(NAME "Command line: the editor creates, opens and exports projects at non-ASCII paths"
		COMMAND "${CMAKE_COMMAND}" ${PX_NON_ASCII_TEST_ARGUMENTS} -DMODE=editor -P "${CMAKE_CURRENT_SOURCE_DIR}/scripts/NonAsciiCommandLineTest.cmake")
	add_test(NAME "Command line: the runtime plays a project and its export from non-ASCII paths"
		COMMAND "${CMAKE_COMMAND}" ${PX_NON_ASCII_TEST_ARGUMENTS} -DMODE=runtime -P "${CMAKE_CURRENT_SOURCE_DIR}/scripts/NonAsciiCommandLineTest.cmake")
	set_tests_properties("Command line: the editor creates, opens and exports projects at non-ASCII paths" PROPERTIES
		TIMEOUT 600
		LABELS "smoke;gpu"
		FIXTURES_SETUP NonAsciiCommandLine
		ENVIRONMENT "LSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/tools/sanitizers/lsan.supp")
	set_tests_properties("Command line: the runtime plays a project and its export from non-ASCII paths" PROPERTIES
		TIMEOUT 600
		LABELS "smoke;gpu"
		FIXTURES_REQUIRED NonAsciiCommandLine
		ENVIRONMENT "LSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/tools/sanitizers/lsan.supp")
endif()

# Without --project and with no game beside it (the build directory has none), the runtime refuses to start.
if(TARGET Paralax-Runtime)
	add_test(NAME "Runtime: without a project beside it the runtime exits with an error"
		COMMAND $<TARGET_FILE:Paralax-Runtime> --headless --frames 1)
	# Matching the message (rather than any failure) proves it failed for this reason; that path exits non-zero.
	set_tests_properties("Runtime: without a project beside it the runtime exits with an error" PROPERTIES
		PASS_REGULAR_EXPRESSION "No game to run"
		TIMEOUT 60
		ENVIRONMENT "LSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/tools/sanitizers/lsan.supp")
endif()

# Windowed tests: native windows and real swapchains. They need a display, so they are registered as one CTest
# test run under xvfb-run where available (Linux).
add_executable(Paralax-WindowedTests
	"${CMAKE_CURRENT_SOURCE_DIR}/windowed/WindowedSwapchainTests.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/TestMain.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/GPUTestContext.cpp")
target_include_directories(Paralax-WindowedTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(Paralax-WindowedTests PRIVATE Paralax::Paralax doctest::doctest nvrhi)
px_configure_target(Paralax-WindowedTests)

if(PX_XVFB_RUN AND PX_REGISTER_WINDOWED_TESTS)
	add_test(NAME "Windowed: swapchain tests under Xvfb"
		COMMAND "${PX_XVFB_RUN}" -a -s "-screen 0 1280x720x24" $<TARGET_FILE:Paralax-WindowedTests>
		WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
	set_tests_properties("Windowed: swapchain tests under Xvfb" PROPERTIES
		TIMEOUT 300
		LABELS "windowed;gpu"
		ENVIRONMENT "LSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/tools/sanitizers/lsan.supp")
endif()
