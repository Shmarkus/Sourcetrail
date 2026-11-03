"""Arm files: everything that differs between the arms of a benchmark, one JSON file per arm in arms/ (see README.md)."""

from __future__ import annotations

import copy
import json
import os
import re
from dataclasses import dataclass, field, replace

ARMS_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "arms")
REPO = os.path.dirname(os.path.dirname(os.path.dirname(ARMS_DIR)))
_PLACEHOLDER = re.compile(r"\{([A-Za-z_][A-Za-z0-9_]*)\}")
_KEYS = {"name", "description", "workdir", "copy_name", "st", "path_prepend", "env", "vars", "tools", "allowed_tools",
         "mcp_servers", "settings", "note", "note_readme_section", "adoption", "warm_up", "startup",
         "startup_repeats", "shut_down", "version", "session_log"}


class ArmError(ValueError):
    pass


@dataclass
class ArmSpec:
    name: str
    directory: str
    tools: list
    allowed_tools: list
    description: str = ""
    workdir: str = "copy"            # "copy": a clean copy of the project per arm; "project": the project as given
    copy_name: str = ""
    st: str = "hidden"               # "hidden": a shadow st answers "command not found"; "on_path": the --st binary
    path_prepend: list = field(default_factory=list)
    env: dict = field(default_factory=dict)
    vars: dict = field(default_factory=dict)
    mcp_servers: dict | None = None
    settings: dict | None = None
    note: str | None = None
    note_readme_section: bool = False
    adoption: dict = field(default_factory=dict)
    warm_up: str | None = None
    startup: str | None = None
    startup_repeats: int = 3
    shut_down: str | None = None
    version: str | None = None
    # A file the tool appends "<epoch seconds> start ..." to each time it starts: counts starts inside each agent run.
    session_log: str | None = None


def _path(name_or_path: str, arms_dir: str) -> str:
    if name_or_path.endswith(".json") or os.sep in name_or_path:
        return os.path.abspath(name_or_path)
    return os.path.join(arms_dir, f"{name_or_path}.json")


def _note_path(note: str, directory: str) -> str:
    return note if os.path.isabs(note) else os.path.normpath(os.path.join(directory, note))


def load_arm(name_or_path: str, arms_dir: str = ARMS_DIR) -> ArmSpec:
    """An arm by name (arms/<name>.json next to bench.py) or by the path of its file."""
    path = _path(name_or_path, arms_dir)
    if not os.path.isfile(path):
        raise ArmError(f"no arm file for '{name_or_path}' ({path})")
    with open(path, encoding="utf-8") as handle:
        data = json.load(handle)
    unknown = sorted(set(data) - _KEYS)
    if unknown:
        raise ArmError(f"{path}: unknown keys {', '.join(unknown)}")
    if not data.get("name"):
        raise ArmError(f"{path}: no name")
    if data.get("workdir", "copy") not in ("copy", "project"):
        raise ArmError(f"{path}: workdir must be 'copy' or 'project'")
    if data.get("st", "hidden") not in ("hidden", "on_path"):
        raise ArmError(f"{path}: st must be 'hidden' or 'on_path'")
    if not data.get("tools"):
        raise ArmError(f"{path}: tools must list at least one tool")
    directory = os.path.dirname(path)
    fields = dict(data)
    fields.setdefault("allowed_tools", list(data["tools"]))
    fields.setdefault("copy_name", os.path.join(data["name"], "project-without-sourcetrail"))
    if data.get("note"):
        fields["note"] = _note_path(data["note"], directory)
    return ArmSpec(directory=directory, **fields)


def _substitute(text: str, values: dict, where: str) -> str:
    def one(match):
        key = match.group(1)
        if key not in values:
            raise ArmError(f"{where}: unknown placeholder {{{key}}}")
        return values[key]

    return _PLACEHOLDER.sub(one, text)


def _walk(value, values: dict, where: str):
    if isinstance(value, str):
        return _substitute(value, values, where)
    if isinstance(value, list):
        return [_walk(item, values, where) for item in value]
    if isinstance(value, dict):
        return {key: _walk(item, values, where) for key, item in value.items()}
    return value


def resolve(spec: ArmSpec, run_values: dict, environ: dict | None = None) -> ArmSpec:
    """The spec with its placeholders filled: run values ({work}, {workdir}, {project}, {st}, ...) first, then the
    environment, then the arm's own `vars` defaults. `{arms}` is the arm file's folder, `{repo}` the repository."""
    environ = os.environ if environ is None else environ
    values = {key: os.path.expanduser(environ.get(key, default)) for key, default in spec.vars.items()}
    values.update({"arms": spec.directory, "repo": REPO})
    values.update(run_values)
    resolved = replace(spec, **{key: _walk(copy.deepcopy(getattr(spec, key)), values, f"arm {spec.name}")
                                for key in ("path_prepend", "env", "mcp_servers", "settings", "warm_up", "startup",
                                            "shut_down", "version", "session_log")})
    return resolved
