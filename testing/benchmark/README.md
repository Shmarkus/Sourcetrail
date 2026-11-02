# st agent benchmark

The same questions are answered by fresh, headless Claude Code agents (`claude -p`) in two arms. Nothing else
differs between them:

| | grep arm | st arm |
|---|---|---|
| Working directory | a copy of the project without Sourcetrail files (no index, no delombok copies) | the indexed project |
| `st` | hidden: a shadow `st` answers "command not found" | on `PATH` |
| Project note | none | the CLAUDE.md snippet (`docs/agents/st-claude-md-snippet.md`) and a copy of the README's `st` section |
| Prompt, tools, model | the same: `Bash, Read, Grep, Glob`, no MCP servers | the same |

The prompt never mentions `st`, so the st arm also measures whether agents pick it up on their own.

**Measured, not estimated.** Tokens, cost, tool calls and tool output come from Claude Code's `stream-json` log.
Wall time is measured around each run. Total tokens add up the full context on every turn, so a large fixed context
(system prompt, hooks, skills) is paid again on each turn. **Context growth** leaves that floor out: the context after
the final answer minus the context of the first call, which is what the task itself added. Each answer ends in an `answer` block that is scored against the question's
ground truth: **correct** (every required item, no look-alike, nothing extra), **partial**, **wrong** or **no answer**.

`bench.py speed` times `st` itself: startup (`st status`), each question's reference query (with `--no-refresh`), and
the first query after a one-line edit, three times by default (`--refresh-repeats`; the edit is undone each time). A
refresh attempt counts only when `st` reports `refreshed N files`; otherwise the report says the refresh failed.

The report puts both against the success criteria in `docs/superpowers/specs/2026-10-30-sourcetrail-cli-requirements.md`.

## Run it on the public project

Needs: `claude` logged in, Python 3.8+, `st` and `Sourcetrail_cli` built, a JDK 17 for this project's Gradle 7.4
wrapper. Agents run with `--dangerously-skip-permissions`, so use a throwaway clone.

```bash
git clone https://github.com/gothinkster/spring-boot-realworld-example-app ~/bench/realworld
git -C ~/bench/realworld checkout ee17e31aafe733d98c4853c8b9a74d7f2f6c924a
cp testing/benchmark/projects/realworld.srctrlprj ~/bench/realworld/
JAVA_HOME=/path/to/jdk-17 Sourcetrail_cli index ~/bench/realworld/realworld.srctrlprj --full

cd testing/benchmark
# A cheap first check: two questions, one run each (4 agent runs).
python3 bench.py run --project ~/bench/realworld --questions questions/realworld.json --only Q1,Q9 --repeats 1
# The full run: 10 questions x 2 arms x 3 repeats = 60 agent runs.
python3 bench.py run --project ~/bench/realworld --questions questions/realworld.json --out results/realworld
python3 bench.py speed --project ~/bench/realworld --questions questions/realworld.json --out results/realworld \
    --edit-file src/main/java/io/spring/application/ArticleQueryService.java
```

`results/<name>/report.md` is the result. `runs.jsonl` holds one record per agent run, and `logs/` the full agent
logs. `--resume` continues an interrupted run, and `bench.py report --out DIR` rewrites the report.

When every run of a question agrees against the question file (all miss the same item, or all list the same extra),
the report lists it under "Check the ground truth": the agents are more often right than the question file. Fix the
question, then `bench.py rescore --out DIR --questions FILE` scores the stored answers again (the old records are kept).

**Freeze the build** you benchmark (copy `st` with `Sourcetrail`, `sourcetrail_indexer` and the data payload next to
it). `run` stops when `st` or a binary beside it changes during the run, keeping the finished runs for `--resume`;
`speed` refuses to time a refresh when `Sourcetrail` or `sourcetrail_indexer` is missing beside `st`.

Useful options: `--model` (the same for both arms), `--st /path/to/st`, `--arms st` (one arm only), `--timeout`,
`--max-turns`, `--bare` (no CLAUDE.md discovery at all; needs `ANTHROPIC_API_KEY`). Without `--bare`, your
`~/.claude/CLAUDE.md` applies to both arms. The run warns if it mentions `st`.

## Your own projects

Write a question file like `questions/realworld.json` and keep it out of the repo if the project is private (for
example in `.superpowers/benchmark/`). For each question:

- `kind`: `structural`, `stacktrace` or `text`. Adoption is counted on the first two. Text questions check that agents
  still use grep where grep fits.
- `prompt`: phrased as a developer would ask it. It must not name `st`, and the loader refuses one that does.
- `answer_items`: what one line of the answer is, for example "the calling methods as `Class.method`".
- `expected.required`: items that must appear. An item can be a list of alternatives.
  `expected.optional`: allowed but not needed (an interface hop on a path).
  `expected.forbidden`: look-alikes that must not appear. Set `expected.allow_extra` to `true` when other items are fine.
- `st_reference`: the `st` command whose time `speed` reports (optional).

Matching respects identifier boundaries: `ArticleApi.update` does not match `ArticleApi.updateArticle`, and a package
prefix or a parameter list around an item is fine. Build the ground truth with grep and reading, before any `st` run.

## Tests

```bash
cd testing/benchmark && python3 -m unittest discover -s tests -t .
```
