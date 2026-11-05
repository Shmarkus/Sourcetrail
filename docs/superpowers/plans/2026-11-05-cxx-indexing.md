# C and C++ Indexing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the base's Clang-based C/C++ indexer back on, with project setup from a folder, `st` support and packaging.

**Architecture:** Build with `BUILD_CXX_LANGUAGE_PACKAGE=ON` against apt.llvm.org's LLVM/Clang 19 shared libraries; reuse upstream's `lib_cxx` and CDB source group; extend ProjectSetup detection and `st` name handling for `::`.

**Tech Stack:** C++20/Qt 6.8, LLVM/Clang 19 (apt.llvm.org), CMake presets, gtest.

**Spec:** `docs/superpowers/specs/2026-11-05-cxx-indexing-design.md`

## Global Constraints

- Java/TypeScript/Python behaviour and `st` output unchanged (compare-index identical on testing/cli/{lombok,maven,angular-spring,python}; st goldens).
- Package still passes the Ubuntu 22.04 glibc/libstdc++ guard and runs in a clean 22.04 container.
- Fixture `testing/cli/cpp`: a small CMake project (namespace `shop`, classes with inheritance and virtual overrides, overloads, a template, free functions, a header-only part, a test executable) with a committed way to produce `compile_commands.json`.
- No private names; tests first; attribution lines; never `git add -A`; push only when the user asks.

## Review Focus

1. A `compile_commands.json` with relative paths, or one in `build/` rather than the root: detected and indexed. Task 2.
2. Editing a header refreshes the files that include it (existing Sourcetrail behaviour) and `st` sees the header edit as stale. Task 2.
3. `st` keys with `::`, operators (`operator==`), templates (`shop::Box<T>::get`), overloads (`f(int)` vs `f(double)`). Task 2.
4. A project mixing C++ and Python/Java: refresh isolation per language holds. Task 2.
5. Package on clean 22.04 with the bundled LLVM libraries: `sourcetrail index` of the fixture works. Task 3.

---

### Task 1: Build and test with the C/C++ indexer on

- [ ] Configure `fork_release` with `BUILD_CXX_LANGUAGE_PACKAGE=ON`, finding Clang via `find_package(Clang CONFIG)` (system LLVM; `Clang_DIR` override kept). Fix compile errors against the locally installed LLVM 18 if small; if not feasible, stop and report what LLVM 19 packages are needed (the controller asks the user to install them).
- [ ] Upstream's C/C++ tests (lib_cxx tests, integration cxx suites) run in `ctest --preset fork_release` and pass.
- [ ] Fixture `testing/cli/cpp` + a node of `compile_commands.json` generation (`cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON` in tests, or a committed relative-path CDB). Headless `Sourcetrail index` of a CDB project on the fixture: 0 errors, expected symbols present (test).
- [ ] compare-index identical for the existing Java/TS/Python fixtures.
- [ ] Commit.

### Task 2: Project setup and st for C/C++

- [ ] ProjectSetup: `DetectedBuild` gains `cxx` (a `compile_commands.json` at the root or in `build/`, `cmake-build-*/`, `out/build/*`); createProject writes a CDB source group (with the CDB path relative); `CMakeLists.txt` without a CDB → message with the cmake command. GUI New Project + Add C/C++ to an existing project; wizard types available. Tests first.
- [ ] `st`: `::` keys (Review Focus 3), freshness covers C/C++ sources and headers and the CDB file, `st status` row `cxx` only for C/C++ groups; Java/TS/Python goldens unchanged. Tests first on the fixture.
- [ ] Mixed-language isolation test (Focus 4); header edit refresh (Focus 2).
- [ ] GUI check under Xvfb on a copy of the fixture.
- [ ] Commit.

### Task 3: CI, packaging, docs

- [ ] CI: install `clang-19 libclang-19-dev llvm-19-dev` from apt.llvm.org on the ubuntu-22.04 runner (add the apt source and key in the job), configure with C/C++ on, cache as needed.
- [ ] Package bundles libclang-cpp.so.19 and libLLVM.so.19 (check size); glibc guard passes; clean 22.04 container: index the fixture.
- [ ] st-smoke C++ section.
- [ ] Docs in the user's style: README support list (C/C++ supported), DOCUMENTATION.md C/C++ entry (setup via compile_commands.json, refresh, limits). Results doc `docs/superpowers/specs/2026-11-05-cxx-indexing-results.md`: fixture + one public C/C++ project at a pinned commit (e.g. a small library like `fmt` or `nlohmann/json` tests) — files, errors, index time, refresh after a header edit, a few st answers.
- [ ] Commit (no push).
