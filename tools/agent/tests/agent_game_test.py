#!/usr/bin/env python3
"""End-to-end test: an agent builds a small game from nothing, plays it with synthetic input, and exports it.

The game ("catch"): a ball drops onto a paddle that the player steers with Left and Right; each catch scores a point,
plays a sound and drops the ball again from above the paddle. Everything is done through editor commands, as an agent
would: creating the project, writing scripts, adding entities with physics and audio, playing deterministically with
injected key presses, checking the score through the running scripts, rendering, and exporting a standalone game.

Run by CTest: agent_game_test.py <Paralax-Editor> <output dir>.
"""

from __future__ import annotations

import math
import shutil
import struct
import subprocess
import sys
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from paralax_agent import AgentError, EditorSession  # noqa: E402

PADDLE = """
-- Steered with Left and Right; scores when the ball lands on it.
local Paddle = { speed = 6 }

function Paddle:OnCreate()
	Score, Jumps, Scrolled = 0, 0, 0
end

function Paddle:OnUpdate(dt)
	if Input.WasKeyPressed(Key.Space) then Jumps = Jumps + 1 end
	Scrolled = Scrolled + Input.GetMouseScrollDelta().y
	local t = self.entity:GetComponent("Transform")
	local x = t.Translation.x
	if Input.IsKeyDown(Key.Left) then x = x - self.speed * dt end
	if Input.IsKeyDown(Key.Right) then x = x + self.speed * dt end
	t.Translation = vec3(math.max(-4, math.min(4, x)), t.Translation.y, t.Translation.z)
end

function Paddle:OnCollisionEnter(other)
	if other.name == "Ball" then
		Score = Score + 1
		Played = Audio.PlayOneShot("Audio/Catch.wav")
		-- Drop it again from above the paddle.
		local paddle = self.entity:GetComponent("Transform").Translation
		other:GetComponent("Transform").Translation = vec3(paddle.x, 6, 0)
		Physics.SetLinearVelocity(other, vec3(0, 0, 0))
	end
end

return Paddle
"""


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_sound(path: Path) -> None:
    rate = 22050
    frames = rate // 10
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as file:
        file.setnchannels(1)
        file.setsampwidth(2)
        file.setframerate(rate)
        file.writeframes(struct.pack(f"<{frames}h", *(int(8000 * math.sin(2 * math.pi * 880 * i / rate)) for i in range(frames))))


def build(editor: EditorSession, project: Path) -> None:
    editor.call("scene.new", name="Catch")
    paddle_script = editor.call("script.create", path="Scripts/Paddle.lua", source=PADDLE)["handle"]
    write_sound(project / "Assets" / "Audio" / "Catch.wav")
    editor.call("asset.import", path="Audio/Catch.wav")

    editor.call("entity.create", name="Camera", components={
        "Camera": {"Primary": True}, "AudioListener": {}, "Transform": {"Translation": [0, 3, 12]}})
    editor.call("entity.create", name="Sun", components={"DirectionalLight": {"Intensity": 3}, "Transform": {"Rotation": [-0.4, 0.3, 0.1, 0.86]}})
    editor.call("entity.create", name="Paddle", components={
        "Transform": {"Translation": [0, 0, 0], "Scale": [2, 0.3, 1]}, "Mesh": {"Mesh": "1"},
        "Collider": {"Shape": "Box"}, "Rigidbody": {"Type": "Kinematic"}, "Script": {"Script": paddle_script}})
    editor.call("entity.create", name="Ball", components={
        "Transform": {"Translation": [0, 4, 0], "Scale": [0.5, 0.5, 0.5]}, "Mesh": {"Mesh": "2"},
        "Collider": {"Shape": "Sphere"}, "Rigidbody": {"Mass": 1}})
    editor.call("scene.save", path="Scenes/Catch.pxscene")
    editor.call("project.set_start_scene", path="Scenes/Catch.pxscene")


