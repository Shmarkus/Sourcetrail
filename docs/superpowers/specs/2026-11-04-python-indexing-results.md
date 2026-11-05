# Python indexing: results

Measured on Nov 4, 2026, on three projects (re-measured after the final fix wave):
- **The fixture**, `testing/cli/python`: a small shop package (`pyproject.toml`, `shop/` with a re-exporting
  `__init__.py`, `tests/test_services.py`), no environment.
- **`click` 8.1.7** (commit `874ca2bc1c30d93a4ac6e36a15ed685eafe89097`): `src/` layout with `py.typed`, tests and
  examples.
- **`requests` 2.32.3** (commit `0e322af87745eff34caffe4df68456ebc20d9068`): `src/` layout, a package typeshed has
  stubs for.

Both public projects were cloned at their release tags, with the project file at `.sourcetrail/<name>.srctrlprj` and
the group's root `..`, as New Project creates it. "With a venv" means a virtualenv outside the project, named in the
group's interpreter setting, holding the dependencies as PyPI wheels (click: `pytest==7.4.0`, `typing_extensions`,
`importlib_metadata`, `colorama`; requests: its four runtime dependencies, `PySocks` and the test requirements); the
project itself was not installed. "Without" means no venv, so the indexer used the system `python3` 3.12 (which happens
to have `colorama` in Debian's `dist-packages` and `typing_extensions` in the user site). Host: the `fork_release`
build, Node 24.13, the machine otherwise busy. Spec: `2026-11-04-python-indexing-design.md`.

The numbers are from the final fix wave: the indexer now lists the interpreter's distributions for scip-python (it
names a library module after the distribution that holds it, and leaves any other module local), and a `src/` layout's
own modules come first in that list. An earlier run with an empty list had more unresolved modules, no library links
from a venv outside the project, and no links from requests' tests into `src/`; that was the cause of the
"`dist-packages` and typeshed stubs do not resolve" limits reported before, not the folder or the stubs themselves.

## Projects

