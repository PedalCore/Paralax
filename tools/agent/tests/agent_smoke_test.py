#!/usr/bin/env python3
"""End-to-end test: an agent launches the editor headless, builds a small scene over JSON-RPC and through the MCP
bridge, renders it and quits. Run by CTest: agent_smoke_test.py <Paralax-Editor> <output dir>."""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from paralax_agent import AgentClient, AgentError, EditorSession  # noqa: E402


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def exercise_rpc(editor: AgentClient, output: Path) -> None:
    # The demo scene an editor starts with is not an edit: nothing to save, nothing to undo.
    info = editor.call("scene.info")
    check(info["entities"] > 0 and not info["dirty"] and not info["canUndo"], f"the editor did not start clean: {info}")
    commands = {c["name"] for c in editor.call("help")["commands"]}
    check({"entity.create", "component.set", "render.capture", "editor.quit", "console.get", "console.clear"} <= commands,
          f"missing commands: {commands}")

    # A failure that points at the log can be explained from it.
    revision = editor.call("console.get", limit=1)["revision"]
    try:
        editor.call("scene.open", path="Missing.pxscene")
        raise AssertionError("expected a missing scene to fail")
    except AgentError as error:
        check("console.get" in error.message, f"the error does not point at the log: {error.message}")
    log = editor.call("console.get", sinceRevision=revision, minLevel="error")["messages"]
    check(any("Missing.pxscene" in message["text"] for message in log), f"the log does not say why: {log}")

    editor.call("scene.new", name="AgentScene")
    ground = editor.call("entity.create", name="Ground", components={
        "Transform": {"Scale": [10, 1, 10]}, "Mesh": {"Mesh": "3"}})["id"]
    ball = editor.call("entity.create", name="Ball", components={
        "Transform": {"Translation": [0, 1, 0]}, "Mesh": {"Mesh": "2"}})["id"]
    editor.call("entity.create", name="Sun", components={"DirectionalLight": {}})
    camera = editor.call("entity.create", name="Camera", components={"Transform": {"Translation": [0, 2, 6]}, "Camera": {}})["id"]
    check(editor.call("scene.info")["entities"] == 4, "expected four entities")

    editor.call("entity.set_parent", id=ball, parent=ground)
    check(editor.call("entity.get", id=ball)["parent"] == ground, "parenting failed")
    editor.call("edit.undo")
    check(editor.call("entity.get", id=ball).get("parent") in (None, "", "0"), "undo did not restore the parent")

    try:
        editor.call("entity.get", id="12345")
        raise AssertionError("expected an error for an unknown entity")
    except AgentError as error:
        check(error.code == -32000, f"unexpected error code {error.code}")

    # One frame, several commands.
    results = editor.batch([("entity.rename", {"id": camera, "name": "Main Camera"}), ("no.such.command", {}), ("scene.info", {})])
    check(isinstance(results[1], AgentError) and results[1].code == -32601, "batch error not reported")
    check(results[2]["entities"] == 4, "batch did not run in order")

    # UI-owned state is drivable too: editor camera, gizmo and renderer settings (registered by the editor layer).
    camera_state = editor.call("editor.camera.set", {"position": [4, 3, 6], "target": [0, 0.5, 0]})
    check(abs(camera_state["position"][0] - 4) < 1e-3, "editor camera was not placed")
    editor.call("editor.camera.focus", id=ball)
    check(editor.call("editor.gizmo.get") == {"operation": "none", "space": "local"}, "the editor did not start without a gizmo")
    gizmo = editor.call("editor.gizmo.set", operation="rotate", space="world")
    check(gizmo == {"operation": "rotate", "space": "world"}, f"editor.gizmo.set did not return the gizmo: {gizmo}")
    try:
        editor.call("editor.gizmo.set", operation="explode")
        raise AssertionError("expected an invalid gizmo operation to fail")
    except AgentError:
        pass
    check(editor.call("editor.gizmo.get") == gizmo, "a refused gizmo change must change nothing")
    settings = editor.call("render.settings.set", {"settings": {"Exposure": 1.5, "Tonemapper": "ACES"}})
    check(settings["Exposure"] == 1.5 and settings["Tonemapper"] == "ACES", f"settings not applied: {settings}")
    try:
        editor.call("render.settings.set", {"settings": {"Exposure": 1.0, "Bogus": 1}})
        raise AssertionError("expected an unknown setting to fail")
    except AgentError:
        pass
    check(editor.call("render.settings.get")["Exposure"] == 1.5, "a failed settings change must change nothing")

    # Without a project there is nowhere to write scripts; play mode still runs and stops cleanly.
    try:
        editor.call("script.create", path="Scripts/Mover.lua")
        raise AssertionError("script.create needs a project")
    except AgentError:
        pass
    editor.call("play.start")
    editor.call("play.step", frames=5)
    try:
        editor.call("edit.undo")
        raise AssertionError("undo must be refused while playing")
    except AgentError as error:
        check("play.stop" in error.message, f"unexpected undo error while playing: {error.message}")
    check(editor.call("script.eval", code="Scene.GetEntityCount()")["result"] == "4", "script.eval did not see the running scene")
    check(editor.call("script.errors")["errors"] == [], "unexpected script errors")
    editor.call("play.stop")

    image = output / "AgentCapture.png"
    capture = editor.call("render.capture", path=str(image), width=320, height=180)
    check(capture["width"] == 320 and image.stat().st_size > 1000, "capture failed")
    # The viewport renders every editor frame too, but nothing renders between the requests of one batch: these
    # statistics are the capture's, whose camera sees the ground and the ball (and no point or spot lights).
    captured, stats = editor.batch([("render.capture", {"path": str(image), "width": 320, "height": 180}), ("render.stats", {})])
    check(not isinstance(captured, AgentError) and not isinstance(stats, AgentError), f"batch failed: {captured}, {stats}")
    check(set(stats) == {"drawCalls", "shadowDrawCalls", "triangles", "culledSubmeshes", "textGlyphs", "lights",
                         "staticLightingGenerations"}, f"unexpected render statistics: {stats}")
    check(stats["drawCalls"] == 2 and stats["triangles"] > 2 and stats["lights"] == 0, f"unexpected render statistics: {stats}")
    editor_image = output / "AgentCaptureEditorCamera.png"
    editor.call("render.capture", path=str(editor_image), width=160, height=90, camera="editor")
    check(editor_image.exists(), "editor camera capture failed")
    try:
        editor.call("render.capture", path=str(output / "notes.txt"))
        raise AssertionError("expected a non-PNG capture path to fail")
    except AgentError:
        pass


