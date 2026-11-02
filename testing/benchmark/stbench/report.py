"""Aggregates run records per arm and renders the markdown report against the spec's success criteria."""

from __future__ import annotations

import re

from .speed import median

STRUCTURAL_KINDS = {"structural", "stacktrace"}
TARGETS = {
    "median_tokens": ("Tokens per question (median)", -0.50, "at least 50% fewer"),
    "median_context_growth": ("Context growth per question (median)", -0.50, "at least 50% fewer"),
    "median_tool_calls": ("Tool calls per question (median)", -0.50, "at least 50% fewer"),
    "median_duration_s": ("Time per question, start to answer (median)", -0.30, "at least 30% faster"),
}


def _arm_summary(runs: list[dict]) -> dict:
    ok = [run for run in runs if not run.get("is_error")]

    def values(key):
        return [run[key] for run in ok if run.get(key) is not None]

    return {
        "runs": len(ok),
        "errors": len(runs) - len(ok),
        "correct": sum(run["verdict"] == "correct" for run in ok),
        "partial": sum(run["verdict"] == "partial" for run in ok),
        "wrong": sum(run["verdict"] in ("wrong", "no answer") for run in ok),
        "median_tokens": median(values("total_tokens")),
        "median_context_growth": median(values("context_growth_tokens")),
        "median_output_tokens": median(values("output_tokens")),
        "median_tool_calls": median(values("tool_calls")),
        "median_duration_s": median(values("duration_s")),
        "total_tokens": sum(values("total_tokens")),
        "total_tool_calls": sum(values("tool_calls")),
        "total_tool_output_chars": sum(values("tool_output_chars")),
        "total_cost_usd": sum(values("cost_usd")),
    }


def _change(base: float | None, new: float | None) -> float | None:
    if not base or new is None:
        return None
    return (new - base) / base


def summarize(runs: list[dict]) -> dict:
    arms = {}
    for arm in dict.fromkeys(run["arm"] for run in runs):
        arms[arm] = _arm_summary([run for run in runs if run["arm"] == arm])
    st_runs = [run for run in runs if run["arm"] == "st" and not run.get("is_error")]
    adoption = {
        "structural_runs": sum(run["kind"] in STRUCTURAL_KINDS for run in st_runs),
        "structural_with_st": sum(run["kind"] in STRUCTURAL_KINDS and run["used_st"] for run in st_runs),
        "text_runs": sum(run["kind"] == "text" for run in st_runs),
        "text_with_st": sum(run["kind"] == "text" and run["used_st"] for run in st_runs),
    }
    change = {}
    if "grep" in arms and "st" in arms:
        change = {key: _change(arms["grep"][key], arms["st"][key])
                  for key in ("median_tokens", "median_context_growth", "median_tool_calls", "median_duration_s",
                              "total_tool_output_chars")}
    return {"arms": arms, "adoption": adoption, "change": change}


def _item_key(line: str) -> str:
    """`io.spring.P.process()  // note` and `P.process` are the same item: the last two dotted names, no parameters."""
    name = re.split(r"[\s(]", line.strip(), maxsplit=1)[0]
    return ".".join(name.split(".")[-2:])


def suspect_ground_truth(runs: list[dict]) -> list[dict]:
    """Items every scored run of a question (both arms, at least two runs) missed, or listed against the ground truth.

    Agents that all agree against the ground truth are more often right than the ground truth is.
    """
    suspects = []
    for question in dict.fromkeys(run["question"] for run in runs):
        scored = [run for run in runs if run["question"] == question and not run.get("is_error")]
        if len(scored) < 2:
            continue
        for what, field, key in (("missed by every run", "missing", str),
                                 ("listed by every run, not in the ground truth", "extras", _item_key),
                                 ("listed by every run although it is forbidden", "forbidden_hits", str)):
            per_run = [{key(item) for item in run.get(field) or []} for run in scored]
            for item in sorted(set.intersection(*per_run)):
                suspects.append({"question": question, "what": what, "item": item, "runs": len(scored)})
    return suspects


def _percent(value: float | None) -> str:
    if value is None:
        return "n/a"
    return "0%" if round(value * 100) == 0 else f"{value * 100:+.0f}%"


def _seconds(value: float | None) -> str:
    return "n/a" if value is None else f"{value:.1f} s"


def _number(value) -> str:
    if value is None:
        return "n/a"
    if isinstance(value, float) and not value.is_integer():
        return f"{value:,.1f}"
    return f"{int(value):,}"


def _met(change: float | None, target: float) -> str:
    if change is None:
        return "n/a"
    return "yes" if change <= target else "no"


def _criteria_table(summary: dict) -> list[str]:
    arms, change = summary["arms"], summary["change"]
    grep, st = arms.get("grep", {}), arms.get("st", {})
    lines = ["| Metric | grep | st | Change | Target | Met |", "|---|---|---|---|---|---|"]
    lines.append(f"| Correct runs | {grep.get('correct', 0)} / {grep.get('runs', 0)} | "
                 f"{st.get('correct', 0)} / {st.get('runs', 0)} | | no worse than grep | "
                 f"{'yes' if st.get('correct', 0) >= grep.get('correct', 0) else 'no'} |")
    lines.append(f"| Partial / wrong runs | {grep.get('partial', 0)} / {grep.get('wrong', 0)} | "
                 f"{st.get('partial', 0)} / {st.get('wrong', 0)} | | | |")
    for key, (label, target, wording) in TARGETS.items():
        shown = _seconds if key.endswith("_s") else _number
        lines.append(f"| {label} | {shown(grep.get(key))} | {shown(st.get(key))} | "
                     f"{_percent(change.get(key))} | {wording} | {_met(change.get(key), target)} |")
    lines.append(f"| Tool output characters (total) | {_number(grep.get('total_tool_output_chars'))} | "
                 f"{_number(st.get('total_tool_output_chars'))} | {_percent(change.get('total_tool_output_chars'))} | | |")
    lines.append(f"| Cost, USD (total, as the CLI reports it) | {grep.get('total_cost_usd', 0):.2f} | "
                 f"{st.get('total_cost_usd', 0):.2f} | | | |")
    lines.append(f"| Failed runs (left out above) | {grep.get('errors', 0)} | {st.get('errors', 0)} | | 0 | |")
    return lines


