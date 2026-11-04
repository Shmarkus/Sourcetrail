# Python indexing: results

Measured on Nov 4, 2026, on three projects:
- **The fixture**, `testing/cli/python`: a small shop package (`pyproject.toml`, `shop/` with a re-exporting
  `__init__.py`, `tests/test_services.py`), no environment.
- **`click` 8.1.7** (commit `874ca2bc1c30d93a4ac6e36a15ed685eafe89097`): `src/` layout with `py.typed`, tests and
  examples.
- **`requests` 2.32.3** (commit `0e322af87745eff34caffe4df68456ebc20d9068`): `src/` layout, a package typeshed has
  stubs for.

Both public projects were cloned at their release tags, with the project file at `.sourcetrail/<name>.srctrlprj` and
the group's root `..`, as New Project creates it. "With a venv" means a `.venv` holding the dependencies as PyPI wheels
(click: `pytest==7.4.0`, `typing_extensions`, `importlib_metadata`, `colorama`; requests: its four runtime
dependencies, `PySocks` and the test requirements); the project itself was not installed. "Without" means no `.venv`,
so the indexer used the system `python3` 3.12, which has none of them. Host: the `fork_release` build, Node 24.13,
the machine otherwise busy. Spec: `2026-11-04-python-indexing-design.md`.

## Projects

