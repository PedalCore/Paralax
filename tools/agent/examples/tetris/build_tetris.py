#!/usr/bin/env python3
"""Builds a Tetris game in Paralax entirely through editor commands, as an agent would.

    python3 tools/agent/examples/tetris/build_tetris.py <Paralax-Editor> <new project dir> [--export <dir>]

The game logic is one Lua script (Tetris.lua, beside this file); everything else (project, materials, the well,
camera, lights, sky, the scene) is created with agent commands. Run the result with Paralax-Runtime --project <dir>,
or open it in the editor. tools/agent/tests/tetris_test.py plays it with injected input.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from paralax_agent import EditorSession  # noqa: E402

HERE = Path(__file__).resolve().parent
CUBE = "1"
SKY_ENVIRONMENT = "18"
PIECE_COLORS = {
    "I": [0.20, 0.80, 0.95, 1.0],
    "O": [0.95, 0.85, 0.20, 1.0],
    "T": [0.65, 0.30, 0.90, 1.0],
    "S": [0.30, 0.85, 0.35, 1.0],
    "Z": [0.90, 0.25, 0.25, 1.0],
    "J": [0.25, 0.40, 0.95, 1.0],
    "L": [0.95, 0.55, 0.15, 1.0],
}


def build(editor: EditorSession) -> None:
    """Creates the Tetris scene in the editor's open project and makes it the start scene."""
    editor.call("scene.new", name="Tetris")

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
