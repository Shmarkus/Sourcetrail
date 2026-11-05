# C and C++ indexing: results

Measured on Nov 5, 2026, with the release package (`createPackages.sh` on the `fork_release_cxx` build, LLVM 19.1.7
from apt.llvm.org), in the `st-ci-2204-cxx` image (Ubuntu 22.04, gcc 13, CMake 3.31, 20 cores, default indexer settings:
as many threads as cores, multi-process C/C++ indexing) with `--shm-size=4g` (see below), on two projects:
- **The fixture**, `testing/cli/cpp`: a CMake project, namespace `shop` (abstract `Item` with `Book` and `Food`
  overriding it, `operator==`, a header-only template `Box<T>`, overloads of `discount`, `Order`, an app and a test
  executable).
- **`fmt` 11.0.2** (commit `0c9fce2ffefecfdce794e1859584e25877b7b592`), configured with
  `cmake -B build -G Ninja -DFMT_TEST=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`: the library (`src/`, 14 headers in
  `include/fmt`, 17,028 lines), its tests (11,850 lines) and the bundled googletest amalgamation (14,434 lines).

The project file was written as New Project writes it: `.sourcetrail/<name>.srctrlprj`, a "C/C++ from Compilation
Database" group with `../build/compile_commands.json`, indexed header path `..` and the exclude `../build/_deps/**`.
Spec: `2026-11-05-cxx-indexing-design.md`.

To reproduce the environment: `st-ci-2204-cxx` is `ubuntu:22.04` with the "Install build tools" and "Install LLVM 19"
steps of `.github/workflows/build.yml` run in it (as root, without `sudo`), plus JDK 21, Node.js 22.13 or newer and
`scripts/fork/bootstrap.sh`; run it with `--init --shm-size=4g`.

## Projects

| | Fixture | fmt |
|---|---|---|
| Compile commands (source files) | 5 (5) | 49 (29; the same test main is in several targets) |
| `st index --full` | `indexed 5 files, 0 errors` | `indexed 49 files, 0 errors` |
| `st index --full` wall time | 2.1 s | 10.1 s |
| Indexed files (`st status` `files`), all files seen | 8, 227 | 51, 490 |
| Nodes, edges | 343, 842 | 71,345, 316,381 |
| `unresolved_symbols` | 0 | 0 (7,283 before the fix below) |
| Header edit, then a query | `box.hpp`: `refreshed 1 files in 2.0s` | `include/fmt/os.h`: 31 of 49 sources re-indexed, `refreshed 1 files in 10.1s` (indexer 6.6 s) |
| Source edit, then a query | | `src/os.cc`: `refreshed 1 files in 6.1s` |
| `st status` | 0.03 s | 0.33 s |

"refreshed 1 files" counts the changed files; the refresh re-indexes every source that includes a changed header.
After `os.h`, which `src/os.cc`, `test/util.h` and `test/gtest-extra.h` include, that is most of the tests. Before the
query `st status --no-refresh` showed `stale_files 1`, after it 0. The indexer's own log for the header edit:
`Finished indexing: 31/31 source files indexed; 00:00:06:640; 0 errors`. An edit of `src/format.cc`: 2 sources
(it is in two compile commands), 1.4 s in the indexer.

`st status` checks whether the index holds a source file of the compilation database and now stops at the first one it
finds, instead of canonicalising every entry first; on fmt's 49 entries the whole command takes 0.33 s, most of it
loading the index.

## `st` answers

| Project | Command | Result |
|---|---|---|
| Fixture | `callers 'shop::Box<T>::get'` | 2: `main() via shop::Box<double>::get()` `app/main.cpp:10`, `main() via shop::Box<int>::get()` `tests/shop_tests.cpp:26` |
| Fixture | `callers 'shop::discount(double, double)'` | 2: `shop::discount(double, int)` `src/pricing.cpp:10`, `main()` `tests/shop_tests.cpp:23` (also without the space) |
| Fixture | `callers 'shop::operator=='` | 1: `main()` `tests/shop_tests.cpp:27` |
| Fixture | `callees 'shop::totalWithTax(const Order&)'` | `shop::Order::total()`, `shop::boxed<double>(double)` |
| Fixture | `supers shop::Book::price`, `impls shop::Item` | `shop::Item::price()` `include/shop/item.hpp:13`; `shop::Book`, `shop::Food` |
| Fixture | `callers main` | exit 2: two `main()` keys, one per program, that `st` cannot tell apart |
| fmt | `callers 'fmt::v11::detail::format_error_code' --depth 2` | 7: `format_system_error`, `report_system_error` and 4 gtest test bodies, plus `testing::Test::Run() via testing::Test::TestBody()` |
| fmt | `impls fmt::v11::detail::buffer` | 4: `iterator_buffer`, `counting_buffer`, `basic_memory_buffer` and the tests' `mock_buffer` |
| fmt | `outline fmt::v11::basic_memory_buffer` | 15 members with access and line ranges |
| fmt | `impact fmt::v11::report_system_error` | 11 symbols in 5 files: the destructors of `buffered_file` and `file`, then the test that calls it up to gtest's runner |
| fmt | `callers 'fmt::v11::vformat(string_view, format_args)'` | 294, nearly all instantiations of `fmt::v11::format<...>` at one line, `include/fmt/format.h:4365` |
| fmt | `supers fmt::v11::format_error` | 0: its base `std::runtime_error` is a library symbol (before the fix also `st: incomplete: 1 links to unresolved symbols`) |

