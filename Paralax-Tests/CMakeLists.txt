file(GLOB_RECURSE PX_TEST_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

add_executable(Paralax-Tests ${PX_TEST_SOURCES})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${PX_TEST_SOURCES})

target_include_directories(Paralax-Tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(Paralax-Tests PRIVATE Paralax::Paralax doctest::doctest nvrhi)
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
if(PX_XVFB_RUN AND TARGET Paralax-Editor)
	add_test(NAME "Smoke: windowed editor presents 120 frames under Xvfb with validation"
		COMMAND "${PX_XVFB_RUN}" -a -s "-screen 0 1280x720x24" $<TARGET_FILE:Paralax-Editor> --frames 120 --validation
		WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
	set_tests_properties("Smoke: windowed editor presents 120 frames under Xvfb with validation" PROPERTIES
		TIMEOUT 300
		LABELS "smoke;gpu"
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

if(PX_XVFB_RUN)
	add_test(NAME "Windowed: swapchain tests under Xvfb"
		COMMAND "${PX_XVFB_RUN}" -a -s "-screen 0 1280x720x24" $<TARGET_FILE:Paralax-WindowedTests>
		WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
	set_tests_properties("Windowed: swapchain tests under Xvfb" PROPERTIES
		TIMEOUT 300
		LABELS "windowed;gpu"
		ENVIRONMENT "LSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/tools/sanitizers/lsan.supp")
endif()
