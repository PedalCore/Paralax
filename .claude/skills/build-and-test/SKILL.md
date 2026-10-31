---
name: build-and-test
description: Configure, build and test Paralax on the dev VM (debug/release/dist/asan presets, single tests, headless runs, CI results). Use whenever code is changed or a test needs running.
---

# Build and test Paralax

All builds run on the Linux dev VM; Windows and macOS are covered by CI. Never build on the user's Mac.

## Everyday loop

```bash
export CPM_SOURCE_CACHE=$HOME/.cache/CPM
cmake --preset debug -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++   # first time only
cmake --build build/debug
ctest --preset debug -j8
```

## Before committing (required)

```bash
cmake --preset asan && cmake --build build/asan && ctest --preset asan -j8        # GCC + ASan/UBSan
cmake --preset tsan -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ && cmake --build build/tsan && ctest --preset tsan -j8   # ThreadSanitizer
cmake --preset release && cmake --build build/release && ctest --preset release -j8
# Dist with tests: verifies asserts compile out and Dist-only code paths
cmake -S . -B build/dist-tests -G Ninja -DCMAKE_BUILD_TYPE=Dist -DPX_BUILD_TESTS=ON
cmake --build build/dist-tests && ctest --test-dir build/dist-tests -j8
git ls-files '*.h' '*.cpp' | xargs clang-format-18 --dry-run --Werror
```

## Focused runs

- One test case or a group: `build/debug/bin/Paralax-Tests -tc="LayerStack:*"`
- Doctest options: `-s` shows successful assertions, `-d` shows durations, `--list-test-cases` lists the tests.
- Headless app run: `build/debug/bin/Paralax-Editor --headless --frames 10 --fixed-timestep 0.016`
- Real window under a virtual display: `xvfb-run -a build/debug/bin/Paralax-Editor --frames 30`
- The software Vulkan device (lavapipe) is installed for renderer tests: `vulkaninfo --summary`

## New source files

Sources are globbed with `CONFIGURE_DEPENDS`. A new `.cpp` file is picked up on the next build; re-running configure is never needed for that.

## Checking CI

The `gh` CLI is not authenticated on the VM. Use the public REST API instead (60 requests/hour unauthenticated):

```bash
curl -s "https://api.github.com/repos/PedalCore/Paralax/actions/runs?per_page=3" | python3 -c 'import json,sys; [print(r["id"], r["head_sha"][:8], r["status"], r["conclusion"]) for r in json.load(sys.stdin)["workflow_runs"]]'
curl -s "https://api.github.com/repos/PedalCore/Paralax/actions/runs/<RUN_ID>/jobs" | python3 -c 'import json,sys; [print(j["name"], j["conclusion"]) for j in json.load(sys.stdin)["jobs"]]'
```

Job logs require authentication. When a job fails, read its failing step through the check-run annotations (`/repos/PedalCore/Paralax/check-runs/<JOB_ID>/annotations`), or ask the user for a fine-grained token with Actions read access.
