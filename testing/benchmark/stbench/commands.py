"""Classifies the shell commands an agent ran: which programs, and how often `st`."""

from __future__ import annotations

import os
import re
import shlex

TEXT_TOOLS = {"grep", "egrep", "fgrep", "rg", "ag", "ack", "find", "fd", "sed", "awk", "cat", "head", "tail", "less", "nl"}
_ASSIGNMENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=")
_PREFIXES = {"time", "command", "exec", "nice", "env"}


def _segments(command: str) -> list[str]:
    """Splits a command line at |, ;, &, newlines and $( that are outside quotes."""
    segments: list[str] = []
    current: list[str] = []
    quote = ""
    index = 0
    while index < len(command):
        char = command[index]
        if quote:
            if char == quote:
                quote = ""
            elif char == "\\" and quote == '"' and index + 1 < len(command):
                current.append(char)
                index += 1
                char = command[index]
            current.append(char)
        elif char in "'\"":
            quote = char
            current.append(char)
        elif char in "|;&\n(`":
            segments.append("".join(current))
            current = []
        elif char == "$" and command[index + 1 : index + 2] == "(":
            segments.append("".join(current))
            current = []
            index += 1
        else:
            current.append(char)
        index += 1
    segments.append("".join(current))
    return segments


def _first_word(segment: str) -> str | None:
    try:
        words = shlex.split(segment)
    except ValueError:
        words = segment.split()
    for word in words:
        if _ASSIGNMENT.match(word) or word in _PREFIXES or word in "{}!":
            continue
        return os.path.basename(word.rstrip(")"))
    return None


def command_words(command: str) -> list[str]:
    """The program name of every simple command in a shell command line, in order."""
    words = []
    for segment in _segments(command):
        word = _first_word(segment)
        if word:
            words.append(word)
    return words


def st_invocations(command: str) -> int:
    return sum(1 for word in command_words(command) if word == "st")


def is_text_tool(word: str) -> bool:
    return word in TEXT_TOOLS
