# st agent benchmark

The same questions are answered by fresh, headless Claude Code agents (`claude -p`) in several arms. Each arm is one
file in `arms/`; nothing else differs between them:

| | grep | st | intellij | jdtls |
|---|---|---|---|---|
| Working directory | a copy of the project without Sourcetrail files (no index, no delombok copies) | the indexed project | its own clean copy | its own clean copy |
| `st` | hidden: a shadow `st` answers "command not found" | on `PATH` | hidden | hidden |
| Own tool | none | `st` through Bash | IntelliJ IDEA's MCP server, navigation and search tools only | Claude Code's `LSP` tool (jdtls-lsp plugin) |
| Project note | none | the CLAUDE.md snippet (`docs/agents/st-claude-md-snippet.md`) and a copy of the README's `st` section | `arms/intellij-note.md` | `arms/jdtls-note.md` |
| Warm-up | none | `st serve --detach` | start the IDE, Gradle import, indexing | jdtls import and build |
| Prompt, model, base tools | the same: `Bash, Read, Grep, Glob`, MCP servers only from the arm's file | the same | the same | the same |

The prompt never names a tool, so each tool arm also measures whether agents pick its tool up on their own.
Every arm's tools are allowed up front (`--tools`, `--allowedTools`, `--strict-mcp-config`); no arm skips permission
checks, and the report totals each arm's permission denials. The intellij arm allows each whitelisted MCP tool by
name, and its warm-up fails if the server offers any other tool or lets `execute_tool` run one. The arms take turns
going first: the order rotates by one arm per question.

**No arm may see the answers.** Each run's Bash commands and Read, Grep and Glob paths are checked for the question
file's name and folder, `/questions/`, `.superpowers` and the results folder; a run that names one is flagged in its
record (`leaks`) and listed in the report. Keep `--work` free of question files and results (the run refuses a work
folder that holds either, or one inside this repository).

**Measured, not estimated.** Tokens, cost, tool calls and tool output come from Claude Code's `stream-json` log.
Wall time is measured around each run. Total tokens add up the full context on every turn, so a large fixed context
(system prompt, hooks, skills) is paid again on each turn. **Context growth** leaves that floor out: the context after
the final answer minus the context of the first call, which is what the task itself added. Each answer ends in an `answer` block that is scored against the question's
ground truth: **correct** (every required item, no look-alike, nothing extra), **partial**, **wrong** or **no answer**.

`bench.py speed` times `st` itself: startup (`st status`), each question's reference query (with `--no-refresh`), and
the first query after a one-line edit, three times by default (`--refresh-repeats`; the edit is undone each time). A
refresh attempt counts only when `st` reports `refreshed N files`; otherwise the report says the refresh failed.

The report puts the arms against the success criteria in `docs/superpowers/specs/2026-10-30-sourcetrail-cli-requirements.md`,
each with its change against grep, and counts **adoption** per arm: the runs that called the arm's own tool, and the
calls by tool name.

**Warm-up is not question time.** Before the first question each arm runs its warm-up command once (timed; a tool may
also report its own measure, such as the IDE's uptime when it left dumb mode) and its per-session startup probe a few
times. jdtls starts with every agent session that calls LSP, so its startup is also inside those question times; the
report shows the startup apart and, from the wrapper's start log (`session_log` in the arm file), each run's
`tool_started_in_session` and the median question time with and without a start. After the last question each arm's shut-down command runs, also on failure.
`warmup.jsonl` holds the entries, and `meta.json` the tool versions (each arm's `version` command), the Claude Code
version and the models seen in the runs.

## Run it on the public project

Needs: `claude` logged in, Python 3.8+, `st` built (with `Sourcetrail` and `sourcetrail_indexer` beside it), a JDK 17
for this project's Gradle 7.4 wrapper. Agents may run any Bash command, so use a throwaway clone.

```bash
git clone https://github.com/gothinkster/spring-boot-realworld-example-app ~/bench/realworld
git -C ~/bench/realworld checkout ee17e31aafe733d98c4853c8b9a74d7f2f6c924a
cp testing/benchmark/projects/realworld.srctrlprj ~/bench/realworld/
JAVA_HOME=/path/to/jdk-17 st index --full --project ~/bench/realworld/realworld.srctrlprj

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

Useful options: `--model` (the same for every arm), `--st /path/to/st`, `--arms st` (one arm only), `--timeout`,
`--max-turns`, `--bare` (no CLAUDE.md discovery at all; needs `ANTHROPIC_API_KEY`), `--work DIR` (keep the arms'
copies and tool state), `--no-warm-up` (the tools are already warm). Without `--bare`, your `~/.claude/CLAUDE.md`
applies to every arm. The run warns if it mentions `st`.

## Arms

`--arms grep,st,intellij,jdtls` takes arm names (`arms/<name>.json`) or paths of arm files. An arm file holds:

- `name`, `description`;
- `workdir`: `copy` (a clean copy of the project per arm, at `copy_name` under the work folder) or `project` (the
  `--project` folder as given, the indexed one);
- `st`: `hidden` or `on_path` (the `--st` binary); `path_prepend` and `env` for everything else on PATH and in the
  environment;
- `tools` (`--tools`) and `allowed_tools` (`--allowedTools`, default: `tools`); `mcp_servers` (written to a file for
  `--mcp-config`) and `settings` (`--settings`);
- `note`: the project note, a file path relative to the arm file (`note_readme_section` adds the README's `st` section);
- `adoption`: the arm's own tool by `tools` name, `tool_prefixes` (an MCP server) or `programs` run through Bash;
- `warm_up`, `startup` (`startup_repeats` times), `shut_down` and `version`: shell commands, run in the arm's working
  directory with its environment. A command may end its stdout with a JSON object line: its own measurements;
  `warm_up_s` there (the IDE's ready time) is reported as the warm-up instead of the command's wall time, and
  `startup_s` as the startup;
- `session_log`: a file the tool appends `<epoch seconds> start` lines to, to count starts inside each run.

Strings may hold placeholders: `{work}`, `{workdir}`, `{project}`, `{st}`, `{st_dir}`, `{python}`, `{arms}` (the arm
file's folder), `{repo}`, and the arm's `vars`, which the environment overrides (`BENCH_INTELLIJ_PORT=64902 bench.py
run ...`).

**intellij** needs IntelliJ IDEA 2025.2 or later (its bundled MCP server plugin). `arms/intellij-mcp.py` starts
`idea.sh mcpServer` headless as a second IDE process with its own config, system and log folders under the work
folder, so an IDE you have open is left alone, and stops it after the run. The server exposes only the tools in
`BENCH_INTELLIJ_TOOLS`. Set `BENCH_IDEA` (path of `idea.sh`), and `BENCH_GRADLE_JAVA_HOME` to the JDK the project's
Gradle build needs.

**jdtls** needs jdtls (`BENCH_JDTLS_HOME`, default `~/.local/share/jdtls`), a JDK 21 to run it
(`BENCH_JDTLS_JAVA_HOME`) and Claude Code's `jdtls-lsp` plugin enabled. The plugin starts `jdtls` from PATH; the arm
puts `arms/jdtls-bin/jdtls` first, which adds the Lombok agent (`BENCH_LOMBOK_JAR`, default: the newest in the Gradle
cache) and keeps the workspace in the work folder. Its warm-up and startup probe (`arms/jdtls-probe.py`) need a class
of the project to look up: `BENCH_JDTLS_PROBE_SYMBOL`.

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
