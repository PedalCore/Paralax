---
name: dependency-update
description: Add or bump a third-party dependency in Paralax (CPM pinning policy, verification, licensing). Use when touching cmake/Dependencies.cmake.
---

# Adding or updating a dependency

Policy: every dependency is pinned to an **exact release tag** in `cmake/Dependencies.cmake`. Never use a branch, `latest` or a version range. CPM itself is vendored in `cmake/CPM.cmake`.

1. **Choose a version.** Use a released tag that is at least two weeks old. List tags with `git ls-remote --tags --refs https://github.com/<owner>/<repo>`.
2. **Check the licence.** It must be permissive (MIT, BSD, zlib, Apache-2.0 or similar). Record new licences in the README.
3. **Add the package.**
   - Use `CPMAddPackage` with `GITHUB_REPOSITORY` and `GIT_TAG` (or `VERSION`).
   - Turn off the dependency's tests, examples, docs and install.
   - Add its targets to the `SYSTEM`/folder loop, so our `-Werror` doesn't apply to its headers.
4. **Link it** `PRIVATE` unless its types appear in Paralax public headers.
5. **Check the packaged version.** If it isn't packaged as source (e.g. a binary SDK), document how CI and the dev VM get it, and pin that version too.
6. **Verify.**
   - Run a clean configure with an empty build dir.
   - Build and test all presets (`build-and-test`).
   - Push and confirm that all three OSes are green in CI. The CPM cache key changes automatically, because it hashes `Dependencies.cmake`.
