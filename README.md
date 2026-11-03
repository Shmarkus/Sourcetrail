# Sourcetrail

Sourcetrail is a free and open-source cross-platform source explorer that helps you get productive on unfamiliar source code.

__Links__
* [Download](https://github.com/OpenSourceSourceTrail/Sourcetrail/releases)
* [Quick Start Guide](DOCUMENTATION.md#getting-started)
* [Documentation](DOCUMENTATION.md)
* [Changelog](CHANGELOG.md)

!["Sourcetrail User Interface"](docs/readme/user_interface.png "Sourcetrail User Interface")

Sourcetrail is:
* free
* working offline
* operating on Windows, macOS and Linux
* supporting C, C++, Java and Python
* offering an SDK ([SourcetrailDB](https://github.com/CoatiSoftware/SourcetrailDB)) to write custom language extensions

> This fork builds for Linux, with Java support and `st`, a command-line tool for code navigation by agents. C/C++ indexing
> is off, and Python support returns in a later release. Its builds are on the
> [fork's releases page](https://github.com/Shmarkus/Sourcetrail/releases).

## Using Sourcetrail

To set up Sourcetrail on your machine, you can either download the respective build for your operating system from our list of [Releases](https://github.com/OpenSourceSourceTrail/Sourcetrail/releases) and install it on your machine, or use one of the following package managers:
 > The following command will install the original Sourcetrail that is currently archived!

* Use the [Chocolatey package](https://chocolatey.org/packages/sourcetrail) with `choco install sourcetrail`

After your installation is complete, follow our [Quick Start Guide](DOCUMENTATION.md#getting-started) to get to know Sourcetrail.

## How to Contribute

* Please read and follow the steps in [CONTRIBUTING.md](CONTRIBUTING.md) file.
* If you are looking for more information about Sourcetrail software development, please refer to our [wiki](https://github.com/CoatiSoftware/Sourcetrail/wiki).

# How to Build

This fork builds on Linux (x86-64) only, with Java support and the `st` command-line tool. The C/C++ indexer is off,
and Python support returns in a later release. Windows and macOS builds are not maintained here.

## Required tools

* __gcc 13__, __lld__, __Ninja__, __CMake 3.23 or newer__, __Git__ and __Python 3__ with `venv`
    * On Ubuntu 22.04: `sudo apt install gcc-13 g++-13 lld ninja-build cmake git python3-venv` (gcc 13 comes from
      `ppa:ubuntu-toolchain-r/test`). The presets and the Conan profile call plain `gcc`/`g++`, so make gcc 13 the default
      (for example with `update-alternatives`).
    * Git also provides the version number: the newest `v*` tag (`v2.0.0-fork` gives version `2.0.0-fork`).
* __JDK 21__ and __Maven__, to build the Java indexer (`java_indexer/`) and to run the Java tests.

Everything else (Qt 6.8.2, and Boost, fmt, spdlog, SQLite and the other libraries pinned in `conan.lock`) is installed by
`scripts/fork/bootstrap.sh`.

## Building

```bash
git clone https://github.com/Shmarkus/Sourcetrail.git
cd Sourcetrail
scripts/fork/bootstrap.sh                      # once; idempotent
eval "$(scripts/fork/bootstrap.sh --env)"      # in every new shell
cmake --preset fork_release
cmake --build --preset fork_release -j16
```

`bootstrap.sh` puts a Python venv with Conan and aqtinstall, Qt from aqt and its own `CONAN_HOME` into
`$ST_TOOLCHAIN` (default `~/.cache/sourcetrail-toolchain`; `~/.conan2` is not touched), then runs `conan install` for
`.conan/gcc/` with the lock file. `--env` only prints the `export` lines for `CONAN_HOME`, `CMAKE_PREFIX_PATH` and
`PATH`.

The `fork_release` preset builds into `build/fork-release` with Java support and lld. The binaries are
`build/fork-release/app/Sourcetrail` (the GUI, also `Sourcetrail index` headless), `sourcetrail_indexer` and `st`. They
find their resources through the `data` and `user` symlinks CMake creates next to them.

> Gradle imports use the project's Gradle wrapper version by default. Set `GRADLE_VERSION` before launching Sourcetrail
> only if you need to override that version.

## Creating the Linux package

After building with the `fork_release` preset, run `./setup/Linux/createPackages.sh` from the main directory. It creates
`Sourcetrail_Linux_64bit.tar.gz` there, with the Qt libraries and plugins of the Qt the build linked against bundled.
Packaging needs `patchelf`, `file`, `objdump`, ImageMagick's `convert`, and `libxcb-cursor0` installed so that it can be
bundled. Build and package on an Ubuntu 22.04 host (CI does the same): the script rejects binaries that need a glibc
newer than 2.35.

# Java projects and `st`

## New Java projects

**New Project** (start screen, Project menu or Ctrl+N) asks for a folder. If that folder holds a project
already (`.sourcetrail/*.srctrlprj`), Sourcetrail opens it. Otherwise it looks for a build at the top level of the
folder: `build.gradle`, `build.gradle.kts` or `settings.gradle` means Gradle, which wins when there is also a
`pom.xml`; only a `pom.xml` means Maven. Build files in sub-modules alone do not count. It then creates a project with
tests included and opens it with the usual "Start Indexing" dialog. Without a build file it offers a plain Java project
and opens the project settings so that you can fill in the class path. The JDK is the one in the preferences.

The project lives in `<folder>/.sourcetrail/<folder name>.srctrlprj`. The index database, bookmarks and Sourcetrail's
dependency and delombok copies sit next to it. `.sourcetrail/` contains its own `.gitignore` (`*`), so none of it shows up
in `git status`. `st` finds the project from anywhere in the folder.

**Project > New Custom Project...** opens the full project wizard (Custom Command source groups, manual Java source
groups, or several groups in one project).

An index written by an older version is recognized as outdated and indexed again in full, from the GUI or from `st`.

## `st`: code navigation for agents

`st` answers "who calls this", "what implements that", "how does A reach B" from an
existing Sourcetrail index, as plain text lines that pipe into each other.

### For agents

| Question | Command |
|---|---|
| Who calls X? | `st callers OrderService.place --depth 2` |
| What does X call? | `st callees OrderService.place` |
| Implementations or overrides (and the reverse) | `st impls PaymentGateway`, `st impls PaymentGateway.charge`; `st supers CardGateway` |
| How does A reach B? | `st path OrderController.create OrderRepository.save` |
| What breaks if X changes? | `st impact OrderService.place --group file` |
| A stack-trace line: its method and callers | `st at OrderService.java:42 \| st callers --depth 3` |
| Members of a class or file; package or class dependencies | `st outline OrderService`, `st outline src/main/java/com/acme/shop/OrderService.java`; `st deps com.acme.shop.order` (`--in` for incoming) |
| Files in a git diff to symbols | `git diff --name-only \| st symbols-in` |
| What uses a library annotation or type (`@Scheduled`, `TaskScheduler`) | `st refs Scheduled`; a whole library package: `st impact org.springframework.scheduling --depth 1` |
| Narrow a result by annotation, kind or file | `... \| st filter --annotated Test`, `--kind method`, `--not --file '*Test*'` |
| Text, config, strings, log messages | grep, not `st`; quote globs for zsh: `grep -rn --include='*.java' 'text' .` |

Names are a short name (`OrderService.place`) or a key; parameters take simple type names: `'OrderService.place(Order, int)'`.
- **Trust the answer.** No `st: incomplete:` line on stderr means the answer is complete as far as the index goes: don't
  re-read source to confirm it. Only the [blind spots](#st-known-limits) need grep or reading: text, config, strings, Java records,
  Lombok `@Builder` setters of a field, JPA metamodel uses (`Order_.status`), calls through a field of an unresolved type.
- **Exit 1** means none. **Exit 2** (ambiguous): pick a key from the candidates on stderr and rerun.
  **Exit 3** (not found): check the name, the parameters (count and simple type names; stderr lists the overloads), and the
  project: `st` picks it from the current directory, `st find <name>` names it (`find: 0 (project shop)`), `--project` overrides.

Faster: `st serve --detach` once per session keeps the index loaded (opt-in; `st status` shows `server running`).
```bash
st at OrderService.java:42 | st callers --depth 4 | st filter --annotated Test            # stack trace -> callers -> tests
git diff --name-only | st symbols-in | st impact | st filter --not --file '*Test*'          # diff -> what it can break, tests left out
st impls PaymentGateway.charge | st callers | st filter --annotated RestController           # interface -> impls -> annotated callers
```

### Build and reference

The `fork_release` build (see [How to Build](#how-to-build)) puts `st` next to the other binaries. Put it on your `PATH`:

```bash
ln -s "$PWD/build/fork-release/app/st" ~/.local/bin/st
```

It needs the adjacent `Sourcetrail`, `sourcetrail_indexer` and data payload, so link it rather than copying it alone.

**Indexing.** `st index [--full] [--verbose]` indexes the project, incrementally unless `--full`, and prints
`indexed N files, M errors` on stdout (exit 0 even when the index holds parse errors, 1 when indexing fails). It runs
the same headless indexing as the GUI (`Sourcetrail index`), including Gradle dependency resolution and delombok, with
the Java settings from the GUI's preferences; the configured Java path is the JVM library (`libjvm.so`), not
`bin/java`. Status lines go to stderr; `--verbose` adds the indexer's log lines, such as each file it indexes. When
indexing fails, `st` prints the last 50 lines of the indexer's output. With `st serve` running, `st index` goes through
the server. Create the project first (see [New Java projects](#new-java-projects)), then run `st index --full` once from
anywhere in its folder (or with `--project /abs/project.srctrlprj`). After an upgrade, an index built by an older version
needs one `--full` reindex; until then `st` exits 4 with `index is empty or incompatible; reindex with --full`.

**Project discovery.** `st` looks for a `.srctrlprj` in the current directory and upward, at each level first in the
directory itself, then in its `.sourcetrail/` folder (a root-level project file wins). Failing
that it consults the registry `~/.config/sourcetrail/st-projects.tsv` (source tree to project file).
Any run with `--project /abs/project.srctrlprj` registers that project for its source tree, so
afterwards `st` works from anywhere in the tree without the flag.

**Commands:** `find`, `status`, `refs`, `callers`, `callees`, `impls`, `supers`, `path`, `impact`, `deps`, `filter`,
`symbols-in`, `at`, `outline` (see `st --help`).
The symbol is a name or a full key (`pkg.Class.method(pkg.Arg)`); without one, `st` reads
keys from stdin, one per line or the first tab-separated column, so commands pipe.
Ambiguous names exit 2 and list the candidates; a bare class name that matches only the class and its own constructors resolves to the class. A parameter list matching no overload exits 3 and lists the overloads.

**Output.** One tab-separated line per result: `key`, `kind`, `path:line`, source line
(`--no-source` drops it). Paths are relative to the current directory when below it. The count (`callers: 9 (project shop)`, the project file's base name)
goes to stderr; a `+N more (--limit)` overflow line ends stdout. Calls through an
interface show as `method via pkg.Iface.method()`. With `--depth` above 1 (or 0) `callers` and `callees` append
`, depth <n>` to the kind (`method, depth 2`; `depth 0` is reached through override links alone, such as the implementation
`callees` reaches through an interface method; `--limit` keeps the lowest depths). `--limit` (default 50), `--depth`, `--kind` shape results.
`find` ranks exact, then suffix, then substring matches. External (library) symbols are hidden.

```bash
st find --kind class OrderService
st callers 'com.acme.OrderService.place(com.acme.Order)' --depth 3
st find --kind interface Repository | st impls | st callers --depth 3
st path com.acme.Controller.create com.acme.Repo.save
```

**More commands.**
- `at <file>:<line>`: the symbol defined on that line (fields, declarations), else the innermost enclosing scope. A bare file
  name (`OrderService.java:42`, as in a stack trace) is matched as a path suffix; two matches exit 2 and list both on stderr.
  An absolute path that does not exist here (a CI or container stack trace) matches the indexed files sharing the longest
  path suffix with it.
  Text after a second colon is ignored, so `grep -Hn` output pipes in (plain `grep -n` on a single file prints no path and
  exits 64). `symbols-in` takes paths, so pipe `grep -l` into it.
- `symbols-in <file>...`: symbols defined in the files, in line order (no packages, files or type parameters).
- `outline <class|file>...`: a class's direct members, or a file's symbols, with `visibility kind` (`public method`) and a
  `path:line-endLine` range. An argument naming an existing file is a file, anything else a symbol.
  Package-private members print `package method`; interface and annotation members print no access word.
  `outline <class>` lists nested types but not their members.
- `impact <symbol>`: what a change can break: users, subtypes and overrides, transitively. `--depth` (default 0, unlimited)
  counts every hop except override hops; a class stands for all its members, a package for every type and member in it and
  its sub-packages (by name prefix, as in `deps`); the input's own symbols are not listed. `--group file|package` prints one
  line per file or package. Overrides are followed up and down, but not down again after going up. Rows read
  `<kind>, depth <n>`: `depth 1` is a direct user, `depth 0` is reached through override links alone (such as an
  implementation of an input interface method). `--limit` keeps the lowest depths. stderr gets
  `impact: N symbols in F files, P packages (project <name>)`.
  `| st filter --not --file '*Test*'` leaves out tests (symbol rows only: `filter` exits 3 on `--group` rows).
- `deps <symbol>`: outgoing dependencies, one row per top-level type (`class, N links`), or per package for a package input
  (`package, N links`; sub-packages are included). `--in` gives incoming. Links inside the input, external targets and builtin types
  are not counted; for a member input, its whole top-level class counts as inside, so links to sibling members don't count.
- `filter`: keeps the stdin lines that pass, unchanged and in order: `--kind K`, `--package P` (keys under `P.`),
  `--annotated A` (the symbol or its enclosing type carries `@A`), `--file G` (wildcard; with a `/` it matches the path, else
  the file name), `--not` negates the combination. Passing lines print unchanged, identical lines once. A key that is unknown or
  ambiguous, or a row that is not a symbol (an `impact --group` line), is named once on stderr (`st: symbol not found: <key>`), its
  lines are dropped, and the exit is 3 even when other lines pass; otherwise 0 with output, 1 without.

`--edges call,usage,type-usage,override,inheritance,import,annotation` picks the links `path` and `impact` follow (an unknown
name exits 64). Defaults: `path` `call,usage,override`; `impact` `call,usage,type-usage,override,inheritance,annotation`.
`--package`, `--annotated`, `--file`, `--not`, `--in`, `--group` and `--edges` on another command exit 64, as does `--kind`
on `outline`, `symbols-in` and `at` (pipe into `st filter --kind` instead).

**Names.** Type parameters may be left out: `Result.failed` finds `Result<T>.failed<T>(...)`. `find` hides type-parameter
nodes unless `--kind` asks for them.

**Input rules.** `filter`, `impact` and `deps` take symbols as arguments or keys on stdin; `symbols-in` takes paths; `at`
takes `file:line`. With Lombok, give the original path and line: the file maps to its delombok copy and the line maps back
through the generated code; every line of a statement delombok joined onto one line maps to it. A line with no counterpart
gets the symbol of the nearest mapped lines above and below when both give the same one; otherwise st reports it on stderr
(`st: <file>:<line> has no counterpart in the indexed (delombok) file`) and skips it rather than guessing, like an input that is
not indexed; `at` exits 3 only when every input was skipped. If a piped command was cut off by `--limit`, stderr says `input truncated upstream`.

```bash
grep -Hn 'placeOrder' src/main/java/com/acme/*.java | st at | st impact --group package
st find --kind class Controller | st filter --annotated RestController | st deps
```

<a id="st-known-limits"></a>

**Known limits.** Java records are not indexed as types by the bundled Java indexer: record types, constructors and
components are missing from every answer, and calls made inside a record body are attributed to no symbol. Classes an
annotation processor generates during the build (JPA metamodel `Order_`, MapStruct `...MapperImpl`) are not indexed either.
Field reads and writes are not distinguished (both are `usage`).
A class two modules both declare (same package and name, e.g. a test helper) is one symbol listed at both files, with
one access level per member in `outline` (one copy's private field can show as public); a third
module resolves it to the first copy on the class path, so its calls to members only the other copy has are missing
(`st` warns, see below; module-aware resolution is a tracked follow-up).

**Incomplete answers.** When an answer of `callers`, `callees`, `refs`, `impls`, `supers`, `path`, `impact` or `deps` touches such
a gap, stderr ends with one line, `st: incomplete: N links to unresolved symbols, M index errors (see st status)` (a zero part is
left out); stdout and the exit code stay the same. No `st: incomplete:` line means the answer is complete as far as the index goes; there is no need to re-read the source to confirm it, except for what the index never holds and so cannot warn about: plain text, config and string uses (use grep), switch arms and other code inside Java records, the Lombok `@Builder` setter of a field, and calls through a field whose type is unresolved (see Known limits and the next paragraph). N counts the links the command followed from the symbols it walked whose
other end the index could not resolve: the indexer's `unsolved-symbol` placeholder, or a symbol without a definition whose
package is one the project defines types in, or declares below one (library symbols and implicit members such as default
constructors don't count). M counts the index errors that belong to a walked symbol or to the symbol enclosing a result row: an
error belongs to the innermost named method or type around it (anonymous and local classes are skipped), at class level to a
field declared on its line, and outside every scope (an import) to the file's top-level types. `st status` adds the index-wide
totals, `files_with_errors` and `unresolved_symbols`.
A further part, `K symbols declared in several modules` (listed first; `st status` has no total for it, so the line drops
the hint when it is the only part), counts the inputs and the symbols the walk went on from, or types enclosing one, that
more than one file declares (a class two modules both have): their callers may be bound to the other copy or missing. A
result row in such a class does not count. The check assumes Java: in C and C++ a declaration in a header and a
definition in a source file count as several files, so `st` on C/C++ indexes is untested and may warn wrongly.
The warning can stay silent where a gap is real: the Java indexer reports an unresolved type once, where it is declared (a
field of a generated type), so calls through that field vanish without an error in the calling method; and `refs` of an
entity field misses its JPA metamodel uses (`Order_.status`) without a warning. `refs` and `impact` of a field of a Lombok
`@Builder` class do not reach the builder setter of the same name (the builder has its own field), so builder calls that set
it are missing without a warning; query `<Type>.<Type>Builder.<field>` as well.

`symbol not found` on a Java record or one of its members means records aren't indexed.

**Exit codes.** 0 results, 1 empty, 2 ambiguous, 3 symbol not found, 4 no index,
5 index stale (with `--no-refresh`, or when a refresh failed) or project locked past `--wait`, 64 usage error, 70 internal error.

**Lombok.** Sourcetrail indexes delombok copies, so `st` maps those locations back to the original
file and line, mapping only lines whose text matches the original unambiguously. Unchanged copies map line for line; a
location in a statement delombok joined onto one line maps to the original line holding its column. Generated members and other lines without an
original line print the original path without a line, `src/.../Order.java (generated)`; if there is no original file, the
row keeps the delombok `path:line` and the kind column gets `(generated)`. `--no-generated` (every command except `path`) drops those rows.

**Auto-refresh.** Before each query `st` compares source (and Gradle build file) times with the last index
and incrementally re-indexes if they changed, printing `refreshed N files in X.Ys` on stderr. The
`<project>.srctrlst` marker beside the project file records the last refresh so edits made during one are not lost.
`--no-refresh` skips this and exits 5 when stale; `--wait <ms>` bounds the wait for the project lock (default 30000).
File times are compared below the second; only a file saved within about 20 ms before a refresh started is refreshed once
more.

**`st serve`.** Opt-in resident server for one project: it keeps the index loaded and answers `st` calls over a Unix socket
(in `$XDG_RUNTIME_DIR/st/`, else `/tmp/st-<uid>/`; a directory not private to you is refused), so a query costs
about 100-150 ms instead of 0.4-0.6 s on a large project.
- `st serve` runs in the foreground; `st serve --detach` starts it in the background (output in `<project dir>/.st-serve.log`);
  `st serve --stop` stops it. `st status` ends with `server<TAB>running` or `server<TAB>off`.
- While it runs it watches the sources and re-indexes in the background after edits (logs: `<project dir>/.st-refresh.log` and `.st-refresh.log.verbose`), so
  the next query does not wait for the indexer. `--no-watch` turns this off. The logs sit next to the project file; for a
  project in `.sourcetrail/` its `.gitignore` covers them, otherwise add them to `.gitignore`.
- It is never started automatically, and there is one server per project. A client that finds no usable server answers itself.
- `--no-server` (or `ST_NO_SERVER=1`) answers in the calling process even when a server runs.
- Answers are identical with or without the server, and freshness is still checked per query. The server uses its own
  environment (`HOME`, `JAVA_HOME`, ...); the client's is not forwarded.
- stderr notes: `st: server <is from another build | speaks another protocol version | serves another project>; answering without it`,
  `st: server did not answer; answering without it`, and `st: waited Xs for the background refresh` (a query that arrived
  while a refresh was running). Measurements: `docs/superpowers/specs/2026-11-02-st-serve-results.md`.

**Measured** (Nov 3, 2026, on a private 2k-file Spring/Gradle service with Lombok; `testing/benchmark`, `bench.py speed`).
The machine was under load (IDE, browsers, full swap), so this build was measured in the same session as the previous
master build, and both are slower than on a quiet machine: the day before, master's own `st serve` numbers were 83 ms,
0.14 s, 0.60 s and 8.4 s.

| | Without a server | With `st serve` | master with `st serve`, same session | Target |
|---|---|---|---|---|
| Startup, `st status` | 451 ms | 89 ms | 114 ms | under 200 ms |
| Query, median over the questions | 0.59 s | 0.16 s | 0.20 s | under 1 s |
| Query, slowest (`st at … \| st callers --depth 3`) | 1.38 s | 0.78 s | 0.89 s | under 1 s |
| First query right after a one-line edit (it refreshes) | not measured | 10.5 s | 11.5 s | under 5 s |

About 4.6 s of a refresh is Gradle, delombok and class path work, the same on both builds. With the watcher running, a
query made some seconds after an edit finds the index already refreshed (about 120 ms).
In the agent benchmark (`bench.py run`: 9 questions, 3 runs each, fresh headless agents, the same prompt with grep only
or with `st` and the CLAUDE.md snippet), both arms answered 27 of 27 correctly. With `st`, agents used 18% fewer tokens
and 25% fewer tool calls per question (medians: 53,942 against 65,440 tokens, 3 against 4 calls), and picked `st`
unprompted on 21 of 21 structural questions. The `st` arm was run with this build on Nov 3, the grep arm on Nov 2 (it
does not depend on the build); against master's `st` arm of Nov 2 the medians were +7% tokens and the same number of
calls. Details: `docs/superpowers/specs/2026-11-02-upgrade-port-results.md`.

## Tests

```bash
eval "$(scripts/fork/bootstrap.sh --env)"
HOME=/tmp/st-test-home QT_QPA_PLATFORM=offscreen ctest --preset fork_release
JAVA_HOME=/path/to/jdk-21 bash testing/cli/st-smoke.sh build/fork-release/app/st
```

The tests are GoogleTest suites run by `ctest`. The preset runs them one at a time, because the integration suites share
temporary databases. Point `HOME` at a scratch directory so your own `~/.config/sourcetrail` is not written. The Java
tests need a JVM: `$HOME/.config/sourcetrail/ApplicationSettings.xml` naming a `libjvm.so`, or `JAVA_HOME` pointing at a
JDK 21. `st-smoke.sh` indexes a copied Lombok fixture with an isolated home and checks `st` end to end, including
`st serve`.

# License

Sourcetrail is licensed under the [GNU General Public License Version 3](LICENSE.txt).

# Trademark

The "Sourcetrail" name is a trademark owned by Coati Software and is not included within the assets licensed under the GNU GPLv3.
