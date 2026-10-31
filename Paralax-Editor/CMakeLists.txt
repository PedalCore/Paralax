# Editor logic independent of the UI (context, commands, undo): shared by the editor executable and the tests.
file(GLOB_RECURSE PX_EDITOR_CORE_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/EditorCore/*.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/EditorCore/*.h")
add_library(Paralax-EditorCore STATIC ${PX_EDITOR_CORE_SOURCES})
target_include_directories(Paralax-EditorCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(Paralax-EditorCore PUBLIC Paralax::Paralax)
px_configure_target(Paralax-EditorCore)

file(GLOB_RECURSE PX_EDITOR_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
list(FILTER PX_EDITOR_SOURCES EXCLUDE REGEX "/EditorCore/")
add_executable(Paralax-Editor ${PX_EDITOR_SOURCES})
target_link_libraries(Paralax-Editor PRIVATE Paralax-EditorCore)
px_configure_target(Paralax-Editor)
