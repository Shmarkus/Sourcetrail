"""Times `st` itself: startup, each question's reference query, and the refresh after a one-line edit."""

from __future__ import annotations

import os
import re
import subprocess
import time

_REFRESHED = re.compile(r"refreshed (\d+) files in ([0-9.]+)s")
EDIT_MARKER = "// st-bench edit"


def median(values: list[float]) -> float | None:
    ordered = sorted(values)
    if not ordered:
        return None
    middle = len(ordered) // 2
    return ordered[middle] if len(ordered) % 2 else (ordered[middle - 1] + ordered[middle]) / 2


def parse_refreshed(stderr: str) -> tuple[int, float] | None:
    match = _REFRESHED.search(stderr)
    return (int(match.group(1)), float(match.group(2))) if match else None


def _timed(command: str, cwd: str, env: dict) -> tuple[float, subprocess.CompletedProcess]:
    started = time.perf_counter()
    done = subprocess.run(command, shell=True, cwd=cwd, env=env, capture_output=True, text=True)
    return time.perf_counter() - started, done


def _repeat(command: str, cwd: str, env: dict, repeats: int) -> dict:
    seconds, exit_codes = [], set()
    for _ in range(repeats):
        elapsed, done = _timed(command, cwd, env)
        seconds.append(elapsed)
        exit_codes.add(done.returncode)
    return {"command": command, "median_s": median(seconds), "max_s": max(seconds), "exit_codes": sorted(exit_codes)}


def with_no_refresh(command: str) -> str:
    """Adds --no-refresh to every `st` in a pipeline, so query timing never includes a refresh."""
    return re.sub(r"(^|\|\s*)st\s+(\S+)", r"\1st \2 --no-refresh", command)


def _refresh_after_edit(project: str, edit_file: str, probe: str, env: dict) -> dict:
    path = os.path.join(project, edit_file)
    with open(path, "rb") as handle:
        original = handle.read()
    try:
        with open(path, "ab") as handle:
            handle.write(f"\n{EDIT_MARKER} {time.time()}\n".encode())
        edit_s, edited = _timed(probe, project, env)
    finally:
        with open(path, "wb") as handle:
            handle.write(original)
    revert_s, reverted = _timed(probe, project, env)
    return {
        "edit_file": edit_file,
        "probe": probe,
        "after_edit_s": edit_s,
        "after_edit_st": parse_refreshed(edited.stderr),
        "after_revert_s": revert_s,
        "after_revert_st": parse_refreshed(reverted.stderr),
    }


def measure(project: str, questions: list[dict], edit_file: str | None, repeats: int, env: dict) -> dict:
    # One query with refresh first, so the timings below start from a current index.
    _timed("st find st_bench_probe", project, env)
    result = {
        "startup": _repeat("st status", project, env, repeats),
        "queries": [],
        "refresh": None,
    }
    for question in questions:
        reference = question.get("st_reference")
        if reference:
            timing = _repeat(with_no_refresh(reference), project, env, repeats)
            timing["question"] = question["id"]
            result["queries"].append(timing)
    if edit_file:
        result["refresh"] = _refresh_after_edit(project, edit_file, "st find st_bench_probe", env)
    return result
