"""Loads and checks a question file (JSON)."""

from __future__ import annotations

import json
import re

KINDS = {"structural", "stacktrace", "text"}
_NAMES_ST = re.compile(r"(?<![A-Za-z0-9_./-])st(?![A-Za-z0-9_-])")


class QuestionError(ValueError):
    pass


def _check(question: dict, seen: set) -> None:
    ident = question.get("id")
    if not ident:
        raise QuestionError(f"a question has no id: {question}")
    if ident in seen:
        raise QuestionError(f"duplicate question id {ident}")
    seen.add(ident)
    if question.get("kind") not in KINDS:
        raise QuestionError(f"{ident}: kind must be one of {sorted(KINDS)}")
    if not question.get("prompt", "").strip():
        raise QuestionError(f"{ident}: empty prompt")
    if _NAMES_ST.search(question["prompt"]):
        raise QuestionError(f"{ident}: the prompt names st; both arms must get the same neutral question")
    if not question.get("expected", {}).get("required"):
        raise QuestionError(f"{ident}: expected.required must list at least one item")


def load_questions(path: str, only: list[str] | None = None) -> dict:
    with open(path, encoding="utf-8") as handle:
        data = json.load(handle)
    seen: set = set()
    for question in data.get("questions", []):
        _check(question, seen)
    if only:
        unknown = [ident for ident in only if ident not in seen]
        if unknown:
            raise QuestionError(f"unknown question ids: {', '.join(unknown)}")
        data["questions"] = [q for q in data["questions"] if q["id"] in only]
    return data