Every answer took 0.2 to 0.4 s on fmt and about 0.03 s on the fixture, with no `st serve`. Parameter types match
without their namespace (`string_view` for `fmt::v11::string_view`). A function declared in a header and defined in
`format-inl.h` is one key listed at both places; `find vformat` lists both.

Two things to know from fmt:
- **Library symbols.** Symbols from headers outside the project, such as `std::runtime_error`, are parsed but not
  indexed, so `find`, `callees` and `supers` leave them out without a warning; `refs std::runtime_error` answers (78
  references in fmt). fmt's tests specialize templates in `std`, and in the first measurement `st` therefore counted
  `std` as one of the project's namespaces and its undefined symbols as unresolved: `unresolved_symbols 7283`, and
  answers that reached them ended with `st: incomplete:`. Fixed (review fix round): an undefined C/C++ symbol is never
  unresolved, because Clang resolves every name it indexes and one it cannot is an index error, so such a symbol is
  declared in a non-indexed header. After the fix fmt shows `unresolved_symbols 0` and no `st: incomplete:` line; the
  fixture now specializes `std::hash<shop::Item>`, and `StCxx` checks the same.
- **Template instantiations** are callers of their own: 294 callers of `vformat` are mostly `format<...>` for each
  argument type the tests use, all at the same line.

## Docker and `/dev/shm`

C/C++ indexing passes its results through shared memory. Docker's default `/dev/shm` of 64 MB filled up on fmt: with
multi-process indexing (the default) the indexer processes waited without CPU use for 10 minutes until killed;
with it turned off the indexer aborted (`boost::interprocess::interprocess_exception`). With `--shm-size=4g` fmt indexes
in 10 s. The fixture fits in 64 MB. On a desktop `/dev/shm` is half the memory. Documented under C and C++ projects.

## Package (Ubuntu 22.04)

The release package is now built from `fork_release_cxx`, the preset CI configures, builds, tests and packages.
`fork_release` stays for machines without LLVM 19 and packages without C/C++.

- `createPackages.sh build/fork-release-cxx`: the glibc 2.35 / GLIBCXX 3.4.30 guard passed. The package bundles
  `libclang-cpp.so.19.1` (71 MB) and `libLLVM.so.19.1` (129 MB), and through `libLLVM` `libxml2`, ICU 70 (32 MB),
  `libedit`, `libffi`, `libbsd` and `libmd`, plus Clang's builtin headers in `share/data/cxx` (14 MB). Package size:
  157 MB compressed (80.9 MB in the Python results), 465 MB unpacked (`lib/` 301 MB). About 75 MB of the compressed
  size is the LLVM libraries, their dependencies and the headers.
- `bin/app/data/cxx/include` now holds Clang 19.1.7's resource headers (`/usr/lib/llvm-19/lib/clang/19/include`, 241
  files), replacing the pre-Clang-16 set: 59 added, 21 removed (among them the sanitizer, fuzzer and ORC headers of
  compiler-rt, which LLVM 19 ships in another package), 108 changed. Configuring against another LLVM major version
  now fails with a message saying so.
- `st-smoke.sh` on the packaged `st` passed, with its new C/C++ section (CMake configures a copy of the fixture, then
  `st index --full` `indexed 5 files, 0 errors`, `cxx indexed`, `callers 'shop::Box<T>::get'`, `supers`).
- Then the package alone in a clean `ubuntu:22.04` with no LLVM development packages (Mesa's `libLLVM-15.so`, pulled in
  by `libgl1`, is the only LLVM on the system), plus `g++` and `cmake` for the project and the GUI's system libraries:
  no library missing (`ldd`), `st index --full --project ~/cpp/shop.srctrlprj` printed `indexed 5 files, 0 errors` in
  2.0 s, `sourcetrail index --full` the same, `st status` `cxx indexed`, and `callers 'shop::Box<T>::get'`,
  `callers 'shop::discount(double,double)'`, `supers shop::Book::price` and `impls shop::Item` gave the answers above.

## CI

`.github/workflows/build.yml` adds an "Install LLVM 19" step (the apt.llvm.org key into `/usr/share/keyrings`, the
`llvm-toolchain-jammy-19` source, `clang-19 llvm-19-dev libclang-19-dev libclang-cpp19-dev`), switches configure,
build and test to `fork_release_cxx` (ctest stays serial), packages `build/fork-release-cxx`, checks that the package
holds `libclang-cpp.so.19*` and `libLLVM.so.19*` (the smoke test skips C/C++ for an `st` built without it), and names
C/C++ in the release notes.

Checked: that step, run verbatim in a clean `ubuntu:22.04` (plus `sudo`, `curl`, `gpg`), installs LLVM 19.1.7, and
`find_package(Clang CONFIG)` with the preset's `Clang_DIR` finds it (LLVM 19.1.7, `clang-cpp` dylib); its resource
headers are identical to the ones committed. The build, the tests (`ctest --preset fork_release_cxx`, 1,444 of 1,444
serially), packaging and the smoke test ran in `st-ci-2204-cxx`, which is the CI steps up to that one plus it.
Not checked: a run on GitHub's runner itself (its disk space with LLVM and the larger build, its preinstalled clang
versions next to apt.llvm.org's).