def _adoption_lines(adoption: dict) -> list[str]:
    structural, used = adoption["structural_runs"], adoption["structural_with_st"]
    share = f"{used / structural * 100:.0f}%" if structural else "n/a"
    met = "n/a" if not structural else ("yes" if used / structural >= 0.8 else "no")
    return [
        f"- `st` used on {used} of {structural} structural and stack-trace runs ({share}; target at least 80%: {met}).",
        f"- `st` used on {adoption['text_with_st']} of {adoption['text_runs']} plain-text runs "
        "(grep is the right tool there; a few cross-checks are fine).",
    ]


_MARKS = {"correct": "✓", "partial": "½", "wrong": "✗", "no answer": "∅"}


def _cells(runs: list[dict]) -> list[str]:
    """Result marks, median tool calls and median tokens of one arm's repeats of one question."""
    if not runs:
        return ["", "", ""]
    marks = "".join("E" if run.get("is_error") else _MARKS.get(run["verdict"], "?") for run in runs)
    ok = [run for run in runs if not run.get("is_error")]
    return [marks, _number(median([run["tool_calls"] for run in ok])),
            _number(median([run["total_tokens"] for run in ok]))]


def _question_table(runs: list[dict]) -> list[str]:
    lines = ["| Question | Kind | grep result | grep calls | grep tokens | st result | st calls | st tokens | st used |",
             "|---|---|---|---|---|---|---|---|---|"]
    for question in dict.fromkeys(run["question"] for run in runs):
        mine = [run for run in runs if run["question"] == question]
        st = [run for run in mine if run["arm"] == "st"]
        used = f"{sum(run['used_st'] for run in st)} / {len(st)}" if st else ""
        cells = [question, mine[0]["kind"]] + _cells([run for run in mine if run["arm"] == "grep"]) + _cells(st) + [used]
        lines.append("| " + " | ".join(cells) + " |")
    return lines


def _speed_lines(speed: dict) -> list[str]:
    lines = ["| Measure | Median | Target |", "|---|---|---|"]
    startup = speed.get("startup")
    if startup:
        lines.append(f"| Startup, `st status` | {startup['median_s'] * 1000:.0f} ms | under 200 ms |")
    queries = speed.get("queries", [])
    if queries:
        slowest = max(queries, key=lambda timing: timing["median_s"])
        lines.append(f"| Query, median over questions | {median([q['median_s'] for q in queries]):.2f} s | under 1 s |")
        lines.append(f"| Query, slowest ({slowest['question']}) | {slowest['median_s']:.2f} s | under 1 s (path under 2 s) |")
    refresh = speed.get("refresh")
    if refresh:
        if refresh.get("valid"):
            shown = (f"{refresh['median_s']:.1f} s ({refresh['min_s']:.1f}–{refresh['max_s']:.1f} s, "
                     f"{refresh['valid']} of {len(refresh['attempts'])} refreshed)")
        else:
            shown = f"no refresh in {len(refresh.get('attempts', []))} attempts: check the build"
        lines.append(f"| First query after a one-line edit | {shown} | under 5 s |")
    return lines


_SETUP_LABELS = [("date", "Date"), ("questions", "Questions"), ("project_commit", "Project commit"),
                 ("sourcetrail_commit", "Sourcetrail commit"), ("claude_version", "Claude Code"), ("model", "Model"),
                 ("repeats", "Repeats per question and arm"), ("arms", "Arms")]


def _setup_lines(meta: dict) -> list[str]:
    return [f"- {label}: {meta[key]}" for key, label in _SETUP_LABELS if meta.get(key) not in (None, "")]


def render_markdown(summary: dict, runs: list[dict], speed: dict | None, title: str = "st benchmark",
                    meta: dict | None = None) -> str:
    parts = [f"# {title}", ""]
    if meta:
        parts += _setup_lines(meta) + [""]
    parts += ["## Against the success criteria", ""]
    parts += _criteria_table(summary)
    parts += ["", "## Adoption", ""] + _adoption_lines(summary["adoption"])
    parts += ["", "## Per question", "", "✓ correct, ½ partial, ✗ wrong, ∅ no answer block, E failed run; "
              "calls and tokens are medians over repeats.", ""]
    parts += _question_table(runs)
    suspects = suspect_ground_truth(runs)
    if suspects:
        parts += ["", "## Check the ground truth", "",
                  "Every scored run agrees against the question file here. Check the code; if the runs are right, "
                  "fix the question and run `bench.py rescore`.", ""]
        parts += [f"- {s['question']}: `{s['item']}` {s['what']} ({s['runs']} runs)" for s in suspects]
    if speed:
        parts += ["", "## st speed", ""] + _speed_lines(speed)
    return "\n".join(parts) + "\n"
