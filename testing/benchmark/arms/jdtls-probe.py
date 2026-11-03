#!/usr/bin/env python3
"""Starts `jdtls` from PATH in the current directory the way an editor does, waits until it serves, and stops it.

    jdtls-probe.py --symbol CLASS [--build] [--timeout S]

The jdtls arm's warm-up runs it with --build once per project copy: the Gradle import and a full build land in the
arm's jdtls workspace, which later sessions reuse. Its startup probe runs it without --build: on that warm workspace,
the time to ready is the startup every agent session pays before its first LSP answer, because Claude Code's plugin
starts one jdtls per session. The last output line is JSON: ready_s (launch to the "ServiceReady" status),
first_answer_s (launch to the answer of a workspace/symbol query) and, with --build, built_s (to the end of
java/buildWorkspace). Wall time includes the wait for jdtls to exit, so startup_s (= first_answer_s) is the startup.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import threading
import time
from pathlib import Path


class Server:
    def __init__(self):
        self.process = subprocess.Popen(["jdtls"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL)
        self.next_id = 0
        self.lock = threading.Lock()

    def send(self, message: dict) -> None:
        body = json.dumps(message).encode()
        with self.lock:
            self.process.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
            self.process.stdin.flush()

    def request(self, method: str, params) -> int:
        self.next_id += 1
        self.send({"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params})
        return self.next_id

    def read(self) -> dict | None:
        length = None
        while True:
            line = self.process.stdout.readline()
            if not line:
                return None
            line = line.strip()
            if not line:
                break
            if line.lower().startswith(b"content-length:"):
                length = int(line.split(b":")[1])
        return json.loads(self.process.stdout.read(length))

    def answer_server_request(self, message: dict) -> None:
        """jdtls asks the client for configuration and registrations; a silent client stalls it."""
        result = None
        if message["method"] == "workspace/configuration":
            result = [None] * len(message.get("params", {}).get("items", []))
        self.send({"jsonrpc": "2.0", "id": message["id"], "result": result})


def stop(process: subprocess.Popen) -> None:
    """jdtls can take a while to exit after `exit`; the workspace is saved by then, so don't wait for long."""
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()


def wait_for(server: Server, done, deadline: float, statuses: list) -> dict:
    while time.time() < deadline:
        message = server.read()
        if message is None:
            raise RuntimeError("jdtls exited")
        if "method" in message and "id" in message:
            server.answer_server_request(message)
            continue
        if message.get("method") == "language/status":
            statuses.append(message["params"].get("type"))
        if done(message):
            return message
    raise TimeoutError("jdtls did not answer in time")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", action="store_true", help="also build the whole workspace (warm-up)")
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("--symbol", required=True, help="a class of the project, the workspace/symbol query")
    args = parser.parse_args()
    root = Path(os.getcwd())
    started = time.time()
    deadline = started + args.timeout
    server = Server()
    statuses: list = []
    result = {}
    try:
        init = server.request("initialize", {
            "processId": os.getpid(), "rootUri": root.as_uri(), "rootPath": str(root),
            "workspaceFolders": [{"uri": root.as_uri(), "name": root.name}],
            "capabilities": {"workspace": {"configuration": True, "workspaceFolders": True},
                             "textDocument": {"callHierarchy": {}, "references": {}, "implementation": {}}}})
        wait_for(server, lambda m: m.get("id") == init, deadline, statuses)
        server.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        wait_for(server, lambda m: m.get("method") == "language/status"
                 and m["params"].get("type") == "ServiceReady", deadline, statuses)
        result["ready_s"] = round(time.time() - started, 2)
        query = server.request("workspace/symbol", {"query": args.symbol})
        answer = wait_for(server, lambda m: m.get("id") == query, deadline, statuses)
        result["first_answer_s"] = round(time.time() - started, 2)
        result["symbols"] = len(answer.get("result") or [])
        if not result["symbols"]:
            raise RuntimeError(f"workspace/symbol found no '{args.symbol}': pass --symbol with a class of the project")
        if args.build:
            build = server.request("java/buildWorkspace", True)
            answer = wait_for(server, lambda m: m.get("id") == build, deadline, statuses)
            result["built_s"] = round(time.time() - started, 2)
            result["build_result"] = answer.get("result")
        shut = server.request("shutdown", None)
        wait_for(server, lambda m: m.get("id") == shut, time.time() + 60, statuses)
        server.send({"jsonrpc": "2.0", "method": "exit", "params": None})
        server.process.stdin.close()
    except (RuntimeError, TimeoutError) as error:
        server.process.kill()
        print(f"jdtls probe failed: {error}; statuses seen: {statuses}", file=sys.stderr)
        return 1
    finally:
        stop(server.process)
    result["statuses"] = sorted(set(filter(None, statuses)))
    result["startup_s"] = result["first_answer_s"]
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
