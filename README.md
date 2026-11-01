# Paralax

A 3D game engine written in C++20 for **Windows, macOS and Linux**, with an editor, a runtime that exports games,
and an editor that AI agents can drive end to end.

> Status: early development. In place:
> - the core application layer and windowing
> - a PBR renderer (NVRHI/Vulkan) with shadows and image-based lighting
> - an ECS scene with prefabs
> - the editor and agent control
> - Lua scripting, Jolt physics and miniaudio sound
> - the runtime and project export
>
> Further renderer features are next.

## Tech stack

- C++20, CMake (3.25+), dependencies pinned via [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake)
- [GLFW](https://github.com/glfw/glfw) windowing, [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) over Vulkan for rendering
- [glm](https://github.com/g-truc/glm) math, [spdlog](https://github.com/gabime/spdlog) logging, [doctest](https://github.com/doctest/doctest) tests
- [EnTT](https://github.com/skypjack/entt) scenes, [Jolt Physics](https://github.com/jrouwe/JoltPhysics), [Lua](https://www.lua.org) 5.4 scripting via [sol2](https://github.com/ThePhD/sol2), [fastgltf](https://github.com/spnda/fastgltf) import, [Dear ImGui](https://github.com/ocornut/imgui) editor UI
- [miniaudio](https://github.com/mackron/miniaudio) audio

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

Export a game as a standalone folder (the runtime, `Resources/` and the project), then run it from there:

```bash
build/release/bin/Paralax-Editor --headless --project path/to/MyGame --export path/to/MyGameBuild
path/to/MyGameBuild/MyGame
```

Scripting is documented in [docs/Scripting.md](docs/Scripting.md).

## Contributing

See [AGENTS.md](AGENTS.md) for the code style (Hazel conventions), testing rules and the commit workflow.

## License

Paralax is released under the [MIT License](LICENSE). Third-party dependencies keep their own licences.

Built-in HDR environments are [Kloofendal Partly Cloudy](https://polyhaven.com/a/kloofendal_48d_partly_cloudy_puresky)
and [Studio Small 09](https://polyhaven.com/a/studio_small_09), from Poly Haven under [CC0](https://polyhaven.com/license).
CMake downloads the pinned 2k files and copies them into `Resources/Environments/` beside the executables.
Distribute that directory with the runtime; `PARALAX_RESOURCE_DIR` overrides its location. Missing environments are
reported once and use the procedural sky. Image loading is capped at 128 MiB encoded, 256 MiB for the decoded base
level and 16384 pixels per axis. HDR loading accepts top-to-bottom RGBE flat pixels or scanline RLE, with headers
limited to 64 KiB and lines to 1023 bytes.
