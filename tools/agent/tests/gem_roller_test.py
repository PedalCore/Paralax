#!/usr/bin/env python3
"""End-to-end test: build Gem Roller (tools/agent/examples/gem_roller) through editor commands and play it with
injected input.

The physics counterpart of tetris_test.py: an agent builds a game with rigid bodies, triggers, a kinematic platform,
raycasts and fixed-rate control, then playtests it. Run by CTest: gem_roller_test.py <Paralax-Editor> <output dir>.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "examples" / "gem_roller"))
from build_gem_roller import GEMS, GOAL, build  # noqa: E402
from paralax_agent import EditorSession  # noqa: E402

FRAME = 1.0 / 60.0


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def lua(editor: EditorSession, code: str) -> str:
    return editor.call("script.eval", code=code)["result"]


def numbers(editor: EditorSession, code: str) -> list[float]:
    return [float(value) for value in lua(editor, code).split("\t")]


def ball(editor: EditorSession) -> list[float]:
    return numbers(editor, "local p = RollerGame.entity:GetWorldPosition() return p.x, p.y, p.z")


def no_errors(editor: EditorSession) -> None:
    errors = editor.call("script.errors")["errors"]
    check(errors == [], "the game reported script errors:\n" + "\n".join(errors))


def hold(editor: EditorSession, key: str, frames: int, dt: float = FRAME) -> None:
    editor.call("input.key", key=key, action="press")
    editor.call("play.step", frames=frames, dt=dt)
    editor.call("input.key", key=key, action="release")
    editor.call("play.step", frames=1, dt=dt)


def teleport(editor: EditorSession, position: list[float]) -> None:
    lua(editor, "RollerGame:Teleport(vec3({}, {}, {}))".format(*position))


def roll_forward(editor: EditorSession, frames: int, dt: float) -> list[float]:
    """From a fresh start, holds Up for the given frames; returns where the ball ends up."""
    editor.call("play.start", paused=True)
    editor.call("play.step", frames=30)  # Settle on the start pad.
    editor.call("input.key", key="Up", action="press")
    editor.call("play.step", frames=frames, dt=dt)
    position = ball(editor)  # Measured with the key still down: the same simulated time whatever the frame length.
    editor.call("input.key", key="Up", action="release")
    editor.call("play.stop")
    return position


def play(editor: EditorSession) -> None:
    editor.call("play.start", paused=True)
    editor.call("play.step", frames=30)
    no_errors(editor)
    x, y, z = ball(editor)
    check(abs(y - 0.5) < 0.01 and abs(z - 2.0) < 0.01, f"the ball should rest on the start pad, at ({x}, {y}, {z})")
    check(lua(editor, "RollerGame:OnGround(), RollerGame.gemsLeft, RollerGame.livesLeft") == "true\t5\t3", "a fresh game")

    # Rolling: Up pushes the ball along -z, Right along +x.
    hold(editor, "Up", 40)
    check(ball(editor)[2] < 1.0, "Up should roll the ball forwards")
    hold(editor, "Right", 20)
    check(ball(editor)[0] > 0.2, "Right should roll the ball right")

    # Jumping: only from the ground.
    teleport(editor, [0.0, 0.5, 2.0])
    editor.call("play.step", frames=10)
    hold(editor, "Space", 1)
    editor.call("play.step", frames=10)
    check(ball(editor)[1] > 1.2 and lua(editor, "RollerGame.jumps") == "1", "Space should make the ball jump")
    hold(editor, "Space", 1)  # In the air: no second jump.
    check(lua(editor, "RollerGame.jumps") == "1", "the ball cannot jump in the air")
    editor.call("play.step", frames=90)
    check(abs(ball(editor)[1] - 0.5) < 0.05, "the ball lands again")

    # Gems: rolling into one collects it (a trigger), once.
    gem = GEMS[0]
    teleport(editor, [gem[0], 0.5, gem[2] + 1.5])
    hold(editor, "Up", 45)
    check(lua(editor, "RollerGame.gemsLeft, Scene.FindEntityByName('Gem1') == nil") == "4\ttrue",
          "rolling into a gem should collect it")
    check(lua(editor, "Scene.FindEntityByName('Hud'):GetComponent('Text').Text:sub(1, 8)") == "Gems 1/5", "the HUD counts the gem")

    # The goal does nothing while gems remain.
    teleport(editor, [GOAL[0], 0.5, GOAL[2] + 0.5])
    editor.call("play.step", frames=10)
    check(lua(editor, "RollerGame.won") == "false", "the goal needs every gem")

    # Falling off costs a life and puts the ball back at the start.
    teleport(editor, [10.0, 0.5, 0.0])
    editor.call("play.step", frames=90)
    check(lua(editor, "RollerGame.livesLeft") == "2", "falling off should cost a life")
    x, y, z = ball(editor)
    check(abs(x) < 0.5 and abs(z - 2.0) < 0.5 and y > -1.0, f"the ball should be back at the start, at ({x}, {y}, {z})")

    # The platform moves, and carries the ball resting on it.
    platform = "Scene.FindEntityByName('Platform'):GetWorldPosition()"
    before = numbers(editor, f"return {platform}.z")[0]
    editor.call("play.step", frames=20)
    after = numbers(editor, f"return {platform}.z")[0]
    check(abs(after - before) > 0.1, "the platform should move")
    lua(editor, f"local p = {platform} RollerGame:Teleport(vec3(p.x, p.y + 0.75, p.z))")
    editor.call("play.step", frames=15)
    rider_start, deck_start = ball(editor)[2], numbers(editor, f"return {platform}.z")[0]
    editor.call("play.step", frames=30)
    rider_end, deck_end = ball(editor)[2], numbers(editor, f"return {platform}.z")[0]
    # A rolling ball takes up only part of the deck's acceleration (friction also spins it), so it lags behind the
    # deck; it must still move with it and stay on it.
    carried = f"ball {rider_start} -> {rider_end}, deck {deck_start} -> {deck_end}"
    check(abs(deck_end - deck_start) > 0.3, f"the platform should move under the ball: {carried}")
    check((rider_end - rider_start) * (deck_end - deck_start) > 0, f"the platform should carry the ball along: {carried}")
    check(abs(rider_end - deck_end) < 1.5 and ball(editor)[1] > 0.3, f"the ball should stay on the platform: {carried}")

    # Winning: collect the remaining gems, reach the goal; the best time is kept across the restart.
    for gem in GEMS[1:]:
        teleport(editor, gem)
        editor.call("play.step", frames=3)
    check(lua(editor, "RollerGame.gemsLeft") == "0", "every gem collected")
    teleport(editor, [GOAL[0], 0.5, GOAL[2] + 0.5])
    editor.call("play.step", frames=10)
    check(lua(editor, "RollerGame.won") == "true", "reaching the goal with every gem wins")
    check(lua(editor, "Scene.FindEntityByName('Message'):GetComponent('Text').Text:sub(1, 8)") == "FINISHED", "the message announces the win")
    best = float(lua(editor, "Game.Get('BestTime')"))
    check(best > 0, "the time is recorded")
    editor.call("play.step", frames=190)  # Three seconds, then Scene.Reload.
    no_errors(editor)
    check(lua(editor, "RollerGame.won, RollerGame.gemsLeft, RollerGame.livesLeft") == "false\t5\t3", "a fresh game after the win")
    check(float(lua(editor, "Game.Get('BestTime')")) == best and lua(editor, "Game.Get('Wins')") == "1",
          "the best time survives the restart")
    editor.call("play.stop")


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    executable, output = Path(sys.argv[1]), Path(sys.argv[2])
    shutil.rmtree(output, ignore_errors=True)
    output.mkdir(parents=True)
    project = output / "GemRoller"
    with EditorSession.launch(executable, ["--headless", "--new-project", str(project)]) as editor:
        build(editor)
        play(editor)
        # Control runs at the physics rate (OnFixedUpdate): one second of holding Up gives exactly the same result at
        # 60 and at 30 frames per second, since both are the same 60 physics steps with the same forces.
        at60 = roll_forward(editor, 60, FRAME)
        at30 = roll_forward(editor, 30, 2 * FRAME)
        check(at60[2] < 1.0, "the ball should have rolled forwards")
        check(at60 == at30, f"rolling should not depend on the frame rate: {at60} at 60 fps, {at30} at 30 fps")
        editor.call("play.start", paused=True)
        editor.call("play.step", frames=30)
        editor.call("render.capture", path="Captures/GemRoller.png", width=960, height=540)
        editor.call("play.stop")
        export = editor.call("project.export", output=str(output / "Build"))
    # The exported game runs on its own for a while.
    result = subprocess.run([export["executable"], "--headless", "--frames", "600", "--fixed-timestep", str(FRAME), "--fail-on-script-error"],
                            cwd=output, capture_output=True, text=True, timeout=300)
    check(result.returncode == 0, f"the exported game failed ({result.returncode}):\n{result.stdout}\n{result.stderr}")
    print("Gem Roller test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
