# Python Indexing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Index Python projects through scip-python and the existing SCIP converter, as a Python source group next to Java and TypeScript, with st and GUI support.

**Architecture:** Generalise the TypeScript pipeline (Node tool → SCIP → Sourcetrail sub-db; whole-group source group with group-scoped clearing and an inputs record) so a second language plugs in; add `scip-python`, a Python name mapping, a Python source group, detection, and packaging.

**Tech Stack:** Node ≥ 22.13 (node:sqlite, node:test), `@sourcegraph/scip-python` 0.6.x, C++20/Qt 6.8, gtest.

**Spec:** `docs/superpowers/specs/2026-11-04-python-indexing-design.md`

## Global Constraints

- TypeScript behaviour, output and tests unchanged; Java unchanged (compare-index identical on testing/cli/{lombok,maven,angular-spring}).
- A Java/TypeScript-only edit never runs the Python step; a `.py` edit never re-indexes Java or TypeScript.
- Python trigger files: `*.py`, `pyproject.toml`, `setup.py`, `setup.cfg`, `requirements*.txt`, `poetry.lock`, `uv.lock`, `Pipfile.lock`; walk skips `node_modules`, `.sourcetrail`, `build`, `dist`, `.git`, `.venv`, `venv`, `__pycache__`, `.tox`, `.mypy_cache`.
- Status lines mirror TypeScript: `Python skipped: …`, `Python failed (exit code n): …`.
- One Python part per project. No private names. Commits end with the session's attribution lines; tests first; never `git add -A`; push only when the user asks.

## Review Focus

1. A project with a `.venv` vs none: library calls (`requests.get`) resolve to a library node when the env exists, and are reported unresolved (not invented) without it. Task 1.
2. A package with `__init__.py` re-exports (`from .models import User`): references through the package land on the real class. Task 1.
3. Mixed Gradle + Python folder: a `.py` edit refreshes only Python; a `.java` edit never runs scip-python. Task 2.
4. Python test files (`test_*.py`, `*_test.py`, `tests/`) are indexed; `st callers` includes them (no special filter). Task 1.
5. A file with a syntax error: indexed with an error row, the rest of the project indexes. Task 1.

---

### Task 1: Python in the SCIP tool

**Files:** generalise `tools/ts-indexer/` (name it `tools/scip-indexer/` with `index.mjs --language typescript|python`, keeping `tools/ts-indexer` behaviour via the same CLI defaults — builder's call on renaming; update `cmake/ts_indexer.cmake` and packaging paths accordingly); add `@sourcegraph/scip-python` pinned; Python name mapping; `testing/cli/python/` fixture (a package `shop/` with `models.py` (classes with inheritance, a dataclass, properties), `services.py` (functions/methods calling models, a decorator), `__init__.py` re-exporting, `tests/test_services.py` (pytest), `pyproject.toml`; and a `.venv`-free setup).

**Interfaces:** CLI `node tools/scip-indexer/index.mjs --language python --project <dir> --out <db> [--append] [--python <python3 path>]`; exit codes as TypeScript (0 ok, 3 skipped: e.g. `python3 not found; library symbols unresolved` is NOT a skip — project code still indexes; skip only when scip-python itself can't run), stdout `scip-indexer: <n> files, <m> errors`.

- [ ] Tests first (node:test on the fixture): names `shop.models.User`, `shop.services.OrderService.place()`; CALL edges from `place` to `User.__init__`/methods; inheritance `Admin → User`; decorator as annotation usage; re-export through `shop/__init__.py` lands on `shop.models.User` (Review Focus 2); test function `tests.test_services.test_place` calls `place` (Focus 4); a file with a syntax error → error row, others indexed (Focus 5); no `.venv` → library call reported unresolved, with a stub `.venv` (tiny fake site-packages module) → resolves (Focus 1).
- [ ] Implement; TypeScript tests still 57/57 (or more) unchanged.
- [ ] Commit.

### Task 2: Python source group

**Files:** generalise `SourceGroupTypeScript` into a shared SCIP-group base (whole-group clearing, inputs record, trigger patterns, skip/failure statuses) with TypeScript and Python subclasses; `SOURCE_GROUP_PYTHON` ("Python Source Group") before `SOURCE_GROUP_CUSTOM_COMMAND`; `SourceGroupSettingsPython` (project root relative, optional python interpreter path); ProjectSettings load/save.

- [ ] Tests first (gtest, mirror TypeScriptSourceGroupTestSuite): mixed project indexes both; Java edit and TS edit don't run Python; `.py` edit re-indexes only Python; deleted `.py` cleared; row counts stable after two refreshes; missing Node → `Python skipped: …`, others index; one Python group per project enforced like TypeScript (Focus 3).
- [ ] Implement; all TypeScript source-group tests unchanged and green; compare-index identical on lombok/maven/angular-spring.
- [ ] Commit.

### Task 3: st and GUI

**Files:** `src/st/StFreshness.cpp` (Python trigger files), `st status` row `python`; `src/lib/project/ProjectSetup.{h,cpp}` (`DetectedBuild` gains `python`), `QtMainWindow` New Project, wizard type list, project settings page (interpreter path, prefilled from `.venv`/`venv`).

- [ ] Tests first: detection (pyproject/setup.py/setup.cfg/requirements*.txt/py-only; Python next to Gradle and TypeScript), createProject writes the Python group; st freshness sees a `.py` edit and a `pyproject.toml` edit; `st status` python row only with a Python group; Java/TS-only projects' st output byte-identical.
- [ ] GUI check under Xvfb with a scratch HOME on a copy of testing/cli/python: New Project → Python project, indexes with 0 errors, `st callers` works.
- [ ] Commit.

### Task 4: CI, packaging, docs

- [ ] CI: scip-python installed via the tool's `npm ci`; node tests; a Python section in st-smoke.sh (index the fixture; `st callers shop.services.OrderService.place`).
- [ ] Package ships the Python tool deps; 22.04 container check with and without `python3`.
- [ ] README/DOCUMENTATION.md (user rewrote docs — keep their style): Python entry under Supported Languages, prerequisites, what is resolved.
- [ ] Results: `docs/superpowers/specs/2026-11-04-python-indexing-results.md` with fixture numbers and one public Python project (e.g. a pinned commit of a small, well-known open-source package — builder's pick) — files, errors, index time, a few st answers.
- [ ] Commit (no push).
