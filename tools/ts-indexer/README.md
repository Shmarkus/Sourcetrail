# ts-indexer

Indexes a TypeScript project (scip-typescript) or a Python project (`--language python`, scip-python) into a
Sourcetrail database. Sourcetrail runs it with Node.js 22.13 or newer from `data/ts-indexer`. The build stages it there
(`cmake/ts_indexer.cmake`).

```
node index.mjs --project <dir> --tsconfig <path> --out <db> [--append] [--http-out <file>]
node index.mjs --language python --project <dir> --out <db> [--append] [--python <python3>]
```

Tests: `npm ci && node --test .` (the tests that need a virtualenv skip without `python3`).

## Python

- Library imports resolve against an interpreter's search paths. The interpreter is `--python`, else the active
  virtualenv, `.venv` or `venv` in the project, else `python3` on PATH. The tool hands it to scip-python through a
  `/bin/sh` wrapper, so Python indexing runs on Linux and macOS only. In a src layout the wrapper also puts `src/` on
  `PYTHONPATH`.
- Without an interpreter, project code still indexes, and the standard library resolves from scip-python's typeshed.
  Each missing top-level module is one error row (`unresolved import: x`) and one entry in the stdout summary. Missing
  modules are never linked.
- `os.path` resolves to typeshed's `ntpath`: Pyright takes the first platform branch, so its functions show up as
  `ntpath.join` and so on.
- Bumping `@sourcegraph/scip-python` needs the tests rerun. `pyright.mjs` loads Pyright's parser out of scip-python's
  webpack bundle, which works only because the version is pinned; the tests fail if the bundle layout changes.
