# Python indexing

Date: 2026-11-04. Status: approved in chat (the user asked to skip the questions; the calls below are mine).

## Why

The fork should appeal to a wider audience than Java and TypeScript users. Python came out with the move to the new
base (upstream removed it). It comes back the same way TypeScript came in, reusing that work.

## Goals

1. A project folder with a Python project (`pyproject.toml`, `setup.py`, `setup.cfg` or `requirements*.txt` at its
   root, or `.py` files with no other build) gets its Python indexed into the project's index: modules (from the
   package path), classes, functions, methods, fields/attributes, module variables, and call, usage, type-usage,
   inheritance, override, import and decorator links, with source locations.
2. It runs through the same machinery as TypeScript: `scip-python` (Sourcegraph, Pyright-based, shipped in the package
   with the TypeScript tool) produces SCIP, the existing converter writes a Sourcetrail sub-database, a "Python Source
   Group" type with group-scoped clearing imports it.
3. Refresh: the Python step re-runs only when `*.py`, `pyproject.toml`, `setup.py`, `setup.cfg`, `requirements*.txt`
   or the lockfile (`poetry.lock`, `uv.lock`, `Pipfile.lock`) change. A Java- or TypeScript-only edit never runs it,
   and a `.py` edit never re-indexes Java or TypeScript.
4. `st` answers on Python like on the other languages (`find`, `callers`, `callees`, `impact`, `refs`, `outline`,
   `impls`, `supers`, `path`), and its freshness check knows the Python trigger files.
5. GUI: New Project detects Python as above and adds a Python part (alone, or next to Gradle/Maven/TypeScript in one
   project). Project settings add/remove it; New Custom Project offers it.
6. Library types resolve when the project's Python environment is found: an active virtualenv, `.venv/` or `venv/` in
   the project, otherwise the system `python3`. Without one, project code still indexes; unresolved library symbols
   are reported, never invented.

## Non-goals

- Linking Python HTTP endpoints (Flask/FastAPI/Django) to frontends.
- Jupyter notebooks, Cython, `.pyi`-only packages as sources.
- Several Python projects in one folder (one Python part per project, like TypeScript).
- Python 2.

## Anti-goals

- Java and TypeScript refresh or index contents change because Python was added (compare-index identical on the
  existing fixtures).
- A project with no Python sees any change in `st` output or refresh behaviour.
- Private names in committed files.

## Constraints

- Same prerequisites as TypeScript: Node.js 22.13+. Python indexing also needs `python3` on the indexing machine for
  library resolution (optional; without it, only project code resolves).
- Reuse, don't fork: the converter (`tools/ts-indexer/convert.mjs`, `sqlite.mjs`) and the whole-group source-group
  machinery (`SourceGroupTypeScript`, inputs record, group-scoped clearing, skip/failure statuses) are generalised so
  both languages share them. TypeScript behaviour and tests stay unchanged.
- Every commit builds and passes the tests; tests first.

## Left to the builder

- Whether the shared tool is renamed (e.g. `tools/scip-indexer/` with `--language`) or Python is added to the existing
  tool; keep the packaging path working.
- How declaration extents are found for Python (scip-python's `enclosing_range`, or a syntax pass with Python's own
  `ast` module via `python3`, or a JS Python parser) — pick the one that needs no extra runtime when possible.
- Name delimiter and module naming (`.`; packages → modules from the import path).