def exercise_project(editor: AgentClient, project: Path) -> None:
    """The game-building loop an agent follows: script, prefab, play, inspect, save, capture."""
    info = editor.call("scene.info")  # A new project opens with the demo scene, unedited.
    check(info["entities"] > 0 and not info["dirty"] and not info["canUndo"], f"the project did not open clean: {info}")
    editor.call("scene.new", name="Game")
    mover = editor.call("script.create", path="Scripts/Mover.lua", source="""
        local Mover = { speed = 2 }
        function Mover:OnUpdate(dt)
            local t = self.entity:GetComponent("Transform")
            t.Translation = t.Translation + vec3(self.speed * dt, 0, 0)
        end
        return Mover
    """)
    spawner = editor.call("script.create", path="Scripts/Spawner.lua", source="""
        local Spawner = {}
        function Spawner:OnCreate()
            for i = 1, 3 do Scene.Instantiate("Prefabs/Crate.pxprefab", { position = vec3(i * 2, 0, 0) }) end
        end
        return Spawner
    """)
    crate = editor.call("entity.create", name="Crate", components={"Mesh": {"Mesh": "1"}})["id"]
    editor.call("prefab.create", id=crate, path="Prefabs/Crate")
    editor.call("entity.destroy", id=crate)
    editor.call("entity.create", name="Mover", components={"Script": {"Script": mover["handle"]}, "Mesh": {"Mesh": "2"}})
    editor.call("entity.create", name="Spawner", components={"Script": {"Script": spawner["handle"]}})
    editor.call("entity.create", name="Sun", components={"DirectionalLight": {}})
    editor.call("entity.create", name="Camera", components={"Camera": {}, "Transform": {"Translation": [3, 3, 10]}})

    # Paused: the game advances only through play.step, so results are exact.
    editor.call("play.start", paused=True)
    editor.call("play.step", frames=60, dt=1 / 60)
    x = float(editor.call("script.eval", code="Scene.FindEntityByName('Mover'):GetComponent('Transform').Translation.x")["result"])
    check(abs(x - 2.0) < 1e-3, f"the Mover script did not move its entity (x = {x})")
    count = editor.call("script.eval", code="Scene.GetEntityCount()")["result"]
    check(count == "7", f"the Spawner script did not instantiate three crates ({count} entities)")
    check(editor.call("script.errors")["errors"] == [], f"script errors: {editor.call('script.errors')}")
    capture = editor.call("render.capture", path="Captures/Playing.png", width=320, height=180)
    check((project / "Captures" / "Playing.png").stat().st_size > 1000, f"capture failed: {capture}")
    editor.call("play.stop")

    editor.call("scene.save", path="Scenes/Main")
    check((project / "Assets" / "Scenes" / "Main.pxscene").exists(), "scene was not saved")
    check(editor.call("scene.open", path="Scenes/Main.pxscene")["entities"] == 4, "saved scene did not reload")
    for bad in ("../Escaped.png", str(project.parent / "Escaped.png")):
        try:
            editor.call("render.capture", path=bad)
            raise AssertionError(f"capture outside the project was allowed: {bad}")
        except AgentError:
            pass


