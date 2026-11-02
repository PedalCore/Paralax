#!/usr/bin/env python3
"""Model Context Protocol server exposing the Paralax editor's commands as tools (stdio transport).

Every editor command becomes a tool ("entity.create" -> "entity_create") whose input schema is generated from the
command's parameters, so the tool list always matches the editor. render.capture results are returned with the
image attached, so a model can see what it built.

Configure an MCP client to run one of:
    python3 tools/agent/paralax_mcp.py --info <project>/.paralax/AgentServer.json     (attach to a running editor)
    python3 tools/agent/paralax_mcp.py --launch <Paralax-Editor> -- --project <dir>    (start an editor)

Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import base64
import json
import sys
from pathlib import Path
from typing import Any, Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
from paralax_agent import AgentClient, AgentError, EditorSession, use_utf8_stdio  # noqa: E402

PROTOCOL_VERSION = "2025-06-18"
SUPPORTED_VERSIONS = {"2024-11-05", "2025-03-26", "2025-06-18"}
JSON_TYPES = {"string": "string", "number": "number", "integer": "integer", "boolean": "boolean", "object": "object",
              "array": "array"}
MAX_IMAGE_BYTES = 8 * 1024 * 1024


def tool_name(command: str) -> str:
    return command.replace(".", "_")


def make_tool(command: dict) -> dict:
    properties: dict[str, Any] = {}
    required = []
    for parameter in command.get("parameters", []):
        schema: dict[str, Any] = {"description": parameter.get("description", "")}
        if parameter.get("type") in JSON_TYPES:
            schema["type"] = JSON_TYPES[parameter["type"]]
        properties[parameter["name"]] = schema
        if parameter.get("required"):
            required.append(parameter["name"])
    description = command.get("description", "")
    if command.get("undoable"):
        description += " (Undoable.)"
    return {
        "name": tool_name(command["name"]),
        "description": description,
        "inputSchema": {"type": "object", "properties": properties, "required": required, "additionalProperties": False},
    }


class Bridge:
    def __init__(self, client: AgentClient):
        self.client = client
        self.commands: dict[str, str] = {}

    def list_tools(self) -> list:
        commands = self.client.call("help")["commands"]
        self.commands = {tool_name(c["name"]): c["name"] for c in commands}
        return [make_tool(c) for c in commands]

    def call_tool(self, name: str, arguments: dict) -> dict:
        if not self.commands:
            self.list_tools()
        command = self.commands.get(name)
        if command is None:
            return {"content": [{"type": "text", "text": f"Unknown tool '{name}'"}], "isError": True}
        try:
            result = self.client.call(command, arguments)
        except AgentError as error:
            return {"content": [{"type": "text", "text": error.message}], "isError": True}
        content = [{"type": "text", "text": json.dumps(result, indent=2, ensure_ascii=False)}]
        if command == "render.capture" and isinstance(result, dict) and "path" in result:
            image = Path(result["path"])
            try:
                if image.stat().st_size <= MAX_IMAGE_BYTES:
                    content.append({"type": "image", "mimeType": "image/png",
                                    "data": base64.b64encode(image.read_bytes()).decode("ascii")})
            except OSError:
                pass
        return {"content": content, "structuredContent": result if isinstance(result, dict) else {"result": result}}


def respond(message_id: Any, result: Optional[dict] = None, error: Optional[dict] = None) -> None:
    message: dict[str, Any] = {"jsonrpc": "2.0", "id": message_id}
    if error is not None:
        message["error"] = error
    else:
        message["result"] = result if result is not None else {}
    sys.stdout.write(json.dumps(message, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def handle(bridge: Bridge, message: dict) -> None:
    method = message.get("method")
    message_id = message.get("id")
    params = message.get("params") or {}
    if message_id is None:
        return  # Notifications (initialized, cancelled) need no reply.
    if not isinstance(params, dict):
        respond(message_id, error={"code": -32602, "message": "params must be an object"})
        return
    if method == "initialize":
        requested = params.get("protocolVersion")
        respond(message_id, {
            "protocolVersion": requested if requested in SUPPORTED_VERSIONS else PROTOCOL_VERSION,
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": {"name": "paralax-editor", "version": "0.1.0"},
            "instructions": "Tools drive the Paralax game editor. Start with 'help' and 'component_types'; scene "
                            "edits are undoable with edit_undo; render_capture shows the result.",
        })
    elif method == "ping":
        respond(message_id, {})
    elif method == "tools/list":
        respond(message_id, {"tools": bridge.list_tools()})
    elif method == "tools/call":
        arguments = params.get("arguments") or {}
        if not isinstance(params.get("name"), str) or not isinstance(arguments, dict):
            respond(message_id, error={"code": -32602, "message": "tools/call needs a name and an arguments object"})
        else:
            respond(message_id, bridge.call_tool(params["name"], arguments))
    else:
        respond(message_id, error={"code": -32601, "message": f"Method not found: {method}"})


def main(argv: list[str]) -> int:
    use_utf8_stdio()  # MCP's stdio transport is UTF-8.
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--info", help="AgentServer.json of a running editor")
    group.add_argument("--launch", help="editor executable to start")
    parser.add_argument("editor_args", nargs="*", help="arguments for the launched editor (after --)")
    args = parser.parse_args(argv)

    client: AgentClient = EditorSession.launch(args.launch, args.editor_args) if args.launch else AgentClient.from_info_file(args.info)
    bridge = Bridge(client)
    try:
        for line in sys.stdin:
            if not line.strip():
                continue
            try:
                message = json.loads(line)
            except json.JSONDecodeError:
                respond(None, error={"code": -32700, "message": "Parse error"})
                continue
            if not isinstance(message, dict):
                respond(None, error={"code": -32600, "message": "Batches are not supported"})
                continue
            try:
                handle(bridge, message)
            except (ConnectionError, OSError) as error:
                # The editor is gone (or the connection timed out and is no longer usable): nothing more can work.
                print(f"Lost the editor connection: {error}", file=sys.stderr)
                if message.get("id") is not None:
                    respond(message.get("id"), error={"code": -32603, "message": f"Lost the editor connection: {error}"})
                return 1
            except Exception as error:  # noqa: BLE001 - one bad message must not take the bridge down
                if message.get("id") is not None:
                    respond(message.get("id"), error={"code": -32603, "message": f"Internal error: {error}"})
    finally:
        if isinstance(client, EditorSession):
            client.quit()
        else:
            client.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
