# Editor logic independent of the UI (context, commands, undo): shared by the editor executable and the tests.
file(GLOB_RECURSE PX_EDITOR_CORE_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/EditorCore/*.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/EditorCore/*.h")
add_library(Paralax-EditorCore STATIC ${PX_EDITOR_CORE_SOURCES})
target_include_directories(Paralax-EditorCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(Paralax-EditorCore PUBLIC Paralax::Paralax)
px_configure_target(Paralax-EditorCore)

# The editor UI (the editor layer and its panels): a library so the tests can attach the layer and drive the commands
# it registers, as the executable does.
file(GLOB_RECURSE PX_EDITOR_UI_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
list(FILTER PX_EDITOR_UI_SOURCES EXCLUDE REGEX "/EditorCore/|/EditorApp\\.cpp$")
add_library(Paralax-EditorUI STATIC ${PX_EDITOR_UI_SOURCES})
target_link_libraries(Paralax-EditorUI PUBLIC Paralax-EditorCore)
px_configure_target(Paralax-EditorUI)

add_executable(Paralax-Editor "${CMAKE_CURRENT_SOURCE_DIR}/src/EditorApp.cpp")
target_link_libraries(Paralax-Editor PRIVATE Paralax-EditorUI)
px_configure_target(Paralax-Editor)
