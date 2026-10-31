---
name: pre-commit-review
description: Mandatory review of staged changes before every Paralax commit: correctness, Hazel style compliance, tests, production-quality checklist. Use before running git commit.
---

# Pre-commit review

The project requires a code review before every commit. Do it on the staged diff (`git diff --cached`), or on the full tree for an initial commit.

For non-trivial changes, get an independent read: launch a review subagent and give it the diff and this checklist, **not** your own conclusions. Verify each finding yourself before acting on it.

## Checklist

**Correctness**
- [ ] Look for logic errors, off-by-one errors, wrong conditions, integer overflow and signed/unsigned mistakes.
- [ ] Check lifetimes: no dangling references or pointers, callbacks don't outlive what they capture, and ownership is clear (`Scope`/`Ref`, no raw owning pointers).
- [ ] Every error path is handled. Expected failures return or log; they don't assert. Asserts have no side effects.
- [ ] Thread-safety is documented, and shared state is protected.
- [ ] Resources are released on every path, including partial-construction failure.

**Tests**
- [ ] Every new or changed behaviour has a test, including edge cases and invalid input.
- [ ] The tests fail without the change. Check this mentally or by reverting.
- [ ] The tests are deterministic: no wall-clock timing assumptions, and a fixed timestep for app-level tests.
- [ ] Everything passes in `debug`, `asan`, `release` and Dist-with-tests (see `build-and-test`).
- [ ] When a component or scripting API is added, the feature test scene covers it.

**Style** (AGENTS.md)
- [ ] Naming: `m_`/`s_` prefixes, PascalCase types and functions, camelCase locals, `PX_` macros.
- [ ] `clang-format-18 --dry-run --Werror` is clean.
- [ ] Engine `.cpp` files start with `#include "pxpch.h"`, and include order is respected.
- [ ] No platform `#ifdef` outside `Platform/` and `Core/PlatformDetection.h` / `Core/Config.h`.
- [ ] Comments explain why. Public API states ownership, threading and failure behaviour.

**Production quality**
- [ ] No TODO hacks, dead code, debugging leftovers or commented-out code.
- [ ] Warnings stay clean on GCC, Clang and MSVC: no implicit narrowing, use `static_cast`.
- [ ] Portability: no compiler extensions, and no reliance on libc++ features that older Apple toolchains lack (e.g. floating-point `std::from_chars`).
- [ ] No secrets, private files, `GameEngineDoc.md` or `papers-inspiration/` in the commit.

Record significant review outcomes in the commit message body when they explain design choices.
