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
endif()
target_compile_definitions(Paralax-Tests PRIVATE DOCTEST_CONFIG_SUPER_FAST_ASSERTS)
px_configure_target(Paralax-Tests)

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
