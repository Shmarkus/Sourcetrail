"""Reads an agent's ```answer block and scores it against a question's ground truth."""

from __future__ import annotations

import re

_BLOCK = re.compile(r"```[ \t]*answer[ \t]*\n(.*?)```", re.DOTALL)
_BULLET = re.compile(r"^(?:[-*+]\s+|\d+[.)]\s+)")
_IDENTIFIER = "A-Za-z0-9_$"


def _normalize(line: str) -> str:
    line = _BULLET.sub("", line.strip())
    line = line.replace("`", "").replace("#", ".").replace("::", ".")
    return line.strip().rstrip(",;")


def extract_answer(text: str | None) -> list[str] | None:
    """Lines of the last ```answer block, cleaned of bullets and backticks; None if there is no block."""
    if not text:
        return None
    blocks = _BLOCK.findall(text)
    if not blocks:
        return None
    lines = [_normalize(line) for line in blocks[-1].splitlines()]
    return [line for line in lines if line]


def _matches(item: str, line: str) -> bool:
    item = _normalize(item)
    pattern = rf"(?<![{_IDENTIFIER}]){re.escape(item)}(?![{_IDENTIFIER}])"
    return re.search(pattern, line) is not None


def _alternatives(item) -> list[str]:
    return list(item) if isinstance(item, (list, tuple)) else [item]


def _label(item) -> str:
    return " | ".join(_alternatives(item))


def score(answer: list[str] | None, expected: dict) -> dict:
    """Verdict: correct (every required item, no forbidden, no extras unless allowed), partial, wrong or no answer.

    `optional` items may appear without counting as extras (an interface hop on a path, a generated constructor).
    """
    required = expected.get("required", [])
    forbidden = expected.get("forbidden", [])
    allow_extra = expected.get("allow_extra", False)
    if answer is None:
        return {"verdict": "no answer", "recall": 0.0, "missing": [_label(i) for i in required],
                "forbidden_hits": [], "extras": []}

    found, missing, used_lines = [], [], set()
    for item in required:
        hits = [index for index, line in enumerate(answer)
                if any(_matches(alt, line) for alt in _alternatives(item))]
        (found if hits else missing).append(_label(item))
        used_lines.update(hits)

    for item in expected.get("optional", []):
        used_lines.update(index for index, line in enumerate(answer)
                          if any(_matches(alt, line) for alt in _alternatives(item)))

    forbidden_hits = [_label(item) for item in forbidden
                      if any(_matches(alt, line) for alt in _alternatives(item) for line in answer)]
    extras = [] if allow_extra else [line for index, line in enumerate(answer) if index not in used_lines]
    recall = len(found) / len(required) if required else 1.0

    if recall == 1.0 and not forbidden_hits and not extras:
        verdict = "correct"
    elif found:
        verdict = "partial"
    else:
        verdict = "wrong"
    return {"verdict": verdict, "recall": recall, "missing": missing, "forbidden_hits": forbidden_hits, "extras": extras}
