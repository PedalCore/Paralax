#!/usr/bin/env python3
"""Builds Gem Roller, a small physics game, in Paralax entirely through editor commands, as an agent would.

    python3 tools/agent/examples/gem_roller/build_gem_roller.py <Paralax-Editor> <new project dir> [--export <dir>]

Roll a ball along a floating course with the arrow keys (Space jumps), collect the gems, ride the moving platform
across the gap and reach the goal. The game logic is in the Lua scripts beside this file (Roller.lua for the ball and
the game, Gem.lua, Goal.lua, Platform.lua, FollowCamera.lua); the course, materials, lights and camera are created
with agent commands. The sound effects are generated here and written into the project's Assets folder, then
imported with asset.import (there is no command for writing binary files). tools/agent/tests/gem_roller_test.py plays
it with injected input.
"""

from __future__ import annotations

import argparse
import math
import struct
import sys
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from paralax_agent import EditorSession  # noqa: E402

HERE = Path(__file__).resolve().parent
CUBE, SPHERE = "1", "2"
GROUND_MATERIAL = "17"
SKY_ENVIRONMENT = "18"
SCRIPTS = ("Roller", "Gem", "Goal", "Platform", "FollowCamera")

# The course runs from the start (z = 2) towards -z. Pads are boxes whose top surface is at y = 0.
PADS = (
    ("StartPad", [0.0, -0.5, 0.0], [6.0, 1.0, 8.0]),     # z 4 .. -4
    ("Bridge", [0.0, -0.5, -8.0], [2.0, 1.0, 8.0]),      # z -4 .. -12, narrow
    ("MiddlePad", [0.0, -0.5, -16.0], [8.0, 1.0, 8.0]),  # z -12 .. -20, then a gap the platform crosses
    ("FinishPad", [0.0, -0.5, -33.0], [8.0, 1.0, 8.0]),  # z -29 .. -37
)
PLATFORM_CENTRE = [0.0, -0.25, -24.5]  # Slides +-2.8 along z: its 3 m deck comes within 0.2 m of both edges of the gap.
GEMS = ([2.0, 0.8, -1.0], [0.0, 0.8, -8.0], [-2.5, 0.8, -16.0], [2.5, 0.8, -16.0], [0.0, 0.8, -31.0])
GOAL = [0.0, 1.0, -35.0]


