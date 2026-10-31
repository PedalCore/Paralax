add_executable(Paralax-Editor "${CMAKE_CURRENT_SOURCE_DIR}/src/EditorApp.cpp")
target_link_libraries(Paralax-Editor PRIVATE Paralax::Paralax)
px_configure_target(Paralax-Editor)
