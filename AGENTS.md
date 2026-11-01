# Paralax: Agent & Contributor Guide

Paralax is a production-grade 3D game engine in C++20 for Windows, macOS and Linux (Ubuntu 24.04+).
It has to be stable and fully tested before anyone ships with it. No hacks, no shortcuts.

This file is the source of truth for how work is done in this repository. Read it before every task.

## Repository layout

| Path | What |
|---|---|
| `Paralax/` | Core engine **static library** (`Paralax::Paralax`). All engine code lives here. |
| `Paralax/src/Paralax/<Module>/` | Engine modules (`Core`, `Events`, `Renderer`, `Scene`, `Asset`, `Scripting`, `ImGui`, `Network`, `Project`, and later `Physics`, `Audio`). |
| `docs/` | Reference documentation (e.g. `Scripting.md`, the Lua API). |
| `Paralax/src/Platform/<Backend>/` | Platform/backend implementations (`GLFW`, later `Vulkan`). Never include these from module headers. |
| `Paralax/src/pxpch.h` | Precompiled header. Every engine `.cpp` starts with `#include "pxpch.h"`. |
| `Paralax/src/Paralax.h` | Public umbrella header for client code. |
| `Paralax-Editor/` | Editor executable. It is excluded from distribution builds. `src/EditorCore/` is the UI-independent part (context, commands, undo, agent server) and is linked into the tests. |
| `Paralax-Runtime/` | Runtime executable that plays exported games. |
| `Paralax-Tests/` | doctest unit and integration tests (`Paralax-Tests` executable). Each test case is registered with CTest. |
| `cmake/` | Build modules. `Dependencies.cmake` pins every third-party package. |
| `.github/workflows/ci.yml` | CI: format check, plus Linux, Windows and macOS builds, tests and headless smoke runs. |
| `tools/agent/` | Python client (`paralax_agent.py`) and MCP server (`paralax_mcp.py`) for driving the editor; standard library only. |
| `.claude/skills/` | Task playbooks (build/test, pre-commit review, adding features, dependencies). |

Never commit `GameEngineDoc.md` or `papers-inspiration/` (private brief and third-party papers). Both are excluded in `.git/info/exclude`.

## Build & test

Dependencies are fetched by CPM at configure time. Set `CPM_SOURCE_CACHE=~/.cache/CPM` so all build trees share one download.

```bash
cmake --preset debug && cmake --build build/debug && ctest --preset debug -j8
```

| Preset | Purpose |
|---|---|
| `debug` | Day-to-day development. Asserts and logging on. |
| `release` | Optimised. Asserts still on. |
| `dist` | Distribution: asserts compiled out, no editor, no tests. |
| `asan` | Debug + AddressSanitizer + UndefinedBehaviorSanitizer (GCC/Clang). |
| `tsan` | Debug + ThreadSanitizer (GCC/Clang). |

- Build configurations are **Debug / Release / Dist** only. They define `PX_DEBUG`, `PX_RELEASE` or `PX_DIST`.
- Warnings are errors (`PX_WARNINGS_AS_ERRORS=ON`) on every compiler. Fix warnings; never silence them with pragmas unless the warning comes from a third-party header, and comment why.
- Headless runs need no display server: `Paralax-Editor --headless --frames 10`. Tests always use headless windows (GLFW null platform).
- Before you commit, build and test at least `debug` (clang) and `asan` (gcc). CI covers MSVC and AppleClang; check CI after you push.
- Never build on the user's Mac. Everything runs on the dev VM or in CI.

## Renderer conventions

- **API.** Rendering goes through NVRHI over Vulkan 1.3. Only `Paralax/src/Platform/Vulkan/` touches raw Vulkan, and it does so through function pointers loaded at runtime (no SDK or loader linked). Only one `GraphicsDevice` may exist per process.
- **Shaders.** Shaders are written in **Slang** in `Paralax/shaders/*.slang`. Each file is one module with one or more `[shader("...")]` entry points, compiled to SPIR-V at build time and embedded (look them up with `ShaderLibrary`). Shared code goes in `Paralax/shaders/Common/` as `module`s, pulled in with `import Common.X;`. Bindings use HLSL `register(tN|sN|bN|uN)` syntax; the build shifts them to NVRHI's Vulkan binding offsets.
- **Conventions.** Clip space is D3D-style: +Y up and depth 0..1. Texture UV (0,0) is the top-left. Matrices are column-major (glm). Back buffers are 8-bit sRGB: write linear colour and the hardware encodes it.
- **GPU tests.**
  - Use `GPUTestContext.h`. GPU tests run on lavapipe headless with validation, and must leave the validation error count unchanged.
  - If no Vulkan device is available they pass with a warning, unless `PX_REQUIRE_GPU=1` is set, as it is on Linux CI.
  - Tests write images to `TestOutput/` in the test working directory. CI uploads them as artifacts.
