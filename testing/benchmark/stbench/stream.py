"""Turns a `claude -p --output-format stream-json --verbose` log into per-run metrics."""

from __future__ import annotations

import json
from typing import Iterable

from .commands import command_words, is_text_tool, st_invocations

_TEXT_TOOL_NAMES = {"Read", "Grep", "Glob"}


def _result_length(content) -> int:
    if isinstance(content, str):
        return len(content)
    if isinstance(content, list):
        return sum(len(block.get("text", "")) for block in content if isinstance(block, dict))
    return 0


def _empty_metrics() -> dict:
    return {
        "tool_calls": 0, "st_calls": 0, "st_invocations": 0, "text_tool_calls": 0, "used_st": False,
        "tool_output_chars": 0, "commands": [],
        "input_tokens": 0, "cache_creation_tokens": 0, "cache_read_tokens": 0, "output_tokens": 0,
        "total_tokens": 0, "cost_usd": 0.0, "duration_s": 0.0, "turns": 0,
        "final_text": "", "is_error": True, "error": "no result message (the run was cut off)",
        "api_calls": 0, "initial_context_tokens": None, "final_context_tokens": None, "context_growth_tokens": None,
    }


def _context_size(usage: dict) -> int:
    return sum(usage.get(key, 0) or 0
               for key in ("input_tokens", "cache_creation_input_tokens", "cache_read_input_tokens"))


def _apply_context(metrics: dict, calls: list) -> None:
    """Context growth: the last call's context plus its output, minus the first call's context."""
    if not calls:
        return
    metrics["api_calls"] = len(calls)
    metrics["initial_context_tokens"] = _context_size(calls[0])
    metrics["final_context_tokens"] = _context_size(calls[-1]) + (calls[-1].get("output_tokens", 0) or 0)
    metrics["context_growth_tokens"] = metrics["final_context_tokens"] - metrics["initial_context_tokens"]


def _count_tool_use(metrics: dict, block: dict) -> None:
    metrics["tool_calls"] += 1
    name = block.get("name", "")
    if name in _TEXT_TOOL_NAMES:
        metrics["text_tool_calls"] += 1
        return
    if name != "Bash":
        return
    command = block.get("input", {}).get("command", "")
    metrics["commands"].append(command)
    invocations = st_invocations(command)
    if invocations:
        metrics["st_calls"] += 1
        metrics["st_invocations"] += invocations
        metrics["used_st"] = True
    if any(is_text_tool(word) for word in command_words(command)):
        metrics["text_tool_calls"] += 1


def _apply_result(metrics: dict, message: dict) -> None:
    usage = message.get("usage") or {}
    metrics["input_tokens"] = usage.get("input_tokens", 0) or 0
    metrics["cache_creation_tokens"] = usage.get("cache_creation_input_tokens", 0) or 0
    metrics["cache_read_tokens"] = usage.get("cache_read_input_tokens", 0) or 0
    metrics["output_tokens"] = usage.get("output_tokens", 0) or 0
    metrics["total_tokens"] = (metrics["input_tokens"] + metrics["cache_creation_tokens"]
                               + metrics["cache_read_tokens"] + metrics["output_tokens"])
    metrics["cost_usd"] = float(message.get("total_cost_usd") or 0.0)
    metrics["duration_s"] = (message.get("duration_ms") or 0) / 1000.0
    metrics["turns"] = message.get("num_turns", 0) or 0
    metrics["final_text"] = message.get("result") or metrics["final_text"]
    metrics["is_error"] = bool(message.get("is_error"))
    if metrics["is_error"]:
        reason = message.get("terminal_reason") or message.get("subtype") or "error"
        metrics["error"] = f"{reason}: {message.get('result', '')}".strip()
    else:
        metrics["error"] = ""


def parse_stream(lines: Iterable[str]) -> dict:
    metrics = _empty_metrics()
    calls, call_ids = [], set()  # one usage per API call; a call can arrive as several messages with one id
    for raw in lines:
        try:
            message = json.loads(raw)
        except (json.JSONDecodeError, TypeError):
            continue
        kind = message.get("type")
        content = (message.get("message") or {}).get("content") or []
        if kind == "assistant":
            body = message.get("message") or {}
            usage = body.get("usage")
            call_id = body.get("id") or f"message-{len(calls)}"
            if usage and _context_size(usage) and call_id not in call_ids:
                call_ids.add(call_id)
                calls.append(usage)
            for block in content:
                if block.get("type") == "tool_use":
                    _count_tool_use(metrics, block)
                elif block.get("type") == "text":
                    metrics["final_text"] = block.get("text", "")
        elif kind == "user" and isinstance(content, list):
            for block in content:
                if isinstance(block, dict) and block.get("type") == "tool_result":
                    metrics["tool_output_chars"] += _result_length(block.get("content"))
        elif kind == "result":
            _apply_result(metrics, message)
    _apply_context(metrics, calls)
    return metrics
