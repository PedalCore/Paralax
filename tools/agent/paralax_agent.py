#!/usr/bin/env python3
"""Client for the Paralax editor's agent server (JSON-RPC 2.0 over loopback TCP, one message per line).

Library use:
    with EditorSession.launch("build/debug/bin/Paralax-Editor", ["--headless"]) as editor:
        entity = editor.call("entity.create", {"name": "Player"})
        editor.call("component.set", {"id": entity["id"], "components": {"Transform": {"Translation": [0, 1, 0]}}})

Command line:
    paralax_agent.py --info <AgentServer.json> help
    paralax_agent.py --info <AgentServer.json> entity.create '{"name": "Player"}'

Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import itertools
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any, Optional, Sequence


class AgentError(RuntimeError):
    """A JSON-RPC error returned by the editor (code and message as sent)."""

    def __init__(self, code: int, message: str):
        super().__init__(f"{message} (code {code})")
        self.code = code
        self.message = message


class AgentClient:
    """A connection to a running editor. Not thread-safe."""

    def __init__(self, port: int, token: str, host: str = "127.0.0.1", timeout: float = 120.0):
        self._socket = socket.create_connection((host, port), timeout=timeout)
        self._socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._reader = self._socket.makefile("rb")
        self._ids = itertools.count(1)
        self.port = port
        self.token = token
        try:
            self.call("session.authenticate", {"token": token})
        except BaseException:
            self.close()
            raise

    @classmethod
    def from_info_file(cls, path: os.PathLike | str, timeout: float = 120.0) -> "AgentClient":
        info = json.loads(Path(path).read_text(encoding="utf-8"))
        return cls(int(info["port"]), str(info["token"]), info.get("host", "127.0.0.1"), timeout)

    def call(self, method: str, params: Optional[dict] = None, /, **kwargs: Any) -> Any:
        """Runs one editor command and returns its result; raises AgentError on failure.

        Parameters can be given as a dict, as keyword arguments, or both (keywords win)."""
        merged = {**(params or {}), **kwargs}
        request_id = next(self._ids)
        response = self._exchange({"jsonrpc": "2.0", "id": request_id, "method": method, "params": merged})
        if not isinstance(response, dict) or response.get("id") != request_id:
            self.close()
            raise ConnectionError(f"Unexpected response from the editor: {response!r:.200}")
        if "error" in response:
            raise AgentError(int(response["error"].get("code", 0)), str(response["error"].get("message", "")))
        return response.get("result")

    def batch(self, calls: Sequence[tuple[str, dict]]) -> list:
        """Runs several commands in one editor frame. Returns one result or AgentError per call, in order."""
        requests = [{"jsonrpc": "2.0", "id": next(self._ids), "method": m, "params": p} for m, p in calls]
        reply = self._exchange(requests)
        if isinstance(reply, dict) and "error" in reply:  # The batch as a whole was rejected.
            error = AgentError(int(reply["error"].get("code", 0)), str(reply["error"].get("message", "")))
            return [error for _ in requests]
        if not isinstance(reply, list):
            self.close()
            raise ConnectionError(f"Unexpected batch response from the editor: {reply!r:.200}")
        responses = {r.get("id"): r for r in reply if isinstance(r, dict)}
        results = []
        for request in requests:
            response = responses.get(request["id"], {"error": {"code": 0, "message": "missing response"}})
            if "error" in response:
                results.append(AgentError(int(response["error"].get("code", 0)), str(response["error"].get("message", ""))))
            else:
                results.append(response.get("result"))
        return results

    def close(self) -> None:
        try:
            self._reader.close()
        finally:
            self._socket.close()

    def _exchange(self, message: Any) -> Any:
        try:
            self._socket.sendall(json.dumps(message, ensure_ascii=False).encode("utf-8") + b"\n")
            line = self._reader.readline()
        except OSError:
            # A timed-out exchange leaves the stream out of step with the requests: the connection is unusable.
            self.close()
            raise
        if not line:
            raise ConnectionError("The editor closed the connection")
        return json.loads(line)

    def __enter__(self) -> "AgentClient":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()


class EditorSession(AgentClient):
    """Launches an editor with the agent server enabled and connects to it."""

    process: subprocess.Popen

    @classmethod
    def launch(cls, executable: os.PathLike | str, arguments: Sequence[str] = (), startup_timeout: float = 120.0,
               cwd: Optional[os.PathLike | str] = None) -> "EditorSession":
        info_dir = tempfile.mkdtemp(prefix="paralax-agent-")
        info_file = Path(info_dir) / "AgentServer.json"
        process = subprocess.Popen([os.fspath(executable), *arguments, "--agent-port", "0", "--agent-info", os.fspath(info_file)],
                                   cwd=cwd)
        try:
            deadline = time.monotonic() + startup_timeout
            info = None
            while info is None:
                if process.poll() is not None:
                    raise RuntimeError(f"The editor exited with code {process.returncode} before its agent server started")
                if time.monotonic() > deadline:
                    raise TimeoutError("The editor's agent server did not start in time")
                # The file may be observed while still empty (it is created before the token is written).
                try:
                    text = info_file.read_text(encoding="utf-8")
                    info = json.loads(text) if text.strip() else None
                except (json.JSONDecodeError, OSError):
                    info = None
                if info is None:
                    time.sleep(0.05)
            session = cls(int(info["port"]), str(info["token"]), info.get("host", "127.0.0.1"))
        except BaseException:
            # Never leave a headless editor running without a controller.
            process.kill()
            process.wait()
            raise
        finally:
            shutil.rmtree(info_dir, ignore_errors=True)
        session.process = process
        return session

    def quit(self, timeout: float = 60.0) -> int:
        """Asks the editor to exit and returns its exit code."""
        try:
            self.call("editor.quit")
        except (AgentError, ConnectionError, OSError):
            pass
        self.close()
        try:
            return self.process.wait(timeout)
        except subprocess.TimeoutExpired:
            self.process.kill()
            raise

    def __exit__(self, *exc: object) -> None:
        if self.process.poll() is None:
            try:
                self.quit()
            except subprocess.TimeoutExpired:
                pass
        else:
            self.close()


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(description="Run a command in a running Paralax editor.")
    parser.add_argument("--info", required=True, help="the editor's AgentServer.json")
    parser.add_argument("method", help="command name, e.g. entity.create (use 'help' to list them)")
    parser.add_argument("params", nargs="?", default="{}", help="JSON object of parameters")
    args = parser.parse_args(argv)
    try:
        params = json.loads(args.params)
    except json.JSONDecodeError as error:
        print(f"params is not valid JSON: {error}", file=sys.stderr)
        return 2
    if not isinstance(params, dict):
        print("params must be a JSON object", file=sys.stderr)
        return 2
    try:
        with AgentClient.from_info_file(args.info) as client:
            print(json.dumps(client.call(args.method, params), indent=2, ensure_ascii=False))
    except AgentError as error:
        print(error, file=sys.stderr)
        return 1
    except (OSError, ValueError, KeyError) as error:
        print(f"Cannot reach the editor: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
