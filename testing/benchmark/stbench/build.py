"""The st build a benchmark runs against: what refresh needs next to st, and whether the build changed."""

from __future__ import annotations

import os

NEEDED_FOR_REFRESH = ["Sourcetrail", "sourcetrail_indexer"]
_FINGERPRINTED = ["st", "Sourcetrail", "sourcetrail_indexer", "Sourcetrail_cli"]


def _directory(st_binary: str) -> str:
    return os.path.dirname(os.path.realpath(st_binary))


def missing_neighbours(st_binary: str) -> list[str]:
    """Files st needs beside it to refresh the index; without them a refresh fails and queries go stale."""
    directory = _directory(st_binary)
    return [name for name in NEEDED_FOR_REFRESH if not os.path.exists(os.path.join(directory, name))]


def build_fingerprint(st_binary: str) -> dict:
    """Size and modification time of st and the binaries beside it."""
    directory = _directory(st_binary)
    fingerprint = {}
    for name in _FINGERPRINTED:
        path = os.path.join(directory, name)
        if name == "st":
            path = os.path.realpath(st_binary)
        if os.path.exists(path):
            info = os.stat(path)
            fingerprint[name] = [info.st_size, info.st_mtime_ns]
    return fingerprint


def changed_files(before: dict, after: dict) -> list[str]:
    return sorted(name for name in set(before) | set(after) if before.get(name) != after.get(name))
