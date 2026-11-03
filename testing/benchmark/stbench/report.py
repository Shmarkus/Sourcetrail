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
        "permission_denials": sum(run.get("permission_denials") or 0 for run in runs),
        "leak_runs": sum(bool(run.get("leaks")) for run in runs),
    }


def _change(base: float | None, new: float | None) -> float | None:
    if not base or new is None:
        return None
    return (new - base) / base


def arm_order(runs: list[dict], given: str | None = None) -> list[str]:
    """The arms in the order the run was given (meta.json), else grep first and the rest as they first ran."""
    seen = list(dict.fromkeys(run["arm"] for run in runs))
    order = [arm.strip() for arm in (given or "").split(",") if arm.strip() in seen]
    if not order:
        order = sorted(seen, key=lambda arm: arm != "grep")
    return order + [arm for arm in seen if arm not in order]


def summarize(runs: list[dict], order: list[str] | None = None) -> dict:
    arms = {}
    for arm in order or arm_order(runs):
        arms[arm] = _arm_summary([run for run in runs if run["arm"] == arm])
    st_runs = [run for run in runs if run["arm"] == "st" and not run.get("is_error")]
    adoption = {
        "structural_runs": sum(run["kind"] in STRUCTURAL_KINDS for run in st_runs),
        "structural_with_st": sum(run["kind"] in STRUCTURAL_KINDS and run["used_st"] for run in st_runs),
        "text_runs": sum(run["kind"] == "text" for run in st_runs),
        "text_with_st": sum(run["kind"] == "text" and run["used_st"] for run in st_runs),
    }
    change_by_arm = {}
    if "grep" in arms:
        change_by_arm = {arm: {key: _change(arms["grep"][key], arms[arm][key]) for key in _CHANGED}
                         for arm in arms if arm != "grep"}
    return {"arms": arms, "adoption": adoption, "change": change_by_arm.get("st", {}) if "st" in arms else {},
            "change_by_arm": change_by_arm,
            "adoption_by_arm": {arm: counts for arm, counts in sorted(_adoption_by_arm(runs).items(),
                                                                       key=lambda item: list(arms).index(item[0]))}}


_CHANGED = ("median_tokens", "median_context_growth", "median_tool_calls", "median_duration_s", "total_tool_output_chars")


def _used_tool(run: dict):
    """Whether the run used its arm's own tool; None for an arm without one. Old records only know `used_st`."""
    if "used_arm_tool" in run:
        return run["used_arm_tool"]
    return run.get("used_st") if run["arm"] == "st" else None


def _adoption_by_arm(runs: list[dict]) -> dict:
    adoption = {}
    for arm in dict.fromkeys(run["arm"] for run in runs):
        mine = [run for run in runs if run["arm"] == arm and not run.get("is_error")]
        if not any(_used_tool(run) is not None for run in mine):
            continue
        calls: dict = {}
        for run in mine:
            for name, count in (run.get("tool_names") or {}).items():
                calls[name] = calls.get(name, 0) + count
        adoption[arm] = {
            "structural_runs": sum(run["kind"] in STRUCTURAL_KINDS for run in mine),
            "structural_with_tool": sum(run["kind"] in STRUCTURAL_KINDS and bool(_used_tool(run)) for run in mine),
            "text_runs": sum(run["kind"] == "text" for run in mine),
            "text_with_tool": sum(run["kind"] == "text" and bool(_used_tool(run)) for run in mine),
            "calls_by_tool": dict(sorted(calls.items(), key=lambda item: (-item[1], item[0]))),
        }
    return adoption


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
    lines.append(f"| Permission denials (total) | {grep.get('permission_denials', 0)} | "
                 f"{st.get('permission_denials', 0)} | | 0 | |")
    lines.append(f"| Runs that touched question or result files | {grep.get('leak_runs', 0)} | "
                 f"{st.get('leak_runs', 0)} | | 0 | |")
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


def _with_change(shown: str, change: float | None) -> str:
    return shown if change is None else f"{shown} ({_percent(change)})"


