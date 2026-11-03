"""Runs every question through a fresh headless Claude Code agent per arm and records metrics and scores."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field

from .armconfig import ArmSpec, load_arm, resolve
from .build import build_fingerprint, changed_files
from .arms import build_prompt, claude_args, copy_without_sourcetrail, path_hiding_st, path_with_st
from .commands import command_words
from .scoring import extract_answer, score
from .stream import parse_stream

RECORDS = "runs.jsonl"
WARM_UP = "warmup.jsonl"
_KEPT_METRICS = ("tool_calls", "st_calls", "st_invocations", "text_tool_calls", "used_st", "tool_output_chars",
                 "input_tokens", "cache_creation_tokens", "cache_read_tokens", "output_tokens", "total_tokens",
                 "cost_usd", "turns", "is_error", "error", "commands",
                 "api_calls", "initial_context_tokens", "final_context_tokens", "context_growth_tokens",
                 "tool_names", "model")


class BuildChanged(RuntimeError):
    """The st build was replaced during a run, so later runs would measure a different build."""


class WarmUpFailed(RuntimeError):
    """An arm's warm-up command failed, so its runs would measure a cold or broken tool."""


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
    # Arm name to spec; arms missing here are loaded from arms/<name>.json.
    specs: dict = field(default_factory=dict)
    # Arm name to note text; an arm whose spec has a note and no entry here gets `note`.
    notes: dict = field(default_factory=dict)
    skip_warm_up: bool = False
    startup_repeats: int | None = None


@dataclass
class Arm:
    name: str
    cwd: str
    env: dict
    note: str | None
    spec: ArmSpec | None = None
    mcp_config: str | None = None
    settings: str | None = None


def _write_json(path: str, data) -> str:
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(data, handle, indent=2)
    return path


def _prepare_arm(config: RunConfig, spec: ArmSpec, work: str) -> Arm:
    if spec.workdir == "copy":
        cwd = os.path.join(work, spec.copy_name)
        if not os.path.isdir(cwd):
            copy_without_sourcetrail(config.project, cwd)
    else:
        cwd = config.project
    st_dir = os.path.dirname(os.path.realpath(config.st_binary)) if config.st_binary else ""
    spec = resolve(spec, {"work": work, "workdir": cwd, "project": config.project, "st": config.st_binary,
                          "st_dir": st_dir, "python": sys.executable})
    path = config.env.get("PATH", "")
    if spec.st == "on_path":
        path = path_with_st(path, config.st_binary, os.path.join(work, "with-st"))
    else:
        path = path_hiding_st(path, os.path.join(work, "hide-st"))
    path = os.pathsep.join(spec.path_prepend + [path])
    env = dict(config.env, **spec.env)
    env["PATH"] = path
    note = config.notes.get(spec.name, config.note) if spec.note else None
    files = os.path.join(work, "arm-files")
    os.makedirs(files, exist_ok=True)
    mcp = (_write_json(os.path.join(files, f"{spec.name}-mcp.json"), {"mcpServers": spec.mcp_servers})
           if spec.mcp_servers else None)
    settings = _write_json(os.path.join(files, f"{spec.name}-settings.json"), spec.settings) if spec.settings else None
    return Arm(spec.name, cwd, env, note, spec, mcp, settings)


def _prepare_arms(config: RunConfig) -> dict:
    work = config.work_dir or tempfile.mkdtemp(prefix="st-bench-")
    return {name: _prepare_arm(config, config.specs.get(name) or load_arm(name), work) for name in config.arms}


def _shell(command: str, arm: Arm, timeout_s: int) -> tuple[float, int, str, str]:
    """Runs one arm command in the arm's directory and environment: wall time, exit code, stdout, stderr."""
    started = time.perf_counter()
    try:
        done = subprocess.run(command, shell=True, cwd=arm.cwd, env=arm.env, capture_output=True, text=True,
                              timeout=timeout_s)
        code, out, err = done.returncode, done.stdout, done.stderr
    except subprocess.TimeoutExpired:
        code, out, err = 124, "", f"timeout after {timeout_s} s"
    return time.perf_counter() - started, code, out, err


def _details(output: str) -> dict:
    """A command may end its stdout with one JSON object line: its own measurements (time to ready, ...)."""
    lines = output.strip().splitlines()
    try:
        value = json.loads(lines[-1]) if lines else None
    except json.JSONDecodeError:
        return {}
    return value if isinstance(value, dict) else {}


def read_warm_up(out_dir: str) -> list[dict]:
    path = os.path.join(out_dir, WARM_UP)
    if not os.path.isfile(path):
        return []
    with open(path, encoding="utf-8") as handle:
        return [json.loads(line) for line in handle if line.strip()]


