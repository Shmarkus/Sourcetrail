#!/usr/bin/env python3
"""st agent benchmark: the same questions answered by fresh Claude Code agents with grep only and with st.

    python3 bench.py run    --project DIR --questions FILE [--repeats 3] [--out DIR]
    python3 bench.py speed  --project DIR --questions FILE [--edit-file REL] [--out DIR]
    python3 bench.py rescore --out DIR --questions FILE
    python3 bench.py report --out DIR

See README.md next to this file.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

from stbench.arms import path_with_st, readme_agent_section, st_note  # noqa: E402
from stbench.build import missing_neighbours  # noqa: E402
from stbench.questions import QuestionError, load_questions  # noqa: E402
from stbench.report import render_markdown, summarize  # noqa: E402
from stbench.rescore import rescore  # noqa: E402
from stbench.runner import RECORDS, BuildChanged, RunConfig, read_records, run_benchmark  # noqa: E402
from stbench.speed import measure  # noqa: E402

README = os.path.join(REPO, "README.md")
SNIPPET = os.path.join(REPO, "docs", "agents", "st-claude-md-snippet.md")


def _fail(message: str) -> None:
    print(f"bench: {message}", file=sys.stderr)
    sys.exit(2)


def _st_binary(given: str | None) -> str:
    binary = given or shutil.which("st")
    if not binary or not os.access(binary, os.X_OK):
        _fail("st not found; put it on PATH or pass --st /path/to/build/Release/app/st")
    return os.path.abspath(binary)


def _st(binary: str, command: str, project: str) -> subprocess.CompletedProcess:
    return subprocess.run(f"{shlex.quote(binary)} {command}", shell=True, cwd=project, capture_output=True, text=True)


def _preflight(args, data: dict, st_binary: str | None) -> None:
    if not shutil.which("claude"):
        _fail("claude (Claude Code CLI) is not on PATH")
    if st_binary:
        missing = missing_neighbours(st_binary)
        if missing:
            print(f"bench: warning: {', '.join(missing)} not found next to {os.path.realpath(st_binary)}; "
                  "st can't refresh the index, so any stale file makes queries fail")
        status = _st(st_binary, "status", args.project)
        if status.returncode != 0:
            _fail(f"`st status` failed in {args.project} (exit {status.returncode}): {status.stderr.strip()}\n"
                  "Index the project first: Sourcetrail_cli index /abs/project.srctrlprj --full")
        stale = re.search(r"stale_files\t(\d+)", status.stdout)
        if stale and int(stale.group(1)):
            print(f"bench: index has {stale.group(1)} stale files; refreshing once before the runs")
            _st(st_binary, "find st_bench_probe", args.project)
    expected_commit = data.get("project", {}).get("commit")
    if expected_commit:
        head = subprocess.run(["git", "-C", args.project, "rev-parse", "HEAD"], capture_output=True, text=True)
        if head.returncode == 0 and not head.stdout.strip().startswith(expected_commit[:12]):
            print(f"bench: warning: project is at {head.stdout.strip()[:12]}, the questions were written for "
                  f"{expected_commit[:12]}; answers may differ")
    if os.path.realpath(args.project).startswith(os.path.realpath(REPO) + os.sep):
        print("bench: warning: the project is inside the Sourcetrail repository, next to the question files; "
              "clone it elsewhere")
    for name in ("CLAUDE.md", "AGENTS.md"):
        if os.path.exists(os.path.join(args.project, name)):
            print(f"bench: warning: the project has {name}; both arms will read it")
    user_memory = os.path.expanduser("~/.claude/CLAUDE.md")
    if not args.bare and os.path.isfile(user_memory):
        with open(user_memory, encoding="utf-8") as handle:
            if re.search(r"(?<![\w./-])st(?![\w-])", handle.read()):
                print("bench: warning: ~/.claude/CLAUDE.md mentions st, and the grep arm reads it too; "
                      "use --bare (needs ANTHROPIC_API_KEY) or move that note while benchmarking")


def _note(work: str) -> str:
    with open(README, encoding="utf-8") as handle:
        section = readme_agent_section(handle.read())
    readme_copy = os.path.join(work, "st-README.md")
    with open(readme_copy, "w", encoding="utf-8") as handle:
        handle.write(section)
    with open(SNIPPET, encoding="utf-8") as handle:
        return st_note(handle.read(), readme_copy)


def _output(command: list[str]) -> str:
    try:
        done = subprocess.run(command, capture_output=True, text=True)
    except OSError:
        return ""
    return done.stdout.strip() if done.returncode == 0 else ""


def _write_meta(out_dir: str, args, arms: list[str], project_name: str) -> None:
    meta = {
        "project": project_name,
        "date": time.strftime("%Y-%m-%d %H:%M"),
        "questions": os.path.relpath(os.path.abspath(args.questions), REPO),
        "project_commit": _output(["git", "-C", args.project, "rev-parse", "--short=12", "HEAD"]),
        "sourcetrail_commit": _output(["git", "-C", REPO, "rev-parse", "--short=12", "HEAD"]),
        "claude_version": _output(["claude", "--version"]),
        "model": args.model or "Claude Code default",
        "repeats": args.repeats,
        "arms": ", ".join(arms),
    }
    with open(os.path.join(out_dir, "meta.json"), "w", encoding="utf-8") as handle:
        json.dump(meta, handle, indent=2)


def _read_json(path: str):
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def _write_report(out_dir: str) -> str:
    runs = read_records(out_dir)
    speed = _read_json(os.path.join(out_dir, "speed.json"))
    meta = _read_json(os.path.join(out_dir, "meta.json")) or {}
    title = f"st benchmark: {meta['project']}" if meta.get("project") else "st benchmark"
    report = render_markdown(summarize(runs), runs, speed, title=title, meta=meta)
    path = os.path.join(out_dir, "report.md")
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(report)
    return path


def _default_out(project_name: str) -> str:
    return os.path.join(HERE, "results", f"{project_name}-{time.strftime('%Y%m%d-%H%M%S')}")


def _load(args) -> dict:
    try:
        return load_questions(args.questions, args.only.split(",") if args.only else None)
    except (QuestionError, OSError, json.JSONDecodeError) as error:
        _fail(str(error))


def cmd_run(args) -> None:
    data = _load(args)
    arms = args.arms.split(",")
    st_binary = _st_binary(args.st) if "st" in arms else None
    _preflight(args, data, st_binary)
    out_dir = args.out or _default_out(data.get("project", {}).get("name", "project"))
    os.makedirs(out_dir, exist_ok=True)
    work = tempfile.mkdtemp(prefix="st-bench-")
    config = RunConfig(
        project=os.path.abspath(args.project), questions=data["questions"], arms=arms, repeats=args.repeats,
        out_dir=out_dir, st_binary=st_binary or "", note=_note(work) if st_binary else "",
        model=args.model, bare=args.bare, max_turns=args.max_turns, timeout_s=args.timeout, work_dir=work,
    )
    if not args.resume or not os.path.isfile(os.path.join(out_dir, "meta.json")):
        _write_meta(out_dir, args, arms, data.get("project", {}).get("name", ""))
    runs = len(config.questions) * len(arms) * args.repeats
    print(f"bench: {runs} agent runs into {out_dir}")
    try:
        run_benchmark(config, resume=args.resume)
    except BuildChanged as error:
        print(f"bench: {error}", file=sys.stderr)
        print(f"bench: partial report {_write_report(out_dir)}", file=sys.stderr)
        sys.exit(3)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print(f"bench: report {_write_report(out_dir)}")


def cmd_speed(args) -> None:
    data = _load(args)
    st_binary = _st_binary(args.st)
    missing = missing_neighbours(st_binary)
    if args.edit_file and missing:
        _fail(f"{', '.join(missing)} not found next to {os.path.realpath(st_binary)}; st can't refresh without them, "
              "so the refresh time would measure a failure")
    out_dir = args.out or _default_out(data.get("project", {}).get("name", "project"))
    os.makedirs(out_dir, exist_ok=True)
    link_dir = tempfile.mkdtemp(prefix="st-bench-")
    env = dict(os.environ, PATH=path_with_st(os.environ.get("PATH", ""), st_binary, link_dir))
    try:
        result = measure(os.path.abspath(args.project), data["questions"], args.edit_file, args.repeats, env,
                         refresh_repeats=args.refresh_repeats)
    finally:
        shutil.rmtree(link_dir, ignore_errors=True)
    failed = [timing for timing in [result["startup"]] + result["queries"]
              if any(code not in (0, 1) for code in timing["exit_codes"])]
    for timing in failed:
        print(f"bench: warning: `{timing['command']}` exited {timing['exit_codes']}; its time is not a query time",
              file=sys.stderr)
    with open(os.path.join(out_dir, "speed.json"), "w", encoding="utf-8") as handle:
        json.dump(result, handle, indent=2)
    print(f"bench: report {_write_report(out_dir)}")


def cmd_report(args) -> None:
    print(_write_report(args.out))


def cmd_rescore(args) -> None:
    records = read_records(args.out)
    if not records:
        _fail(f"no {RECORDS} in {args.out}")
    questions = {q["id"]: q for q in _load(args)["questions"]}
    updated = rescore(records, questions)
    path = os.path.join(args.out, RECORDS)
    backup = f"{path}.before-rescore-{time.strftime('%Y%m%d-%H%M%S')}"
    shutil.copyfile(path, backup)
    with open(path, "w", encoding="utf-8") as handle:
        handle.writelines(json.dumps(record) + "\n" for record in updated)
    changed = sum(old["verdict"] != new["verdict"] for old, new in zip(records, updated))
    print(f"bench: {changed} of {len(records)} verdicts changed; the old records are in {backup}")
    print(f"bench: report {_write_report(args.out)}")


def main(argv=None) -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    def common(sub):
        sub.add_argument("--project", required=True, help="indexed project checkout (where its .srctrlprj is found)")
        sub.add_argument("--questions", required=True, help="question file (JSON)")
        sub.add_argument("--only", help="comma-separated question ids")
        sub.add_argument("--st", help="st binary (default: st on PATH)")
        sub.add_argument("--out", help="result directory (default: results/<project>-<time>)")

    run = commands.add_parser("run", help="agent runs, grep arm against st arm")
    common(run)
    run.add_argument("--arms", default="grep,st")
    run.add_argument("--repeats", type=int, default=3, help="runs per question and arm (default 3)")
    run.add_argument("--model", help="model for both arms (default: the CLI's default)")
    run.add_argument("--bare", action="store_true", help="claude --bare: no CLAUDE.md discovery (needs ANTHROPIC_API_KEY)")
    run.add_argument("--max-turns", type=int, default=40)
    run.add_argument("--timeout", type=int, default=900, help="seconds per agent run")
    run.add_argument("--resume", action="store_true", help="keep finished runs in --out and do the rest")
    run.set_defaults(func=cmd_run)

    speed = commands.add_parser("speed", help="time st: startup, reference queries, refresh after an edit")
    common(speed)
    speed.add_argument("--edit-file", help="project-relative Java file to append a comment line to (restored after)")
    speed.add_argument("--repeats", type=int, default=5)
    speed.add_argument("--refresh-repeats", type=int, default=3, help="edit-and-query attempts (default 3)")
    speed.set_defaults(func=cmd_speed)

    again = commands.add_parser("rescore", help="score the stored answers again against a corrected question file")
    again.add_argument("--out", required=True)
    again.add_argument("--questions", required=True)
    again.set_defaults(func=cmd_rescore, only=None)

    report = commands.add_parser("report", help="rewrite report.md from runs.jsonl and speed.json")
    report.add_argument("--out", required=True)
    report.set_defaults(func=cmd_report)

    args = parser.parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    main()