def _multi_criteria_table(summary: dict) -> list[str]:
    """One column per arm; every arm but grep shows its change against grep next to the value."""
    arms, changes = summary["arms"], summary["change_by_arm"]
    names = list(arms)
    lines = ["| Metric | " + " | ".join(names) + " | Target |", "|---|" + "---|" * (len(names) + 1)]

    def row(label, cells, target=""):
        lines.append(f"| {label} | " + " | ".join(cells) + f" | {target} |")

    row("Correct runs", [f"{arms[a]['correct']} / {arms[a]['runs']}" for a in names], "no worse than grep")
    row("Partial / wrong runs", [f"{arms[a]['partial']} / {arms[a]['wrong']}" for a in names])
    for key, (label, _target, wording) in TARGETS.items():
        shown = _seconds if key.endswith("_s") else _number
        row(label, [_with_change(shown(arms[a].get(key)), changes.get(a, {}).get(key)) for a in names], wording)
    row("Tool output characters (total)",
        [_with_change(_number(arms[a]["total_tool_output_chars"]), changes.get(a, {}).get("total_tool_output_chars"))
         for a in names])
    row("Cost, USD (total, as the CLI reports it)", [f"{arms[a]['total_cost_usd']:.2f}" for a in names])
    row("Failed runs (left out above)", [str(arms[a]["errors"]) for a in names], "0")
    row("Permission denials (total)", [str(arms[a]["permission_denials"]) for a in names], "0")
    row("Runs that touched question or result files", [str(arms[a]["leak_runs"]) for a in names], "0")
    return lines


def _multi_adoption_lines(adoption: dict) -> list[str]:
    lines = []
    for arm, counts in adoption.items():
        structural, used = counts["structural_runs"], counts["structural_with_tool"]
        share = f"{used / structural * 100:.0f}%" if structural else "n/a"
        met = "n/a" if not structural else ("yes" if used / structural >= 0.8 else "no")
        calls = ", ".join(f"{name} {count}" for name, count in counts["calls_by_tool"].items())
        lines.append(f"- `{arm}`: its tool used on {used} of {structural} structural and stack-trace runs ({share}; "
                     f"target at least 80%: {met}), on {counts['text_with_tool']} of {counts['text_runs']} text runs. "
                     f"Calls by tool: {calls or 'none'}.")
    return lines


def _multi_question_table(runs: list[dict], names: list[str]) -> list[str]:
    lines = ["| Question | Kind | " + " | ".join(names) + " |", "|---|---|" + "---|" * len(names)]
    for question in dict.fromkeys(run["question"] for run in runs):
        mine = [run for run in runs if run["question"] == question]
        cells = []
        for arm in names:
            marks, calls, tokens = _cells([run for run in mine if run["arm"] == arm])
            cells.append(f"{marks} {calls} / {tokens}" if marks else "")
        lines.append(f"| {question} | {mine[0]['kind']} | " + " | ".join(cells) + " |")
    return lines


def _warm_up_lines(entries: list[dict]) -> list[str]:
    lines = ["| Arm | Warm-up (once per run) | Wall time of the warm-up command | Tool's own measures "
             "| Per-session startup (paid inside every run) |", "|---|---|---|---|---|"]
    for arm in dict.fromkeys(entry["arm"] for entry in entries):
        mine = [entry for entry in entries if entry["arm"] == arm]
        warm = [entry for entry in mine if entry["phase"] == "warm_up"]
        startup = [entry for entry in mine if entry["phase"] == "startup"]
        if not warm and not startup:
            continue
        if warm:
            last = warm[-1]
            details = last.get("details", {})
            # A tool that knows when it was ready (the IDE's last dumb-mode exit) reports that as warm_up_s; the
            # command's wall time also holds its own checks and waits.
            wall = f"{last['seconds']:.1f} s"
            shown = (f"{details.get('warm_up_s', last['seconds']):.1f} s" if not last["exit"]
                     else f"failed (exit {last['exit']})")
            own = ", ".join(f"{key} {value}" for key, value in details.items()
                            if key.endswith("_s") and key != "warm_up_s" and isinstance(value, (int, float)))
        else:
            shown, wall, own = "none", "", ""
        # A probe that also waits for its tool to exit reports the startup itself as startup_s.
        good = [entry.get("details", {}).get("startup_s", entry["seconds"]) for entry in startup if not entry["exit"]]
        if good:
            start = f"{median(good):.1f} s ({min(good):.1f}–{max(good):.1f} s, {len(good)} runs)"
        else:
            start = "none" if not startup else "failed"
        lines.append(f"| {arm} | {shown} | {wall} | {own} | {start} |")
    return lines