def write_tone(path: Path, frequencies: list[float], seconds: float, volume: float = 0.25) -> None:
    """A short tone (a quick sweep through the given frequencies), 16-bit mono, faded at both ends."""
    rate = 22050
    frames = int(rate * seconds)
    path.parent.mkdir(parents=True, exist_ok=True)
    samples = []
    phase = 0.0
    for i in range(frames):
        frequency = frequencies[min(len(frequencies) - 1, i * len(frequencies) // frames)]
        phase += 2 * math.pi * frequency / rate
        fade = min(1.0, i / 200, (frames - 1 - i) / 400)
        samples.append(int(volume * fade * 32767 * math.sin(phase)))
    with wave.open(str(path), "wb") as file:
        file.setnchannels(1)
        file.setsampwidth(2)
        file.setframerate(rate)
        file.writeframes(struct.pack(f"<{frames}h", *samples))


def build(editor: EditorSession) -> None:
    """Creates the Gem Roller scene in the editor's open project and makes it the start scene."""
    editor.call("scene.new", name="GemRoller")
    assets = Path(editor.call("project.info")["assets"])
    write_tone(assets / "Audio" / "Gem.wav", [880.0, 1175.0, 1568.0], 0.18)
    write_tone(assets / "Audio" / "Jump.wav", [330.0, 440.0], 0.10, 0.18)
    write_tone(assets / "Audio" / "Fall.wav", [392.0, 330.0, 262.0, 196.0], 0.45)
    write_tone(assets / "Audio" / "Win.wav", [523.0, 659.0, 784.0, 1047.0, 1319.0], 0.6)
    for sound in ("Gem", "Jump", "Fall", "Win"):
        editor.call("asset.import", path=f"Audio/{sound}.wav")

    def material(name: str, **fields) -> str:
        return editor.call("material.create", path=f"Materials/{name}.pxmat", fields=fields)["handle"]

    ball_material = material("Ball", BaseColor=[0.75, 0.08, 0.05, 1.0], Roughness=0.25, Metallic=0.3)
    gem_material = material("Gem", BaseColor=[0.05, 0.6, 0.9, 1.0], Roughness=0.1, Metallic=0.0,
                            Emissive=[0.1, 0.6, 1.0], EmissiveStrength=2.0)
    platform_material = material("Platform", BaseColor=[0.9, 0.45, 0.05, 1.0], Roughness=0.5, Metallic=0.2)
    goal_material = material("Goal", BaseColor=[0.1, 0.8, 0.2, 1.0], Roughness=0.4, Metallic=0.0,
                             Emissive=[0.2, 1.0, 0.3], EmissiveStrength=1.5)

    scripts = {name: editor.call("script.create", path=f"Scripts/{name}.lua",
                                 source=(HERE / f"{name}.lua").read_text(encoding="utf-8"))["handle"]
               for name in SCRIPTS}

    for name, position, scale in PADS:
        editor.call("entity.create", name=name, components={
            "Transform": {"Translation": position, "Scale": scale},
            "Mesh": {"Mesh": CUBE, "Materials": [GROUND_MATERIAL]},
            "Collider": {"Shape": "Box", "Friction": 0.9}})

    editor.call("entity.create", name="Platform", components={
        "Transform": {"Translation": PLATFORM_CENTRE, "Scale": [3.0, 0.5, 3.0]},
        "Mesh": {"Mesh": CUBE, "Materials": [platform_material]},
        "Collider": {"Shape": "Box", "Friction": 1.0},
        "Rigidbody": {"Type": "Kinematic"},
        "Script": {"Script": scripts["Platform"]}})

    for i, position in enumerate(GEMS):
        gem = editor.call("entity.create", name=f"Gem{i + 1}", components={
            "Tag": {"Tag": "Gem"},
            "Transform": {"Translation": position},
            "Collider": {"Shape": "Sphere", "Size": [0.8, 0.8, 0.8], "IsTrigger": True},
            "Script": {"Script": scripts["Gem"]}})["id"]
        editor.call("entity.create", name=f"Gem{i + 1}Mesh", parent=gem, components={
            "Transform": {"Scale": [0.45, 0.45, 0.45]},
            "Mesh": {"Mesh": CUBE, "Materials": [gem_material], "CastShadows": False}})

    editor.call("entity.create", name="Goal", components={
        "Transform": {"Translation": GOAL, "Scale": [2.0, 2.0, 2.0]},
        "Mesh": {"Mesh": SPHERE, "Materials": [goal_material], "CastShadows": False},
        "Collider": {"Shape": "Sphere", "IsTrigger": True},
        "Script": {"Script": scripts["Goal"]}})

    editor.call("entity.create", name="Ball", components={
        "Transform": {"Translation": [0.0, 1.0, 2.0]},
        "Mesh": {"Mesh": SPHERE, "Materials": [ball_material]},
        "Collider": {"Shape": "Sphere", "Friction": 1.0, "Restitution": 0.2},
        "Rigidbody": {"Mass": 1.0, "LinearDamping": 0.3, "AngularDamping": 0.3},
        "Script": {"Script": scripts["Roller"]}})

    editor.call("entity.create", name="Hud", components={
        "Text": {"Text": "", "Size": 40, "Color": [1.0, 1.0, 1.0, 1.0]},
        "Transform": {"Translation": [0.03, 0.03, 0.0]}})
    editor.call("entity.create", name="Message", components={
        "Text": {"Text": "", "Size": 90, "Alignment": "Center", "Color": [0.3, 1.0, 0.5, 1.0]},
        "Transform": {"Translation": [0.5, 0.4, 0.0]}})
    editor.call("entity.create", name="Camera", components={
        "Camera": {"Primary": True, "FieldOfView": 0.9},
        "AudioListener": {},
        "Transform": {"Translation": [0.0, 7.0, 11.0]},
        "Script": {"Script": scripts["FollowCamera"]}})
    editor.call("entity.create", name="Sun", components={
        "DirectionalLight": {"Intensity": 3.0},
        "Transform": {"Rotation": [-0.42, 0.22, 0.1, 0.87]}})
    editor.call("entity.create", name="Sky", components={"SkyLight": {"Environment": SKY_ENVIRONMENT, "Intensity": 1.0}})

    editor.call("scene.save", path="Scenes/GemRoller.pxscene")
    editor.call("project.set_start_scene", path="Scenes/GemRoller.pxscene")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("editor", help="Path to Paralax-Editor")
    parser.add_argument("project", help="Directory for the new project (must not exist)")
    parser.add_argument("--export", help="Also export a standalone game to this directory")
    args = parser.parse_args()

    with EditorSession.launch(args.editor, ["--headless", "--new-project", args.project]) as editor:
        build(editor)
        if args.export:
            result = editor.call("project.export", output=str(Path(args.export).resolve()))
            print(f"Exported to {result['path']}")
    print(f"Gem Roller project written to {args.project}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
