"""Runs every question through a fresh headless Claude Code agent per arm and records metrics and scores."""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import time
from dataclasses import dataclass, field

from .arms import build_prompt, claude_args, copy_without_sourcetrail, path_hiding_st, path_with_st
from .scoring import extract_answer, score
from .stream import parse_stream

RECORDS = "runs.jsonl"
_KEPT_METRICS = ("tool_calls", "st_calls", "st_invocations", "text_tool_calls", "used_st", "tool_output_chars",
                 "input_tokens", "cache_creation_tokens", "cache_read_tokens", "output_tokens", "total_tokens",
                 "cost_usd", "turns", "is_error", "error", "commands")


@dataclass
class RunConfig:
    project: str
    questions: list
    arms: list
    repeats: int
    out_dir: str
    st_binary: str
    note: str
    model: str | None = None
    bare: bool = False
    max_turns: int = 40
    timeout_s: int = 900
    env: dict = field(default_factory=lambda: dict(os.environ))
    # Outside every repository, so the grep arm can't wander into the question files or pick up a CLAUDE.md above it.
    work_dir: str | None = None


@dataclass
class Arm:
    name: str
    cwd: str
    env: dict
    note: str | None


def _prepare_arms(config: RunConfig) -> dict:
    work = config.work_dir or tempfile.mkdtemp(prefix="st-bench-")
    arms = {}
    if "grep" in config.arms:
        copy = os.path.join(work, "project-without-sourcetrail")
        if not os.path.isdir(copy):
            copy_without_sourcetrail(config.project, copy)
        env = dict(config.env, PATH=path_hiding_st(config.env.get("PATH", ""), os.path.join(work, "hide-st")))
        arms["grep"] = Arm("grep", copy, env, None)
    if "st" in config.arms:
        env = dict(config.env, PATH=path_with_st(config.env.get("PATH", ""), config.st_binary,
                                                 os.path.join(work, "with-st")))
        arms["st"] = Arm("st", config.project, env, config.note)
    return arms


def read_records(out_dir: str) -> list[dict]:
    path = os.path.join(out_dir, RECORDS)
    if not os.path.isfile(path):
        return []
    with open(path, encoding="utf-8") as handle:
        return [json.loads(line) for line in handle if line.strip()]


def _append_record(out_dir: str, record: dict) -> None:
    with open(os.path.join(out_dir, RECORDS), "a", encoding="utf-8") as handle:
        handle.write(json.dumps(record) + "\n")


def _run_claude(arm: Arm, args: list[str], log: str, timeout_s: int) -> tuple[float, str]:
    """Runs one agent, writing its stream to `log`. Returns the wall time and a failure note ("" if none)."""
    started = time.perf_counter()
    with open(log, "w", encoding="utf-8") as out, open(log + ".stderr", "w", encoding="utf-8") as err:
        try:
            done = subprocess.run(args, cwd=arm.cwd, env=arm.env, stdout=out, stderr=err, timeout=timeout_s)
            failure = "" if done.returncode == 0 else f"claude exit {done.returncode}"
        except subprocess.TimeoutExpired:
            failure = f"timeout after {timeout_s} s"
    return time.perf_counter() - started, failure


def run_one(config: RunConfig, arm: Arm, question: dict, repeat: int) -> dict:
    logs = os.path.join(config.out_dir, "logs")
    os.makedirs(logs, exist_ok=True)
    log = os.path.join(logs, f"{question['id']}-{arm.name}-{repeat}.jsonl")
    args = claude_args(build_prompt(question), config.model, arm.note, config.bare, config.max_turns)
    wall_s, failure = _run_claude(arm, args, log, config.timeout_s)
    with open(log, encoding="utf-8") as handle:
        metrics = parse_stream(handle)
    if failure:
        metrics["is_error"] = True
        metrics["error"] = f"{failure}; {metrics['error']}".rstrip("; ")
    answer = extract_answer(metrics["final_text"])
    record = {"question": question["id"], "kind": question["kind"], "arm": arm.name, "repeat": repeat,
              "answer": answer, "duration_s": wall_s, "cli_duration_s": metrics["duration_s"], "log": log}
    record.update(score(answer, question["expected"]))
    record.update({key: metrics[key] for key in _KEPT_METRICS})
    return record


def _arm_order(arms: list[str], position: int) -> list[str]:
    """Alternates which arm goes first, so neither always runs on a cold or warm cache."""
    return list(arms) if position % 2 == 0 else list(reversed(arms))


def run_benchmark(config: RunConfig, resume: bool = False, progress=print) -> list[dict]:
    os.makedirs(config.out_dir, exist_ok=True)
    if not resume and os.path.exists(os.path.join(config.out_dir, RECORDS)):
        os.remove(os.path.join(config.out_dir, RECORDS))
    done = {(r["question"], r["arm"], r["repeat"]) for r in read_records(config.out_dir)} if resume else set()
    arms = _prepare_arms(config)
    total = len(config.questions) * len(config.arms) * config.repeats
    count = len(done)
    for repeat in range(1, config.repeats + 1):
        for position, question in enumerate(config.questions):
            for name in _arm_order(config.arms, position + repeat):
                if (question["id"], name, repeat) in done:
                    continue
                record = run_one(config, arms[name], question, repeat)
                _append_record(config.out_dir, record)
                count += 1
                status = record["error"] if record["is_error"] else record["verdict"]
                progress(f"[{count}/{total}] {question['id']} {name} #{repeat}: {status}, "
                         f"{record['tool_calls']} tool calls, {record['total_tokens']:,} tokens, "
                         f"{record['duration_s']:.0f} s")
    return read_records(config.out_dir)
