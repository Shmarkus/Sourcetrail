#!/usr/bin/env python3
"""IntelliJ IDEA's headless MCP server for the intellij arm: start it on a project copy and wait until it is ready, or
stop it, or print the IDE build.

    intellij-mcp.py start   --idea IDEA_SH --home DIR --project DIR --port N --tools a,b,c [--java-home JDK]
    intellij-mcp.py stop    --home DIR
    intellij-mcp.py version --idea IDEA_SH

The server is a second IDE process (`idea.sh mcpServer`) with its own config, system and log folders under --home, so
it never touches an IDE the user has open. --java-home is the JDK the project's Gradle build runs with. Ready means:
the MCP server is up, the Gradle import is done, and the project has left dumb mode (indexing) with no new dumb mode
for --quiet seconds. "MCP Server started" alone is printed before the Gradle import even begins. The last output line
is JSON: ready_s is the IDE's uptime at the last "exit dumb mode", the warm-up time to report.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import subprocess
import sys
import time
import urllib.request

# idea.log lines: "2026-11-03 16:12:01,123 [  87012]   INFO - #c.i.o.p.DumbServiceImpl - exit dumb mode [realworld]"
_UPTIME = re.compile(r"^\S+ \S+ \[\s*(\d+)\]")
_ENTER_DUMB = re.compile(r"enter dumb mode")
_EXIT_DUMB = re.compile(r"exit dumb mode")
# ExternalSystemUtil - External project [/abs/project] resolution task started / ... executed in 5424 ms.
_IMPORT_STARTED = re.compile(r"ExternalSystemUtil - External project .* resolution task started")
_IMPORT_DONE = re.compile(r"ExternalSystemUtil - External project .* resolution task executed")


def _write(path: str, text: str) -> None:
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)


def _vm_options(idea: str) -> str:
    bin_dir = os.path.dirname(os.path.realpath(idea))
    with open(os.path.join(bin_dir, "idea64.vmoptions"), encoding="utf-8") as handle:
        lines = [line.rstrip("\n") for line in handle if not line.startswith("-Xmx")]
    # project.open.type: a root that several importers claim fails to open headless without it.
    return "\n".join(lines + ["-Xmx4096m", "-Djava.awt.headless=true", "-Dproject.open.type=Gradle"]) + "\n"


def _log_state(log: str) -> dict:
    state = {"dumb": None, "last_exit_ms": None, "imports_started": 0, "imports_done": 0, "lines": 0}
    if not os.path.isfile(log):
        return state
    with open(log, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            state["lines"] += 1
            if _ENTER_DUMB.search(line):
                state["dumb"] = True
            elif _EXIT_DUMB.search(line):
                state["dumb"] = False
                uptime = _UPTIME.match(line)
                if uptime:
                    state["last_exit_ms"] = int(uptime.group(1))
            if _IMPORT_STARTED.search(line):
                state["imports_started"] += 1
            if _IMPORT_DONE.search(line):
                state["imports_done"] += 1
    return state


def _mcp_tools(port: int) -> list[str]:
    """tools/list over the server's SSE transport: requests are POSTed, answers arrive as events on the stream."""
    base = f"http://127.0.0.1:{port}"
    stream = urllib.request.urlopen(base + "/sse", timeout=60)

    def next_event() -> tuple[str | None, str]:
        kind, data = None, []
        for raw in stream:
            line = raw.decode().rstrip("\r\n")
            if line.startswith("event:"):
                kind = line[6:].strip()
            elif line.startswith("data:"):
                data.append(line[5:].strip())
            elif not line and data:
                return kind, "\n".join(data)
        raise ConnectionError("the MCP server closed the event stream")

    def send(message: dict) -> None:
        request = urllib.request.Request(post, data=json.dumps(message).encode(),
                                         headers={"Content-Type": "application/json"})
        urllib.request.urlopen(request, timeout=60).read()

    def answer(ident: int) -> dict:
        while True:
            message = json.loads(next_event()[1])
            if message.get("id") == ident:
                return message

    _, endpoint = next_event()
    post = base + endpoint if endpoint.startswith("/") else endpoint
    try:
        send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "bench", "version": "1"}}})
        answer(1)
        send({"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}})
        send({"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}})
        return [tool["name"] for tool in answer(2)["result"]["tools"]]
    finally:
        stream.close()


def start(args) -> int:
    home = os.path.abspath(args.home)
    for name in ("config", "system", "log", "plugins"):
        os.makedirs(os.path.join(home, name), exist_ok=True)
    _write(os.path.join(home, "idea.properties"),
           "".join(f"idea.{name}.path={home}/{name}\n" for name in ("config", "system", "log", "plugins")))
    _write(os.path.join(home, "idea64.vmoptions"), _vm_options(args.idea))
    env = dict(os.environ, IDEA_PROPERTIES=os.path.join(home, "idea.properties"),
               IDEA_VM_OPTIONS=os.path.join(home, "idea64.vmoptions"))
    if args.java_home:
        env["JAVA_HOME"] = args.java_home
    log = os.path.join(home, "log", "idea.log")
    if os.path.exists(log):
        os.rename(log, f"{log}.{int(time.time())}")
    server_out = os.path.join(home, "server.out")
    command = [args.idea, "mcpServer", f"--project={os.path.abspath(args.project)}", f"--port={args.port}",
               "--invocation-mode=DIRECT", f"--allowed-tools={args.tools}"]
    started = time.time()
    with open(server_out, "w") as out:
        process = subprocess.Popen(command, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                   env=env, start_new_session=True)
    _write(os.path.join(home, "server.pid"), str(process.pid))
    print(f"started {' '.join(command)} (pid {process.pid})", flush=True)

    quiet_since, last_lines = None, -1
    while True:
        if process.poll() is not None:
            with open(server_out, encoding="utf-8", errors="replace") as handle:
                print(handle.read()[-3000:], file=sys.stderr)
            print(f"the IDE exited with {process.returncode} before it was ready", file=sys.stderr)
            return 1
        if time.time() - started > args.timeout:
            print(f"not ready after {args.timeout} s; see {log}", file=sys.stderr)
            return 1
        with open(server_out, encoding="utf-8", errors="replace") as handle:
            server_up = "MCP Server started" in handle.read()
        state = _log_state(log)
        settled = (server_up and state["dumb"] is False and state["imports_started"] > 0
                   and state["imports_done"] >= state["imports_started"])
        if settled and state["lines"] == last_lines:
            quiet_since = quiet_since or time.time()
        else:
            quiet_since = None
        last_lines = state["lines"]
        if quiet_since and time.time() - quiet_since >= args.quiet:
            break
        time.sleep(1)
    tools = _mcp_tools(args.port)
    missing = [tool for tool in args.tools.split(",") if tool not in tools]
    if missing:
        print(f"the server does not offer {', '.join(missing)}; it offers {', '.join(tools)}", file=sys.stderr)
        return 1
    print(json.dumps({"ready_s": (state["last_exit_ms"] or 0) / 1000.0, "imports": state["imports_done"],
                      "tools": len(tools), "pid": process.pid}))
    return 0


def stop(args) -> int:
    pid_file = os.path.join(os.path.abspath(args.home), "server.pid")
    if not os.path.isfile(pid_file):
        return 0
    with open(pid_file) as handle:
        pid = int(handle.read().strip())
    try:
        os.killpg(pid, signal.SIGTERM)
    except ProcessLookupError:
        os.remove(pid_file)
        return 0
    for _ in range(60):
        try:
            os.killpg(pid, 0)
        except ProcessLookupError:
            break
        time.sleep(0.5)
    else:
        os.killpg(pid, signal.SIGKILL)
    os.remove(pid_file)
    print(f"stopped the IDE process group {pid}")
    return 0


def version(args) -> int:
    root = os.path.dirname(os.path.dirname(os.path.realpath(args.idea)))
    with open(os.path.join(root, "product-info.json"), encoding="utf-8") as handle:
        info = json.load(handle)
    print(f"{info.get('name', 'IntelliJ IDEA')} {info.get('version', '?')} (build {info.get('buildNumber', '?')})")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    run = commands.add_parser("start")
    run.add_argument("--idea", required=True)
    run.add_argument("--home", required=True)
    run.add_argument("--project", required=True)
    run.add_argument("--port", type=int, required=True)
    run.add_argument("--tools", required=True)
    run.add_argument("--java-home")
    run.add_argument("--quiet", type=int, default=20, help="seconds without new IDE log lines before ready")
    run.add_argument("--timeout", type=int, default=1500)
    run.set_defaults(func=start)
    end = commands.add_parser("stop")
    end.add_argument("--home", required=True)
    end.set_defaults(func=stop)
    show = commands.add_parser("version")
    show.add_argument("--idea", required=True)
    show.set_defaults(func=version)
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