def exercise_mcp(info_file: Path, output: Path) -> None:
    bridge = subprocess.Popen([sys.executable, str(Path(__file__).resolve().parent.parent / "paralax_mcp.py"), "--info", str(info_file)],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding="utf-8")
    assert bridge.stdin and bridge.stdout

    def request(message_id: int, method: str, params: dict) -> dict:
        # Unescaped, so non-ASCII text reaches the bridge as raw UTF-8, as MCP clients send it.
        bridge.stdin.write(json.dumps({"jsonrpc": "2.0", "id": message_id, "method": method, "params": params}, ensure_ascii=False) + "\n")
        bridge.stdin.flush()
        response = json.loads(bridge.stdout.readline())
        check(response.get("id") == message_id, f"mismatched response {response}")
        return response

    try:
        init = request(1, "initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test", "version": "1"}})
        check(init["result"]["capabilities"]["tools"] is not None, "no tools capability")
        bridge.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")

        tools = {t["name"]: t for t in request(2, "tools/list", {})["result"]["tools"]}
        check("entity_create" in tools and "render_capture" in tools, "tools missing")
        check("path" in tools["render_capture"]["inputSchema"]["required"], "schema lacks required parameters")

        created = request(3, "tools/call", {"name": "entity_create", "arguments": {"name": "FromMCP"}})["result"]
        check(not created.get("isError") and "id" in created["structuredContent"], f"entity_create failed: {created}")

        failed = request(4, "tools/call", {"name": "entity_rename", "arguments": {"id": "1"}})["result"]
        check(failed.get("isError") is True, "a failing command must be reported as a tool error")

        captured = request(5, "tools/call", {"name": "render_capture", "arguments": {"path": str(output / "McpCapture.png"), "width": 64, "height": 64}})["result"]
        check(any(c["type"] == "image" and c["mimeType"] == "image/png" for c in captured["content"]), "capture image not attached")

        # Non-ASCII names and paths cross the bridge's stdio in both directions as UTF-8 (on Windows its pipes would
        # otherwise use the ANSI code page) and the editor's path conversions.
        name = "Ziegel-é-日本"
        named = request(6, "tools/call", {"name": "entity_create", "arguments": {"name": name}})["result"]
        check(not named.get("isError"), f"entity_create with a non-ASCII name failed: {named}")
        fetched = request(7, "tools/call", {"name": "entity_get", "arguments": {"id": named["structuredContent"]["id"]}})["result"]
        check(fetched["structuredContent"].get("name") == name, f"the non-ASCII name changed on the way: {fetched}")
        scene = output.resolve() / "Szene-é-日本.pxscene"
        saved = request(8, "tools/call", {"name": "scene_save", "arguments": {"path": str(scene)}})["result"]
        check(not saved.get("isError") and Path(saved["structuredContent"]["path"]) == scene and scene.is_file(),
              f"scene_save to a non-ASCII path failed: {saved}")
    finally:
        bridge.stdin.close()
        bridge.wait(30)
    check(bridge.returncode == 0, f"bridge exited with {bridge.returncode}")


