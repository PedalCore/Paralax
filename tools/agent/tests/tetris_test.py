#!/usr/bin/env python3
"""End-to-end test: build Tetris through editor commands (tools/agent/examples/tetris) and play it with injected input.

This is the "ask an agent to build Tetris" check: everything goes through the commands an agent has. Run by CTest:
tetris_test.py <Paralax-Editor> <output dir>.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "examples" / "tetris"))
from build_tetris import build  # noqa: E402
from paralax_agent import EditorSession  # noqa: E402

FRAME = 1.0 / 60.0


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def lua(editor: EditorSession, code: str) -> str:
    return editor.call("script.eval", code=code)["result"]


def tap(editor: EditorSession, key: str, frames: int = 1) -> None:
    editor.call("input.key", key=key, action="press")
    editor.call("play.step", frames=frames)
    editor.call("input.key", key=key, action="release")
    editor.call("play.step", frames=1)


def no_errors(editor: EditorSession) -> None:
    errors = editor.call("script.errors")["errors"]
    check(errors == [], "the game reported script errors:\n" + "\n".join(errors))


def play(editor: EditorSession) -> list[str]:
    editor.call("play.start", paused=True)
    editor.call("play.step", frames=1)
    no_errors(editor)
    check(lua(editor, "TetrisGame.score, TetrisGame:BlockCount(), #TetrisGame.piece.blocks") == "0\t0\t4", "a fresh game")
    first = lua(editor, "TetrisGame.piece.name")
    pieces = [first]

    # Steering: Left three times (each a separate press), then rotate.
    x = int(lua(editor, "TetrisGame.piece.x"))
    for _ in range(3):
        tap(editor, "Left")
    check(int(lua(editor, "TetrisGame.piece.x")) == max(x - 3, 0), "Left should move the piece one cell per press")
    tap(editor, "Up")
    check(lua(editor, "TetrisGame.piece.rotation") in ("0", "1"), "Up rotates (an O piece looks the same)")

    # Gravity: the piece falls one row per 0.8 s at level 1.
    y = int(lua(editor, "TetrisGame.piece.y"))
    editor.call("play.step", frames=60)
    check(int(lua(editor, "TetrisGame.piece.y")) == y + 1, "gravity should drop the piece one row in a second")

    # A hard drop settles four blocks and brings in the next piece.
    tap(editor, "Space")
    check(lua(editor, "TetrisGame:BlockCount()") == "4", "a hard drop settles the piece's four blocks")
    pieces.append(lua(editor, "TetrisGame.piece.name"))

    # Holding Right moves once, then repeats after a short delay: from the left wall to the right one.
    lua(editor, "TetrisGame:SetPiece('O', 0, 0, 2)")
    editor.call("input.key", key="Right", action="press")
    editor.call("play.step", frames=40)
    editor.call("input.key", key="Right", action="release")
    editor.call("play.step", frames=1)
    check(lua(editor, "TetrisGame.piece.x") == "8", "a held key should carry the piece to the wall")

    # A line clear: fill the bottom row except one gap, then drop a vertical I piece into it.
    lua(editor, "for y = 0, 19 do TetrisGame:SetRow(y, '..........') end")
    lua(editor, "TetrisGame:SetRow(19, 'LLLL.JJJJJ') TetrisGame:SetRow(18, 'SSSS.ZZZZZ')")
    lua(editor, "TetrisGame:SetPiece('I', 1, 2, 0)")  # Vertical I, its column at x = 2 + 2 = 4.
    tap(editor, "Space")
    no_errors(editor)
    check(lua(editor, "TetrisGame.lines, TetrisGame.score") == "2\t300", "two rows cleared for 300 points")
    check(lua(editor, "Scene.FindEntityByName('Hud'):GetComponent('Text').Text") == "Score 300\nLevel 1\nLines 2", "the HUD shows the score")
    check(lua(editor, "TetrisGame:BlockCount()") == "2", "the I piece's other two blocks remain")
    check(lua(editor, "TetrisGame.board[19][4] ~= nil and TetrisGame.board[18][4] ~= nil") == "true",
          "the remaining blocks fall to the bottom")

    # Game over: fill the top so the next piece has no room; the game restarts and remembers the best score.
    # Rows 2-19 are full but for one column (so none clears); the O lands on top and the next piece has no room.
    lua(editor, "for y = 2, 19 do TetrisGame:SetRow(y, 'TTTTTTTTT.') end")
    lua(editor, "TetrisGame:SetPiece('O', 0, 4, 0)")
    tap(editor, "Space")
    check(lua(editor, "TetrisGame.over") == "true", "no room for the next piece ends the game")
    check(lua(editor, "Scene.FindEntityByName('Message'):GetComponent('Text').Text") == "GAME OVER\nBest 300", "the message shows game over")
    editor.call("play.step", frames=130)  # Two seconds, then Scene.Reload.
    no_errors(editor)
    check(lua(editor, "TetrisGame.over, TetrisGame.score, TetrisGame:BlockCount()") == "false\t0\t0", "a fresh game after the restart")
    check(lua(editor, "Scene.FindEntityByName('Message'):GetComponent('Text').Text") == "", "the message is cleared by the restart")
    check(lua(editor, "Game.Get('BestScore'), Game.Get('GamesPlayed')") == "300\t1", "the best score survives the restart")
    editor.call("play.stop")
    return pieces


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    executable, output = Path(sys.argv[1]), Path(sys.argv[2])
    shutil.rmtree(output, ignore_errors=True)
    output.mkdir(parents=True)
    project = output / "Tetris"
    with EditorSession.launch(executable, ["--headless", "--new-project", str(project)]) as editor:
        build(editor)
        first = play(editor)
        check(play(editor) == first, "the piece sequence (seeded math.random) should repeat exactly")
        editor.call("play.start", paused=True)
        editor.call("play.step", frames=30)
        editor.call("render.capture", path="Captures/Tetris.png", width=540, height=960)
        editor.call("play.stop")
        export = editor.call("project.export", output=str(output / "Build"))
    # The exported game runs on its own for a while, falling pieces and all.
    result = subprocess.run([export["executable"], "--headless", "--frames", "600", "--fixed-timestep", str(FRAME), "--fail-on-script-error"],
                            cwd=output, capture_output=True, text=True, timeout=300)
    check(result.returncode == 0, f"the exported game failed ({result.returncode}):\n{result.stdout}\n{result.stderr}")
    print("Tetris test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
