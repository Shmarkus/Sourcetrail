"""Scores stored answers again, after a question's ground truth was corrected."""

from __future__ import annotations

from .scoring import score


def rescore(records: list[dict], questions: dict) -> list[dict]:
    """New records with verdicts from each stored answer; failed runs and unknown questions stay as they were."""
    updated = []
    for record in records:
        question = questions.get(record["question"])
        if question is None or record.get("is_error"):
            updated.append(dict(record))
            continue
        updated.append(dict(record, **score(record.get("answer"), question["expected"])))
    return updated