- **Automated runs.** `--validation` makes any validation error exit with code 3. The Xvfb smoke test exercises real swapchain presentation.

## Scenes, components and assets

- **Components** are plain structs in `Scene/Components.h`. To add one:
  1. Register it in `Scene/ComponentRegistry.cpp`, with reflected fields via `Reflection::MakeField`.
  2. Give it tests, including a JSON round trip.
  3. Add it to the feature test scene.

  The reflection layer is the single source for scene files, the inspector, scripting and the agent API; never hand-write per-component serialization.
- **Hierarchy** goes through `Scene` (`SetParent`, `SetSiblingIndex`, `DestroyEntity`). Never edit `RelationshipComponent` directly. Tree walks must be iterative, because hierarchies can be very deep.
- **Engine resources** are large built-in files, such as HDR environments, that ship beside the executables in `Resources/`: never embed them in the binary, and never commit them. Pin them in `cmake/Dependencies.cmake`, have the build copy them, and locate them with `GetEngineResourceDirectory()`. A missing resource is an error that is reported once; it is never a crash. Small files (shaders, UI fonts) are embedded with `px_embed_files`.
- **Assets** are referenced by `AssetHandle` (UUID). Handles 1–1023 are reserved for built-ins (see `AssetManager`). Files live under the project's asset directory and are listed in `AssetRegistry.json`. glTF materials and textures are sub-assets with deterministic handles.
- **Untrusted input.** Every loader (scene, material, registry, glTF, image) treats files as untrusted:
  - check JSON types before reading (nlohmann's `value()` throws)
  - bounds-check every buffer access
  - keep paths inside the project
  - cap sizes and depths

  Add a hostile-input test for each new loader.

## Scripting, prefabs and runtime systems

- **Runtime systems** (scripting now; physics and audio later) implement `SceneSystem` and are added to the running copy of a scene before `OnRuntimeStart`. `EditorContext::StartPlaying` and the runtime player do this. The scene calls them in order and defers destruction requested during callbacks. Never call into a system from `Scene` code directly.
- **Lua** (5.4, built as C++ so errors unwind properly) is bound with sol2, only in `Scripting/ScriptSystem.cpp`; keep sol2 out of headers.
  - Bindings read and write components through reflection (`ComponentRegistry::SetField`), so validation matches scene files and agents.
  - Inside bindings, report errors with `ScriptError(...)`. It raises a Lua error, and the system catches it at the call boundary.
- **Every new scripting API function** must be added to `docs/Scripting.md`, to the tests in `Paralax-Tests/src/Scripting/`, and to the feature test scene.
- **Sandbox:** scripts never get file, OS, `debug`, `load` or bytecode access. Randomness comes from the seeded `math.random`, and time from `Time`, never the wall clock.
- **Physics:**
  - Jolt (cross-platform deterministic build) lives only in `Physics/PhysicsSystem.cpp`, behind a pimpl. Bodies come from Collider and Rigidbody components; the system syncs them to the scene each update.
  - Never step Jolt or touch bodies elsewhere: scripts use the `Physics` table, and the engine uses `PhysicsSystem`'s methods.
  - Contact events are derived from per-step contact sets, which keeps them deterministic and stable across sleep.
  - Physics tests run real simulations in fixed steps and check results against analytic expectations within tolerances.
- **Audio:**
  - miniaudio lives only in `Audio/` (`AudioClip.cpp`, `AudioSystem.cpp`), behind a pimpl. Clips are decoded in full when loaded.
  - `AudioSystem` mixes on a device, or offline in step with simulated time. Offline is used for headless runs and tests, and when there is no device.
  - When a sound ends follows simulated time, never the mixer, so scripts behave the same with or without a device.
  - Agents choose play mode's output with `audio.set_output` and inspect it with `audio.status`.
  - Audio tests run offline and check the mixed samples (`AudioSystemSpecification::MaxCapturedFrames`); they never depend on a sound card.
- **Prefabs** (`.pxprefab`) are scene documents with type "Prefab" and exactly one root. Instantiate them through `PrefabAsset::Instantiate`, never by copying JSON by hand.

## Editor and agent control

- **Commands.** Every editor action is a command in `EditorCore/CommandRegistry` (JSON parameters in, JSON result out), registered in `BuiltinCommands.cpp` or, for UI-owned state such as the camera and renderer settings, by `EditorLayer` while it is attached. The UI, the console, the agent server and the MCP bridge all go through `EditorContext::Execute`. A command that sets `ModifiesScene` gets an undo step and is rolled back if it fails.
- **Undo** stores serialized scene snapshots. Rollback, undo and redo *replace* the `Scene` object, so never hold a `Scene&` or component reference across `Execute`. UI panels call `EditorContext::Defer` while drawing; `EditorLayer` runs the queue after all panels. Multi-frame UI edits (drags, typing, the gizmo) use `ApplyInteractiveEdit`/`TouchInteractiveEdit` so one interaction is one undo step.
- **Paths from commands** are untrusted: resolve scene paths with `EditorContext::ResolveProjectPath` and output files with `ResolveOutputPath`.
- **Agent server.** `Paralax-Editor --agent-port 0` serves JSON-RPC 2.0 on 127.0.0.1 (one request per line) and writes the port and token to `<project>/.paralax/AgentServer.json` (or `--agent-info <file>`). The first request must be `session.authenticate`. Every command is a method; `help` lists them. `editor.quit` exits.
- **Play mode for agents:** `play.start { paused: true }`, then `play.step { frames, dt }`, keeps runs deterministic. `script.eval` inspects the running game and `script.errors` lists script failures.
- **Export.** `project.export { output }` (or `Paralax-Editor --project <dir> --export <dir>`) writes a standalone game folder. It contains:
  - the runtime, renamed after the project
  - `Resources/`
  - the project file and `Assets/`, and nothing else from the project

  The runtime plays the single `.pxproj` beside it. Export is the one command that writes outside the project, so it is deliberately narrow:
  - the output's parent must exist
  - it must not overlap the project or the engine resources (checked by file identity, not only by name)
  - it replaces only an empty folder, or a previous export (marked by `.paralax-export`, which lists its files) that holds nothing else
  - it is assembled in a sibling staging folder and swapped in, so a failure never leaves a half-written export
- **MCP.** `python3 tools/agent/paralax_mcp.py --launch <Paralax-Editor> -- --headless --project <dir>` (or `--info <AgentServer.json>`) exposes every command as an MCP tool; `render_capture` returns the image. The CTest smoke test drives the editor through both.

## Code style (Hazel conventions)

The style follows [Hazel](https://github.com/TheCherno/Hazel), and `.clang-format` enforces the layout. Run `clang-format-18 -i` on every file you touch. CI rejects unformatted code.

**Naming**

| Thing | Convention | Example |
|---|---|---|
| Namespaces | PascalCase; everything lives in `Paralax` | `namespace Paralax {` |
| Types (class, struct, enum, alias) | PascalCase | `class LayerStack`, `using KeyCode = uint16_t;` |
| Functions and methods | PascalCase | `void PushLayer(...)`, `static Application& Get()` |
| Private/protected member variables | `m_` + PascalCase | `m_LayerStack` |
| Static member and file-static variables | `s_` + PascalCase | `s_Instance` |
| Global variables (avoid) | `g_` + PascalCase | `g_Allocator` |
| Public struct fields (data/specification structs) | PascalCase, no prefix | `WindowSpecification::Width` |
| Local variables and parameters | camelCase | `frameCount`, `const Event& event` |
| Enum values | PascalCase (`enum class` preferred) | `EventType::WindowResize` |
| Compile-time constants | PascalCase | `inline constexpr KeyCode Count = 349;` |
| Macros | `PX_` + UPPER_SNAKE | `PX_CORE_ASSERT`, `PX_BIND_EVENT_FN` |
| Template parameters | PascalCase | `template<typename T, typename... Args>` |
| Files | PascalCase, named after the main type | `LayerStack.h` / `LayerStack.cpp` |

**Layout**
- Indent with tabs (width 4). Braces go on their own line (Allman), except `namespace Paralax {`. Namespace contents are indented.
- Use `#pragma once`. No include guards.
- Include order in `.cpp` files: `pxpch.h`, then the file's own header, then other Paralax headers, then third-party, then the standard library. Separate the groups with blank lines.
- Engine headers include engine headers as `"Paralax/..."`. Client code (editor, runtime, tests) uses `<Paralax/...>` or `<Paralax.h>`.
- Specification structs (`XxxSpecification`) configure objects at creation, as in `WindowSpecification` and `ApplicationSpecification`.
- Use `Scope<T>` / `CreateScope` (unique ownership) and `Ref<T>` / `CreateRef` (shared ownership). Use no raw owning pointers. Raw pointers and references are non-owning.
- Prefer `enum class`. Unscoped enums are only for bit flags, like `EventCategory`.
- Mark single-argument constructors `explicit` unless implicit conversion is the point (`Timestep`, `UUID`).
- Delete copy operations on types that own resources.

## Engineering rules

1. **Testing is mandatory.** Every behaviour change ships with unit tests in `Paralax-Tests/src/<Module>/<Thing>Tests.cpp`. Name tests `"<Thing>: <behaviour in plain English>"`. Cover:
   - edge cases
   - invalid input
   - the asserted precondition, with `ScopedAssertionCapture` + `CHECK_THROWS_AS(..., AssertionFailedError)`
   - Dist behaviour where it differs
2. **Feature test scene.** Once scenes exist, every component, system and scripting API function must also appear in the feature test scene, which is run headless in CI. That scene is the integration test for "everything the engine can do". If you add a feature, extend the scene.
3. **Asserts vs verifies vs errors.**
   - `PX_CORE_ASSERT` / `PX_ASSERT`: programmer-error preconditions. Compiled out in Dist, and must not have side effects.
   - `PX_CORE_VERIFY` / `PX_VERIFY`: always evaluated and checked. Use them where continuing would be undefined behaviour or would lose data.
   - Expected runtime failures (missing files, bad user data, malformed command lines, no display server) are *not* asserts. Return `bool`, `std::optional`, `nullptr` from a `Create()` factory, or an error result, and log with `PX_CORE_ERROR`. `Window::Create` and `Application::IsInitialized()` show the pattern.
   - A failed assert or verify never continues. A custom `Assertion` handler must throw or terminate; if it returns, the process aborts.
4. **No exceptions in engine code paths.** Use the `std::error_code` overloads of `std::filesystem`. Where a third-party API throws, catch it at the boundary. Tests may use exceptions through the assertion capture.
5. **Threading.** Engine objects are main-thread-only unless documented otherwise. To get work onto the main thread from another thread, use `Application::SubmitToMainThread`. Document thread-safety on every public class that is not main-thread-only.
6. **Determinism.** Simulation must be reproducible under `ApplicationSpecification::FixedTimestep`. Tests and golden images depend on it. Never read wall-clock time in simulation code; use the `Timestep`.
7. **Layer stack changes are deferred while iterating.** Calling `Application::PushLayer`/`PopLayer` from `OnUpdate`/`OnEvent` is safe: the change applies when that iteration finishes. Never hold iterators into the layer stack across user callbacks.
8. **Input is event-driven.** Read input only through `Paralax::Input`, never by polling GLFW directly. That keeps synthetic events from agents, tests and replays identical to real input.
9. **Agent-controllable.** Every editor action must be reachable programmatically: through the command registry, scripting and the CLI. A feature an agent can't drive is unfinished.
10. **Cross-platform.** No platform `#ifdef`s outside `Platform/` and `Core/PlatformDetection.h` / `Core/Config.h`. macOS has no Vulkan ray tracing and no mesh shaders, so every renderer feature needs a non-RT path.
11. **Dependencies** are pinned to exact versions in `cmake/Dependencies.cmake` (see the `dependency-update` skill). Never use code from elsewhere on the machine as a reference.
12. **Comments** explain *why*, not *what*. Public API gets a short doc comment that states ownership, threading and failure behaviour.

## Commit workflow

1. Build and test (see above). All tests must pass. Never commit with failing or skipped tests.
2. Run the **pre-commit review** (`.claude/skills/pre-commit-review`). It covers correctness, style compliance, test coverage and production quality. Fix everything it finds before committing.
3. `clang-format-18 --dry-run --Werror` must be clean.
4. Before committing, check `git diff --cached --stat` and confirm only the intended files are staged; `git mv` stages immediately. When committing part of the working tree, build and test the commit in a clean worktree (`git worktree add`) before pushing.
5. Make small, focused commits. Write the message in the imperative mood: one summary line, a blank line, then the why.
6. Push to `origin` (`git@github.com:PedalCore/Paralax.git`), then check CI. A red CI is fixed before anything else.