def check_unwritable_info_file(executable: Path, directory: Path) -> None:
    """An editor that cannot write its agent connection file, which holds the token, closes at once with exit code 1
    rather than leave a launcher waiting for the file until it gives up."""
    blocker = directory / "NotADirectory"
    blocker.write_text("a file, so nothing can be created inside it", encoding="utf-8")
    info = blocker / "AgentServer.json"
    # Were the failure ignored, the editor would run all its frames and exit with 0.
    result = subprocess.run([str(executable), "--headless", "--frames", "600", "--agent-port", "0", "--agent-info", str(info)],
                            cwd=directory, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=240)
    log = result.stdout + result.stderr
    check(result.returncode == 1, f"expected exit code 1 without a connection file, got {result.returncode}: {log[-2000:]}")
    check("agent connection file" in log and "closing the editor" in log, f"the editor did not say why it closed: {log[-2000:]}")
    check(not info.exists(), "a connection file appeared where none can be written")


def main() -> int:
    # Absolute, because the editor runs in the output directory (where its log goes) and takes these paths there.
    executable, output = Path(sys.argv[1]).absolute(), Path(sys.argv[2]).absolute()
    output.mkdir(parents=True, exist_ok=True)
    # Each editor session runs in a directory of its own, so the second does not truncate the first's Logs/Editor.log.
    sessions = {name: output / name for name in ("RpcSession", "InfoFileSession", "ProjectSession")}
    for directory in sessions.values():
        directory.mkdir(exist_ok=True)
    editor = None
    try:
        editor = EditorSession.launch(executable, ["--headless", "--validation"], cwd=sessions["RpcSession"])
        exercise_rpc(editor, output)
        info = output / "AgentServer.json"
        info.write_text(json.dumps({"port": editor.port, "token": editor.token}), encoding="utf-8")
        exercise_mcp(info, output)
    except BaseException:
        if editor is not None:
            editor.process.kill()
            editor.process.wait()
        raise
    code = editor.quit()
    # With --validation a Vulkan validation error fails the run (exit code 3, unless an earlier failure already set a
    # code), so 0 also means there were none.
    check(code == 0, f"editor exited with code {code}")

    check_unwritable_info_file(executable, sessions["InfoFileSession"])

    project = output / "Project"
    shutil.rmtree(project, ignore_errors=True)
    editor = None
    try:
        editor = EditorSession.launch(executable, ["--headless", "--validation", "--new-project", str(project)], cwd=sessions["ProjectSession"])
        exercise_project(editor, project)
    except BaseException:
        if editor is not None:
            editor.process.kill()
            editor.process.wait()
        raise
    code = editor.quit()
    check(code == 0, f"editor (project session) exited with code {code}")
    print("agent smoke test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
