# Upgrade port: storage, refresh and source group test cases

Master's `src/test/StorageTestSuite.cpp`, `RefreshInfoGeneratorTestSuite.cpp` and `SourceGroupTestSuite.cpp`,
case by case: where each one lives on `upgrade/modern-base`, or which fork commit it belongs to.

## Base behaviour (added before 6b792d5a4)

Upstream already carries gtest versions of these suites in `tests/integration/lib/` (ctest prefix
`integration.lib.`). Base cases they hold are not ported a second time.

| Master suite | Cases | Where on the branch |
|---|---|---|
| StorageTestSuite | storage saves file, saves node, saves field as member | `tests/integration/lib/StorageTestSuite.cpp` (`Storage.savesFile`, `savesNode`, `savesFieldAsMember`) |
| StorageTestSuite | storage saves method static; clears single file data of single file storage; clears unreferenced / referenced single file data of multi file storage; clears multi file data of multi file storage; finds and removes depending file nodes | not ported: their bodies are commented out on master and assert nothing; upstream dropped them too |
| RefreshInfoGeneratorTestSuite | all 38 base cases (6 "refresh info for all files / updated files", 24 "unknown / nonindexed / indexed ...", 8 "refresh info for updated files clears ..." and "clears unchanged files ...") | `tests/integration/lib/RefreshInfoGeneratorTestSuite.cpp`, same 38 cases and 179 assertions |
| SourceGroupTestSuite | source group custom command generates expected output; can destroy application instance | `tests/integration/lib/SourceGroupTestSuite.cpp` |
| SourceGroupTestSuite | can create application instance | `tests/integration/lib/SourceGroupTestSuite.cpp` (disabled upstream); also done by `SourceGroupJavaFix` |
| SourceGroupTestSuite | source group cxx c empty / cxx cpp empty / cxx cdb / cxx codeblocks | not run: C++ indexer is off (`BUILD_CXX_LANGUAGE_PACKAGE=OFF`); the first three are in upstream's suite behind that flag, codeblocks is gone upstream |
| SourceGroupTestSuite | finds all jar dependencies; can setup environment factory; sourcegroup java empty / gradle / maven generates expected output | ported in Task 3: `src/lib_java/tests/SourceGroupJavaTestSuite.cpp` |

`sourcegroup java gradle generates expected output` has an empty body on Linux on master (`#ifndef __linux__`); the
port skips it on Linux with `GTEST_SKIP`.

## Fork behaviour

| Master suite | Case | Commit | Task |
|---|---|---|---|
| StorageTestSuite | storage trail with unlimited depth terminates when it reaches a package | 044525e80 | 4/5 |
| StorageTestSuite | storage finds files referencing symbols declared in changed files without imports | 044525e80 | 4/5 |
| StorageTestSuite | storage finds only direct importers of changed files | fc435efbf | 4/5 |
| StorageTestSuite | storage finds importers by the file declaring the imported symbol | 4fc7cb353 | 4/5 |
| StorageTestSuite | storage declarations ignore an edit inside a method body | 14d8f4ea9 | 4/5 |
| StorageTestSuite | storage declarations change with a method signature | 14d8f4ea9 | 4/5 |
| StorageTestSuite | storage declarations change when a method is removed | 14d8f4ea9 | 4/5 |
| StorageTestSuite | dependent files are indexed only when declarations changed | 14d8f4ea9 | 4/5 |
| StorageTestSuite | storage declarations change with a field type | 2d7c5439c | 4/5 |
| StorageTestSuite | storage declarations change with a base type | 2d7c5439c | 4/5 |
| StorageTestSuite | storage declarations change with an added overload | 2d7c5439c | 4/5 |
| StorageTestSuite | storage keeps an undeclared symbol a cleared file referenced if it has members | 2d7c5439c | 4/5 |
| StorageTestSuite | storage keeps the edges of a symbol a cleared file only qualifies | 4cfe85ddf | 4/5 |
| StorageTestSuite | storage counts errors injected after clearing files with errors | 4cfe85ddf | 4/5 |
| StorageTestSuite | storage removes a symbol once the files declaring and referencing it are cleared one after the other | 4cfe85ddf | 4/5 |
| StorageTestSuite | storage keeps the members a symbol declared in several files has outside a cleared file | 4cfe85ddf | 4/5 |
| RefreshInfoGeneratorTestSuite | refresh info for updated files defers files referencing the declarations of an edited file | 14d8f4ea9 | 4/5 |
| RefreshInfoGeneratorTestSuite | refresh info for updated files reindexes files referencing a removed file at once | 14d8f4ea9 | 4/5 |
| SourceGroupTestSuite | java class path puts the source file's own root first | 71fa87abf | ported in Task 3 (see below) |

The helpers these cases use come with the same commits: `addJavaSymbol` and friends at the top of master's
StorageTestSuite, `addTokenLocation` / `addSymbolReference` in master's RefreshInfoGeneratorTestSuite.

The natural home for them is upstream's `tests/integration/lib/StorageTestSuite.cpp` and
`RefreshInfoGeneratorTestSuite.cpp`, which already have the fixtures (`addVeryOldFileToStorage` and the others).
A second file in `src/lib/tests/` with the same name would clash with those CMake targets.

### 71fa87abf (not on the Task 4/5 list)

`utility::withOwnRootFirst` and its use in `SourceGroupJava::getIndexerCommands` came onto the branch with the Java
package (1c0f588fb), so the case passes and is ported in Task 3. The same commit makes every Java indexer command list
the source file's own root first, which master's `java_empty` and `java_maven` expected outputs never caught up with
(master's Java source group cases are not registered with ctest and fail when run by hand). The branch's
`output_unix.txt` for both projects has the file's own root moved to the front of each class path; nothing else
differs from master's.
