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


def _edit_and_probe(path: str, original: bytes, probe: str, project: str, env: dict) -> dict:
    try:
        with open(path, "ab") as handle:
            handle.write(f"\n{EDIT_MARKER} {time.time()}\n".encode())
        seconds, done = _timed(probe, project, env)
    finally:
        with open(path, "wb") as handle:
            handle.write(original)
    refreshed = parse_refreshed(done.stderr)
    _timed(probe, project, env)  # refresh back to the original before the next attempt
    return {"seconds": seconds, "exit_code": done.returncode, "refreshed": refreshed,
            "valid": refreshed is not None and done.returncode in (0, 1)}


def measure_refresh(project: str, edit_file: str, probe: str, env: dict, repeats: int) -> dict:
    """First query after a one-line edit, `repeats` times; an attempt counts only if st reported a refresh."""
    path = os.path.join(project, edit_file)
    with open(path, "rb") as handle:
        original = handle.read()
    attempts = [_edit_and_probe(path, original, probe, project, env) for _ in range(repeats)]
    valid = [attempt["seconds"] for attempt in attempts if attempt["valid"]]
    return {"edit_file": edit_file, "probe": probe, "attempts": attempts, "valid": len(valid),
            "median_s": median(valid), "min_s": min(valid) if valid else None, "max_s": max(valid) if valid else None}


def measure(project: str, questions: list[dict], edit_file: str | None, repeats: int, env: dict,
            refresh_repeats: int = 3) -> dict:
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
        result["refresh"] = measure_refresh(project, edit_file, "st find st_bench_probe", env, refresh_repeats)
    return result