def _session_start_lines(runs: list[dict], names: list[str]) -> list[str]:
    """Question time with and without the tool starting inside the run (jdtls starts with the first LSP call)."""
    lines = []
    for arm in names:
        mine = [run for run in runs if run["arm"] == arm and not run.get("is_error")
                and run.get("tool_started_in_session") is not None]
        if not mine:
            continue
        started = [run["duration_s"] for run in mine if run["tool_started_in_session"]]
        cold = [run["duration_s"] for run in mine if not run["tool_started_in_session"]]
        lines.append(f"- `{arm}`: median question time {_seconds(median(started))} in {len(started)} runs that "
                     f"started the tool, {_seconds(median(cold))} in {len(cold)} runs that did not.")
    return lines


def _leak_lines(runs: list[dict]) -> list[str]:
    return [f"- {run['question']} {run['arm']} #{run.get('repeat', '?')}: "
            + ", ".join(f"`{marker}`" for marker in run["leaks"]) for run in runs if run.get("leaks")]


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
                 ("models_seen", "Models seen in the runs"), ("repeats", "Repeats per question and arm"),
                 ("arms", "Arms")]


def _setup_lines(meta: dict) -> list[str]:
    lines = [f"- {label}: {meta[key]}" for key, label in _SETUP_LABELS if meta.get(key) not in (None, "")]
    lines += [f"- Version, {arm}: {version}" for arm, version in (meta.get("tool_versions") or {}).items()]
    return lines


def render_markdown(summary: dict, runs: list[dict], speed: dict | None, title: str = "st benchmark",
                    meta: dict | None = None, warm_up: list[dict] | None = None) -> str:
    parts = [f"# {title}", ""]
    if meta:
        parts += _setup_lines(meta) + [""]
    if meta and meta.get("arms"):
        order = arm_order(runs, meta["arms"])
        if order != list(summary["arms"]):
            summary = summarize(runs, order)
    multi = bool(set(summary["arms"]) - {"grep", "st"}) or len(summary["arms"]) > 2
    parts += ["## Against the success criteria", ""]
    parts += _multi_criteria_table(summary) if multi else _criteria_table(summary)
    if multi:
        parts += ["", "## Adoption", ""] + _multi_adoption_lines(summary["adoption_by_arm"])
    else:
        parts += ["", "## Adoption", ""] + _adoption_lines(summary["adoption"])
    if warm_up:
        parts += ["", "## Warm-up and per-session startup", "",
                  "Not part of any time above. Warm-up runs once before the questions (IDE import and indexing, "
                  "jdtls import and build, starting the st server); per-session startup is measured apart, and an "
                  "arm whose tool starts with every agent session pays it inside each question's time.", ""]
        parts += _warm_up_lines(warm_up)
        starts = _session_start_lines(runs, list(summary["arms"]))
        if starts:
            parts += [""] + starts
    parts += ["", "## Per question", "", "✓ correct, ½ partial, ✗ wrong, ∅ no answer block, E failed run; "
              "calls and tokens are medians over repeats.", ""]
    parts += _multi_question_table(runs, list(summary["arms"])) if multi else _question_table(runs)
    leaked = _leak_lines(runs)
    if leaked:
        parts += ["", "## Runs that touched question or result files", "",
                  "Their commands or paths named a question file, a question or notes folder, or the results "
                  "folder. Check the log; such a run may have seen the answer.", ""] + leaked
    suspects = suspect_ground_truth(runs)
    if suspects:
        parts += ["", "## Check the ground truth", "",
                  "Every scored run agrees against the question file here. Check the code; if the runs are right, "
                  "fix the question and run `bench.py rescore`.", ""]
        parts += [f"- {s['question']}: `{s['item']}` {s['what']} ({s['runs']} runs)" for s in suspects]
    if speed:
        parts += ["", "## st speed", ""] + _speed_lines(speed)
    return "\n".join(parts) + "\n"
