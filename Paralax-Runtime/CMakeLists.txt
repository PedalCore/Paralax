add_executable(Paralax-Runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/RuntimeApp.cpp")
target_link_libraries(Paralax-Runtime PRIVATE Paralax::Paralax)
px_configure_target(Paralax-Runtime)