def play(editor: EditorSession) -> None:
    editor.call("play.start", paused=True)
    # The ball falls straight onto the paddle: one catch in about a second.
    editor.call("play.step", frames=70)
    check(editor.call("script.errors")["errors"] == [], "the game reported script errors")
    check(editor.call("script.eval", code="Score")["result"] == "1", "the first drop should have been caught")
    check(editor.call("script.eval", code="Played")["result"] == "true", "the catch sound should have played")
    check(editor.call("audio.status")["output"] == "Offline", "a headless editor mixes audio offline")

    # One-frame transitions survive the time between requests: the editor runs frames in between, yet the game's next
    # update still sees exactly one press and the scroll.
    editor.call("input.key", key="Space", action="press")
    editor.call("input.scroll", x=0, y=3)
    for _ in range(3):
        editor.call("scene.info")
    editor.call("play.step", frames=2)
    editor.call("input.key", key="Space", action="release")
    editor.call("play.step", frames=1)
    check(editor.call("script.eval", code="Jumps, Scrolled")["result"] == "1\t3.0", "the press and scroll must be seen exactly once")

    # Steer right for half a second: the paddle follows the injected key (held across steps), then stops.
    editor.call("input.key", key="Right", action="press")
    editor.call("play.step", frames=30)
    editor.call("input.key", key="Right", action="release")
    editor.call("play.step", frames=5)
    x = float(editor.call("script.eval", code="Scene.FindEntityByName('Paddle'):GetComponent('Transform').Translation.x")["result"])
    check(2.5 < x < 3.5, f"the paddle should have moved about 3 m right, got {x}")

    # The ball respawned above the paddle's old position; steering away makes it miss, so the score stays.
    editor.call("input.key", key="Left", action="press")
    editor.call("play.step", frames=40)
    editor.call("input.key", key="Left", action="release")
    editor.call("play.step", frames=90)
    check(editor.call("script.eval", code="Score")["result"] == "1", "a dodged ball must not score")
    check(editor.call("script.errors")["errors"] == [], "the game reported script errors")
    editor.call("play.stop")

    # Iterating on the game: make the paddle faster and check the change takes effect on the next run.
    editor.call("script.update", path="Scripts/Paddle.lua", source=PADDLE.replace("speed = 6", "speed = 12"))
    editor.call("play.start", paused=True)
    editor.call("input.key", key="Right", action="press")
    editor.call("play.step", frames=15)
    editor.call("input.key", key="Right", action="release")
    x = float(editor.call("script.eval", code="Scene.FindEntityByName('Paddle'):GetComponent('Transform').Translation.x")["result"])
    check(2.5 < x < 3.5, f"the faster paddle should cover 3 m in a quarter second, got {x}")
    editor.call("play.stop")


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    executable, output = Path(sys.argv[1]), Path(sys.argv[2])
    shutil.rmtree(output, ignore_errors=True)
    output.mkdir(parents=True)
    project = output / "Catch"
    with EditorSession.launch(executable, ["--headless", "--new-project", str(project)]) as editor:
        build(editor, project)
        play(editor)
        # Output files stay inside the project (agent paths are untrusted).
        capture = editor.call("render.capture", path="Captures/Catch.png", width=640, height=360)
        check(Path(capture["path"]).is_file(), "render.capture wrote no image")
        try:
            editor.call("input.key", key="Jump", action="press")
            raise AssertionError("an unknown key must be refused")
        except AgentError:
            pass
        export = editor.call("project.export", output=str(output / "Build"))
    # The exported game runs on its own, headless.
    result = subprocess.run([export["executable"], "--headless", "--frames", "120", "--fixed-timestep", "0.0166667", "--fail-on-script-error"],
                            cwd=output, capture_output=True, text=True, timeout=300)
    check(result.returncode == 0, f"the exported game failed ({result.returncode}):\n{result.stdout}\n{result.stderr}")
    print("Agent game test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