def _arm_phase(config: RunConfig, arm: Arm, phase: str, command: str, timeout_s: int = 1800) -> dict:
    seconds, code, out, err = _shell(command, arm, timeout_s)
    output = out + err
    entry = {"arm": arm.name, "phase": phase, "seconds": seconds, "exit": code, "details": _details(out),
             "date": time.strftime("%Y-%m-%d %H:%M:%S")}
    with open(os.path.join(config.out_dir, WARM_UP), "a", encoding="utf-8") as handle:
        handle.write(json.dumps(entry) + "\n")
    if code:
        entry["output"] = output[-3000:]
    return entry


def _warm_up(config: RunConfig, arms: dict, progress) -> None:
    """Warm-up, then per-session startup, once per arm before any agent run; timed apart from the questions."""
    for arm in arms.values():
        spec = arm.spec
        if spec.warm_up:
            entry = _arm_phase(config, arm, "warm_up", spec.warm_up)
            if entry["exit"]:
                raise WarmUpFailed(f"warm-up of arm {arm.name} failed (exit {entry['exit']}):\n{entry['output']}")
            progress(f"warm-up {arm.name}: {entry['seconds']:.1f} s {json.dumps(entry['details'])}")
        if spec.startup:
            for _ in range(config.startup_repeats or spec.startup_repeats):
                entry = _arm_phase(config, arm, "startup", spec.startup)
                if entry["exit"]:
                    raise WarmUpFailed(f"startup probe of arm {arm.name} failed (exit {entry['exit']}):\n"
                                       f"{entry['output']}")
                progress(f"startup {arm.name}: {entry['seconds']:.1f} s {json.dumps(entry['details'])}")


def _shut_down(config: RunConfig, arms: dict, progress) -> None:
    for arm in arms.values():
        if arm.spec.shut_down:
            entry = _arm_phase(config, arm, "shut_down", arm.spec.shut_down, timeout_s=120)
            if entry["exit"]:
                progress(f"shut-down {arm.name} exited {entry['exit']}: {entry['output'][-500:]}")


def arm_tool_calls(metrics: dict, adoption: dict) -> int | None:
    """Calls of the arm's own tool: by tool name, tool-name prefix (an MCP server) or program run through Bash.
    None for an arm without a tool of its own."""
    tools, prefixes, programs = (adoption.get(key, []) for key in ("tools", "tool_prefixes", "programs"))
    if not (tools or prefixes or programs):
        return None
    calls = sum(count for name, count in metrics["tool_names"].items()
                if name in tools or any(name.startswith(prefix) for prefix in prefixes))
    calls += sum(1 for command in metrics["commands"] if any(word in programs for word in command_words(command)))
    return calls


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
    spec = arm.spec
    args = claude_args(build_prompt(question), config.model, arm.note, config.bare, config.max_turns,
                       tools=spec.tools if spec else None, allowed_tools=spec.allowed_tools if spec else None,
                       mcp_config=arm.mcp_config, settings=arm.settings)
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
    calls = arm_tool_calls(metrics, spec.adoption if spec else {})
    record["arm_tool_calls"] = calls
    record["used_arm_tool"] = None if calls is None else calls > 0
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
    try:
        if not config.skip_warm_up:
            _warm_up(config, arms, progress)
        _run_questions(config, arms, done, progress)
    finally:
        if not config.skip_warm_up:
            _shut_down(config, arms, progress)
    return read_records(config.out_dir)


def _run_questions(config: RunConfig, arms: dict, done: set, progress) -> None:
    fingerprint = build_fingerprint(config.st_binary) if config.st_binary else {}
    total = len(config.questions) * len(config.arms) * config.repeats
    count = len(done)
    for repeat in range(1, config.repeats + 1):
        for position, question in enumerate(config.questions):
            for name in _arm_order(config.arms, position + repeat):
                if (question["id"], name, repeat) in done:
                    continue
                if fingerprint:
                    changed = changed_files(fingerprint, build_fingerprint(config.st_binary))
                    if changed:
                        raise BuildChanged(f"the st build changed during the run ({', '.join(changed)}); "
                                           "finished runs are kept: freeze the build and continue with --resume")
                record = run_one(config, arms[name], question, repeat)
                _append_record(config.out_dir, record)
                count += 1
                status = record["error"] if record["is_error"] else record["verdict"]
                progress(f"[{count}/{total}] {question['id']} {name} #{repeat}: {status}, "
                         f"{record['tool_calls']} tool calls, {record['total_tokens']:,} tokens, "
                         f"{record['duration_s']:.0f} s")
