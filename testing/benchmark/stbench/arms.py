"""What differs between the arms: the PATH, the project copy and the project note. The prompt is shared."""

from __future__ import annotations

import os
import shutil

ST_SECTION_HEADING = "## `st`: code navigation for agents"
_IGNORED_DIRS = {"sourcetrail_dependencies", "build", ".gradle", "target", ".idea", "out"}
_IGNORED_SUFFIXES = (".srctrlprj", ".srctrldb", ".srctrlbm", ".srctrlst", ".srctrldb_tmp")

PROMPT = """You are working in a Java codebase in the current directory. Answer the question below.
Do not modify any files.

Question: {prompt}

End your reply with the final answer as a fenced code block whose info string is `answer`, one item per line,
listing {answer_items}. List every item that answers the question and nothing else: no explanations inside the block.

```answer
...
```
"""


def build_prompt(question: dict) -> str:
    return PROMPT.format(prompt=question["prompt"].strip(),
                         answer_items=question.get("answer_items", "one item per line"))


_SHADOW = "#!/bin/sh\necho \"sh: st: command not found\" >&2\nexit 127\n"


def path_hiding_st(path: str, shadow_dir: str) -> str:
    """PATH with a directory in front whose `st` answers "command not found"; every other tool stays reachable."""
    os.makedirs(shadow_dir, exist_ok=True)
    shadow = os.path.join(shadow_dir, "st")
    with open(shadow, "w") as handle:
        handle.write(_SHADOW)
    os.chmod(shadow, 0o755)
    return os.pathsep.join([shadow_dir, path])


def path_with_st(path: str, binary: str, link_dir: str) -> str:
    """PATH with a directory in front whose `st` is the given binary."""
    os.makedirs(link_dir, exist_ok=True)
    link = os.path.join(link_dir, "st")
    if os.path.lexists(link):
        os.remove(link)
    os.symlink(os.path.abspath(binary), link)
    return os.pathsep.join([link_dir, path])


def readme_agent_section(readme: str) -> str:
    """The `st` section of the Sourcetrail README, up to the next level-two heading."""
    start = readme.find(ST_SECTION_HEADING)
    if start < 0:
        raise ValueError(f"README has no '{ST_SECTION_HEADING}' section")
    end = readme.find("\n## ", start + len(ST_SECTION_HEADING))
    return readme[start:] if end < 0 else readme[start:end + 1]


def _ignore(directory: str, names: list[str]) -> set[str]:
    return {name for name in names
            if name in _IGNORED_DIRS or name.endswith(_IGNORED_SUFFIXES)}


def copy_without_sourcetrail(source: str, target: str) -> None:
    """A copy of the project as it would be without Sourcetrail: no index, delombok copies or build output."""
    shutil.copytree(source, target, ignore=_ignore, symlinks=True)


def st_note(snippet: str, readme_copy: str) -> str:
    """The CLAUDE.md snippet with its README placeholder pointing at a copy of the README's `st` section."""
    return snippet.replace("<path to Sourcetrail README.md>", readme_copy)


def claude_args(prompt: str, model: str | None, note: str | None, bare: bool, max_turns: int) -> list[str]:
    args = ["claude", "-p", prompt, "--output-format", "stream-json", "--verbose",
            "--tools", "Bash,Read,Grep,Glob", "--dangerously-skip-permissions",
            "--no-session-persistence", "--strict-mcp-config", "--max-turns", str(max_turns)]
    if model:
        args += ["--model", model]
    if bare:
        args.append("--bare")
    if note:
        args += ["--append-system-prompt", note]
    return args