| | Fixture | click | requests |
|---|---|---|---|
| Python files | 4 (84 lines) | 71 (17,551 lines) | 36 (11,209 lines) |
| `st index --full` with a venv | `indexed 1 files, 0 errors` | `indexed 1 files, 5 errors` | `indexed 1 files, 13 errors` |
| Index time (indexer's own / `st index --full` wall) | 0.59 s (tool alone) | 5.8 s / 7.0 s | 5.9 s / 7.0 s |
| Nodes, edges with a venv | 52, 108 | 2,326, 10,082 | 1,455, 5,395 |
| Errors (unresolved modules) without a venv | 0 | 6 | 19 |
| Errors with a venv | | 5 | 13 |
| Refresh after a one-line edit, through a query | | 7.1 s (`refreshed 1 files in 7.0s`) | 7.1 s (`refreshed 1 files in 7.0s`) |

"1 files" is the Python group's one source file, its root folder, like TypeScript's `tsconfig.json`; `st status`
shows the real count (`files 36` for requests). Every error is an `unresolved import: <module>` row, one per module and
run. A refresh re-indexes the Python group as a whole, so it costs about a full index of the Python part. The wall
times step in whole seconds (the headless indexer polls).

### What the unresolved modules are

**click with a venv (5):** `pallets_sphinx_themes` and `setuptools` (docs and `setup.py`; not installed: correct);
`ctypes` (`from ctypes.wintypes import DWORD`, Windows-only code); `importlib` (`from importlib import metadata`) and
`urllib` (`from urllib import parse as urlparse`), both standard-library packages imported as
`from <package> import <submodule>`. Without the venv add `pytest`.

**requests with a venv (13):** 9 are right: modules of Python 2 or optional ones that are not installed
(`BaseHTTPServer`, `SimpleHTTPServer`, `StringIO`, `cStringIO`, `dummy_threading`, `OpenSSL`, `chardet`, `setuptools`,
`simplejson`). 4 are not: `http` (`from http import cookiejar as cookielib`), `json` (`import json` after
`import simplejson as json` in a `try`), `urllib` (`from urllib.request import (... proxy_bypass ...)`) and `urllib3`
(`from urllib3.exceptions import SNIMissingWarning`, a name urllib3 2 no longer has, reported under its module).
Without the venv add `certifi`, `charset_normalizer`, `cryptography`, `idna`, `pygments`, `pytest` and `trustme`.

## `st` answers

With the venv. All answers took 0.04 s or less.

| Project | Command | Result | Output |
|---|---|---|---|
| Fixture | `callers shop.models.User.greet` | 1: `shop.services.OrderService.place(str, str, str)`, `shop/services.py:22` | 1 line |
| click | `callers click.utils.echo` | 214 callers: 160 in `tests/`, 37 in `examples/`, 17 in `src/` | 51 lines, 6.6 KB (50 and `+164 more`) |
| click | `impact click.core.Option.__init__ --group file` | 133 symbols in 20 files | 20 lines, 2.2 KB |
| click | `outline click.core.Context` | 52 members with signatures | 51 lines, 5.8 KB |
| requests | `callers requests.api.get` | 68 callers, all in `tests/` | 51 lines, 7.3 KB |
| requests | `impact requests.sessions.Session.send --group file` | 155 symbols in 5 files | 5 lines, 0.5 KB |
| requests | `outline requests.models.Response` | 37 members | 37 lines, 3.6 KB |

In both projects the tests link to `src/` (the indexer puts `src/` on the interpreter's path), and calls through a
name re-exported by the package's `__init__.py` (`click.echo`, `requests.get`) land on the function itself.

requests needs the second part of the fix. Pyright resolves `import requests` in `tests/` to the `requests` stubs it
bundles from typeshed, not to `src/requests`: requests 2.32 has no `py.typed`, and for such a package Pyright prefers
the stubs. With `src/`'s modules listed as the project's distribution, scip-python names the stubs' symbols after the
project's modules, which are the same names, so `requests.get(...)` and `s.send(...)` in the tests link to the code in
`src/`. Before, `callers requests.api.get` found 0 and `impact` of `Session.send` 16 symbols in 2 files. The cost:
4 nodes the stubs declare where the code only imports or re-exports a name, such as
`requests.utils.to_native_string()` next to the real `requests._internal_utils.to_native_string(string, encoding)`.

Dependencies typeshed has stubs for resolve the same way now when they are installed. In the tool's tests a fake `six`
in a venv outside the project resolves `six.ensure_str`; before the fix it was an unresolved import.

**`dist-packages`.** In a clean `ubuntu:22.04` with the system `python3` 3.10 and the apt packages `python3-requests`
and `python3-six`, a project importing both indexes with 0 errors, and `requests.get(url)` is a call to
`requests.api.get`. Debian's packages list no files, so they are matched by their name only: `six.ensure_str(url)`
gets no link (the `six` module itself does). A pip-installed distribution lists its files.

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
| One-file project | skipped likewise | 1 error (`unresolved import: requests`), `callees fetch.fetch` 0 | the same (before the fix wave; see `dist-packages` above for the indexer after it) |

The indexer's `scip-indexer: python3 not found; library symbols unresolved` line is shown by `st index --verbose`
only, as is `scip-indexer: library resolution off: <python> could not list its packages (...)`, which the fix wave
added for an interpreter that cannot answer. Without `python3` the index is the same as with one that lacks the
libraries.

## CI

`.github/workflows/build.yml` needed no new step: `npm ci` in the existing TypeScript step installs scip-python, the
runner's `python3` runs the venv tests, and `st-smoke.sh` now indexes the Python fixture too (index, `st status` row,
`callers shop.models.User.greet`; skipped with a note when `node` is missing). The step name and comments say Python,
and the `latest` prerelease notes no longer say Python returns later. The YAML parses; the steps were reproduced in the
container above; a run on GitHub is still to come with the first push of the branch.
