file(GLOB_RECURSE PX_TEST_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

add_executable(Paralax-Tests ${PX_TEST_SOURCES})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${PX_TEST_SOURCES})

target_include_directories(Paralax-Tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(Paralax-Tests PRIVATE Paralax::Paralax doctest::doctest)
target_compile_definitions(Paralax-Tests PRIVATE DOCTEST_CONFIG_SUPER_FAST_ASSERTS)
px_configure_target(Paralax-Tests)

# Register each doctest test case as its own CTest test so failures are reported individually.
include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")
doctest_discover_tests(Paralax-Tests
	WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
	ADD_LABELS 1)
