#!/usr/bin/env python3
"""Builds a Tetris game in Paralax entirely through editor commands, as an agent would.

    python3 tools/agent/examples/tetris/build_tetris.py <Paralax-Editor> <new project dir> [--export <dir>]

The game logic is one Lua script (Tetris.lua, beside this file); everything else (project, materials, the well,
camera, lights, sky, the scene) is created with agent commands. The two sound effects are generated here and written
into the project's Assets folder, then imported with asset.import: there is no command for writing binary files. Run the result with Paralax-Runtime --project <dir>,
or open it in the editor. tools/agent/tests/tetris_test.py plays it with injected input.
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
CUBE = "1"
SKY_ENVIRONMENT = "18"
# Linear colours (materials are linear): the classic palette, saturated so it survives lighting and tonemapping.
PIECE_COLORS = {
    "I": [0.02, 0.60, 0.90, 1.0],
    "O": [0.90, 0.70, 0.02, 1.0],
    "T": [0.35, 0.05, 0.75, 1.0],
    "S": [0.05, 0.70, 0.08, 1.0],
    "Z": [0.80, 0.03, 0.03, 1.0],
    "J": [0.03, 0.10, 0.85, 1.0],
    "L": [0.90, 0.30, 0.02, 1.0],
}


def write_tone(path: Path, frequencies: list[float], seconds: float) -> None:
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
        samples.append(int(0.25 * fade * 32767 * math.sin(phase)))
    with wave.open(str(path), "wb") as file:
        file.setnchannels(1)
        file.setsampwidth(2)
        file.setframerate(rate)
        file.writeframes(struct.pack(f"<{frames}h", *samples))


def build(editor: EditorSession) -> None:
    """Creates the Tetris scene in the editor's open project and makes it the start scene."""
    editor.call("scene.new", name="Tetris")
    assets = Path(editor.call("project.info")["assets"])
    write_tone(assets / "Audio" / "Drop.wav", [220.0, 165.0], 0.08)
    write_tone(assets / "Audio" / "Clear.wav", [523.0, 659.0, 784.0, 1047.0], 0.35)
    for sound in ("Drop", "Clear"):
        editor.call("asset.import", path=f"Audio/{sound}.wav")

    materials = {}
    for name, color in PIECE_COLORS.items():
        materials[name] = editor.call("material.create", path=f"Materials/Piece{name}.pxmat",
                                      fields={"BaseColor": color, "Roughness": 0.35, "Metallic": 0.1})["handle"]
    frame = editor.call("material.create", path="Materials/Frame.pxmat",
                        fields={"BaseColor": [0.12, 0.13, 0.16, 1.0], "Roughness": 0.6, "Metallic": 0.6})["handle"]

    lua_materials = "{ " + ", ".join(f'{name} = "{handle}"' for name, handle in materials.items()) + " }"
    source = (HERE / "Tetris.lua").read_text(encoding="utf-8").replace("local Materials = MATERIALS",
                                                                      f"local Materials = {lua_materials}", 1)
    script = editor.call("script.create", path="Scripts/Tetris.lua", source=source)["handle"]

    # The well: a floor and two walls around the 10 x 20 board (cells are 1 m cubes at x 0-9, y 0-19).
    for name, position, scale in (("Floor", [4.5, -1.0, 0.0], [12.0, 1.0, 1.0]),
                                  ("LeftWall", [-1.0, 9.5, 0.0], [1.0, 20.0, 1.0]),
                                  ("RightWall", [10.0, 9.5, 0.0], [1.0, 20.0, 1.0])):
        editor.call("entity.create", name=name, components={
            "Transform": {"Translation": position, "Scale": scale},
            "Mesh": {"Mesh": CUBE, "Materials": [frame]}})
    editor.call("entity.create", name="Backdrop", components={
        "Transform": {"Translation": [4.5, 9.5, -0.8], "Scale": [10.0, 20.0, 0.2]},
        "Mesh": {"Mesh": CUBE, "Materials": [frame], "CastShadows": False}})

    editor.call("entity.create", name="Camera", components={
        "Camera": {"Primary": True, "FieldOfView": 0.8},
        "AudioListener": {},
        "Transform": {"Translation": [4.5, 9.5, 30.0]}})
    editor.call("entity.create", name="Sun", components={
        "DirectionalLight": {"Intensity": 3.0},
        "Transform": {"Rotation": [-0.26, 0.17, 0.05, 0.95]}})
    editor.call("entity.create", name="Sky", components={"SkyLight": {"Environment": SKY_ENVIRONMENT, "Intensity": 0.8}})
    editor.call("entity.create", name="Game", components={"Script": {"Script": script}})

    editor.call("scene.save", path="Scenes/Tetris.pxscene")
    editor.call("project.set_start_scene", path="Scenes/Tetris.pxscene")


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
    print(f"Tetris project written to {args.project}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
