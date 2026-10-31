---
name: add-engine-feature
description: Playbook for adding a new engine module, class, component or system to Paralax with matching tests and test-scene coverage. Use when implementing any new engine feature.
---

# Adding an engine feature

1. **Design first.** Decide which module it belongs to (`Paralax/src/Paralax/<Module>/`) and whether it needs a platform backend (`Paralax/src/Platform/<Backend>/`, hidden behind an interface plus a `Create()` factory, as `Window` / `GLFWWindow` are). For large features (renderer passes, GI, scripting), agree the design with the user first.
2. **Header.** Add `<Thing>.h`:
   - `#pragma once` and `namespace Paralax {`
   - an `XxxSpecification` struct when it has creation-time configuration
   - doc comments on ownership, threading and failure behaviour
3. **Implementation.** `<Thing>.cpp` starts with `#include "pxpch.h"`, then its own header. Keep file-local helpers in an anonymous namespace.
4. **Expose.** Add client-facing headers to `Paralax/src/Paralax.h`.
5. **Tests.** Add `Paralax-Tests/src/<Module>/<Thing>Tests.cpp`:
   - Name each case `"<Thing>: <behaviour>"`.
   - Use `ScopedAssertionCapture` for precondition checks and `ScopedLogCapture` for logged errors (both in `TestUtilities.h`).
   - App-level tests use a headless `ApplicationSpecification` with `FixedTimestep` and `MaxFrames`, and drive frames through layers and `SubmitToMainThread`.
6. **Agent control.** If the feature has an editor or user-facing side, register commands so agents can drive it (command registry, scripting, CLI).
7. **Feature test scene.** If it is a component, system or scripting function, add it to the feature test scene and assert on its effect.
8. **Verify** with the `build-and-test` skill, then run the `pre-commit-review` skill, then commit.
