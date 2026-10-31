# Paralax

A 3D game engine written in C++20 for **Windows, macOS and Linux**, with an editor, a runtime that exports games,
and an editor that AI agents can drive end to end.

> Status: early development. The core application layer, windowing, events and input are in place; the renderer,
> scene/ECS, physics, scripting and audio are next.

## Tech stack

- C++20, CMake (3.25+), dependencies pinned via [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake)
- [GLFW](https://github.com/glfw/glfw) windowing, [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) over Vulkan for rendering
- [glm](https://github.com/g-truc/glm) math, [spdlog](https://github.com/gabime/spdlog) logging, [doctest](https://github.com/doctest/doctest) tests
- Planned: [Jolt Physics](https://github.com/jrouwe/JoltPhysics), Lua scripting, [miniaudio](https://github.com/mackron/miniaudio), glTF import

## Building

```bash
cmake --preset debug
cmake --build build/debug
ctest --preset debug
```

Presets: `debug`, `release`, `dist` (distribution build without editor or tests) and `asan` (sanitizers).

On Linux, GLFW needs the X11 and Wayland development packages:

```bash
sudo apt install libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libwayland-dev libxkbcommon-dev wayland-protocols
```

Run the editor without a display (CI, agents): `build/debug/bin/Paralax-Editor --headless --frames 10`.

## Contributing

See [AGENTS.md](AGENTS.md) for the code style (Hazel conventions), testing rules and the commit workflow.

## License

Paralax is released under the [MIT License](LICENSE). Third-party dependencies keep their own licences.