| | Fixture | click | requests |
|---|---|---|---|
| Python files | 4 (84 lines) | 71 (17,551 lines) | 36 (11,209 lines) |
| `st index --full` | `indexed 1 files, 0 errors` | `indexed 1 files, 7 errors` | `indexed 1 files, 14 errors` |
| Index time (indexer's own / `st index --full` wall) | 0.56 s (tool alone) | 5.2 s / 6.0 s | 5.8 s / 7.1 s |
| Nodes, edges (with a venv) | 52, 108 | 2,323, 10,068 | 1,447, 4,277 |
| Errors (unresolved modules) without a venv | 0 | 9 | 20 |
| Errors with a venv | | 7 | 14 |
| Refresh after a one-line edit | | 6.0 s (`refreshed 1 files in 6.0s`, through a query) | 7.0 s (`st index`) |

"1 files" is the Python group's one source file, its root folder, like TypeScript's `tsconfig.json`; `st status`
shows the real count (`files 36` for requests). Every error is an `unresolved import: <module>` row, one per module and
run. A refresh re-indexes the Python group as a whole, so it costs about a full index of the Python part.

### What the unresolved modules are

**click with a venv (7):** `pallets_sphinx_themes` and `setuptools` (docs and `setup.py`; not installed: correct);
`colorama` (in Windows-only code) and `ctypes` (`from ctypes.wintypes import DWORD`, Windows-only); `importlib`
(`from importlib import metadata`) and `urllib` (`from urllib import parse as urlparse`), both standard-library
packages imported as `from <package> import <submodule>`; `typing_extensions` (`import typing_extensions as te` under
`TYPE_CHECKING`). Without the venv add `pytest` and `importlib_metadata`.

**requests with a venv (14):** 10 are right: modules of Python 2 or optional ones that are not installed
(`BaseHTTPServer`, `SimpleHTTPServer`, `StringIO`, `cStringIO`, `dummy_threading`, `OpenSSL`, `chardet`, `pygments`,
`setuptools`, `simplejson`). 4 are not: `http` (`from http import cookiejar as cookielib`), `json` (`import json`
after `import simplejson as json` in a `try`), `urllib` (`from urllib.request import (... proxy_bypass ...)`) and
`urllib3` (`from urllib3.exceptions import SNIMissingWarning`, a name urllib3 2 no longer has, reported under its
module). Without the venv add `certifi`, `charset_normalizer`, `cryptography`, `idna`, `pytest` and `trustme`.

## `st` answers

| Project | Command | Result | Output |
|---|---|---|---|
| Fixture | `callers shop.models.User.greet` | 1: `shop.services.OrderService.place(str, str, str)`, `shop/services.py:22` | 1 line |
| click | `callers click.utils.echo` | 214 callers: 160 in `tests/`, 37 in `examples/`, 17 in `src/` | 51 lines, 6.6 KB (50 and `+164 more`), 0.05 s |
| click | `impact click.core.Option.__init__ --group file` | 133 symbols in 20 files | 20 lines, 2.2 KB, 0.04 s |
| click | `outline click.core.Context` | 52 members with signatures | 51 lines, 5.8 KB, 0.04 s |
| click | `callers click.core.Context.invoke --no-source` | 7, among them `tests.test_commands...OptParseCommand.invoke` | 7 lines, 0.6 KB |
| requests | `callers requests.api.get` | 0 (exit 1) | |
| requests | `impact requests.sessions.Session.send --group file` | 16 symbols in 2 files (`api.py`, `sessions.py`) | 2 lines, 0.2 KB, 0.01 s |
| requests | `outline requests.models.Response` | 37 members | 37 lines, 3.6 KB, 0.02 s |

In click, the tests link to `src/click` (the indexer puts `src/` on the interpreter's path), and calls through
`click.echo` (re-exported by `click/__init__.py`) land on `click.utils.echo`.

In requests they do not. Pyright resolves `import requests` in `tests/` to the `requests` stubs it bundles from
typeshed, not to `src/requests`: requests 2.32 has no `py.typed`, and for such a package Pyright prefers the stubs.
Class names still match (`requests.Request(...)` is `requests.models.Request.__init__`), but `requests.get(...)`
and methods called on objects (`s.send(...)`) come back from scip-python as local symbols and get no link. So
`callers requests.api.get` finds none of the tests' calls, and `impact` of `Session.send` stays inside `src/`. Inside
`src/requests` itself, calls are linked (relative imports included). The same mechanism makes a dependency that
typeshed has stubs for (`six`, `chardet`, older `requests`) an unresolved import in a project that uses
it, even when it is installed. Checked in a clean Ubuntu 22.04 container: `six.py` placed in a venv's
`site-packages` is reported unresolved, a module of any other name in the same place resolves, and the Debian
`requests` 2.25 copied into the venv resolves only once a `py.typed` is added to it.

A second limit found there: a library outside a `site-packages` folder does not resolve either. Debian's
`python3-requests` (in `/usr/lib/python3/dist-packages`) and a module in a `PYTHONPATH` folder are both reported as
unresolved, while a pip-installed `requests` 2.34 in a venv resolves (0 errors). So the system Python of a
distribution gives the project's own code and the standard library, and libraries need a virtualenv.

## Package (Ubuntu 22.04)

The CI steps of `.github/workflows/build.yml`, reproduced in the `st-ci-2204` image (Ubuntu 22.04, gcc 13, Node
22.23.4, JDK 21, a fresh Conan cache; Qt from the host's aqt download), on a clean clone of the branch: configure and
build, `ctest` 1,090 of 1,090 passed, `node --test tools/ts-indexer` 76 of 76 (none skipped: the image has `python3`
with `venv`), `createPackages.sh` (the glibc 2.35 guard passed), and `st-smoke.sh` on the packaged `st`, including its
new Python section, passed. The container needs `--init`: without an init process the smoke test's orphaned indexer
stays a zombie and the check "the left refresh did not end within 60 s" fails.

Package size: 80.9 MB compressed (71 MB before Python), 224 MB unpacked, of which `share/data/ts-indexer` is 77 MB
and scip-python 34 MB. scip-python's `node_modules` holds no native code (no `.node` or ELF files), so the glibc guard
has nothing more to check.

Then the package alone, in a clean `ubuntu:22.04` with only the GUI's system libraries, on copies of the fixture and of
a one-file project (`import requests` and a call to `requests.get`, with a `requirements.txt`):

| | No Node | Node 22.23.4, no `python3` | Node and `python3` 3.10 with `python3-requests` |
|---|---|---|---|
| Fixture `st index --full` | `indexed 1 files, 0 errors`, `Python skipped: cannot run "node": execve failed: No such file or directory` | `indexed 1 files, 0 errors` (0.59 s) | `indexed 1 files, 0 errors` |
| Fixture `st status` | `python not indexed; st index shows why` | `python indexed`, 4 files | `python indexed`, 4 files |
| Fixture `callers shop.models.User.greet` | not found (exit 3) | `shop.services.OrderService.place(str, str, str)` | the same |
| One-file project | skipped likewise | 1 error (`unresolved import: requests`), `callees fetch.fetch` 0 | the same: the system `dist-packages` copy is not used |

The indexer's `scip-indexer: python3 not found; library symbols unresolved` line is not shown by `st index`; without
`python3` the index is the same as with one that lacks the libraries.

## CI

`.github/workflows/build.yml` needed no new step: `npm ci` in the existing TypeScript step installs scip-python, the
runner's `python3` runs the venv tests, and `st-smoke.sh` now indexes the Python fixture too (index, `st status` row,
`callers shop.models.User.greet`; skipped with a note when `node` is missing). The step name and comments say Python,
and the `latest` prerelease notes no longer say Python returns later. The YAML parses; the steps were reproduced in the
container above; a run on GitHub is still to come with the first push of the branch.
