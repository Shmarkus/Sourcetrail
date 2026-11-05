# C and C++ indexing

Date: 2026-11-05. Status: approved in chat (the user asked to skip the questions; the calls below are mine).

## Why

The fork should appeal to a wider audience. Sourcetrail's original strength was C and C++; the base (upstream
3e090d9b5) still contains the Clang-based C/C++ indexer (`src/lib_cxx`), switched off in this fork because it needs
Clang's development libraries.

## Goals

1. Builds and packages ship the C/C++ indexer (`BUILD_CXX_LANGUAGE_PACKAGE=ON`), linked against LLVM/Clang 19's shared
   libraries from apt.llvm.org (`libclang-cpp.so.19`, `libLLVM.so.19`), bundled into the package like the other libraries.
   Upstream's own C/C++ tests run green in CI.
2. C/C++ projects are set up from a folder like the others: New Project detects a `compile_commands.json` (at the root
   or in a build folder such as `build/`) and creates a Compilation Database source group. A `CMakeLists.txt` without a
   compilation database gets a clear message on how to produce one (`cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`).
   The existing wizard's C/C++ source-group types (empty C/C++, CDB) are available in New Custom Project.
3. `st` works on C/C++: names use `::` (`ns::Class::method`), overloads by parameter list, `find`, `callers`, `callees`,
   `impact`, `refs`, `outline`, `impls`/`supers` (derived/base classes, overrides). Freshness and refresh follow the
   compilation database and headers as Sourcetrail already tracks them (C/C++ source groups refresh per file, not as a
   whole group).
4. Mixed projects (e.g. a C++ library next to Java or Python) keep each language's refresh isolated as today.

## Non-goals

- Building Clang ourselves; Windows/macOS; Visual Studio solution import; C/C++ ↔ other-language linking.
- Generating compilation databases for build systems other than CMake (Make/Bear etc. are documented, not automated).

## Anti-goals

- Java, TypeScript and Python indexing, refresh or `st` output change.
- The package stops running on Ubuntu 22.04 (the glibc/libstdc++ guard must still pass with the LLVM libraries).
- Private names in committed files.

## Constraints

- Upstream's C/C++ code is used as is where it works; fixes for LLVM 19 (or 18 for local development) stay small.
- Every commit builds and passes; tests first; one review per task.

## Left to the builder

- Whether local development uses the installed LLVM 18 dev packages (if the base compiles against them) or LLVM 19
  (the user would need to `sudo apt install` it — ask via the controller).
- How the package bundles the LLVM shared libraries (likely automatic through `bundleLibraries.sh`).
