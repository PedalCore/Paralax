# Building games with an agent

The Paralax editor can be driven entirely by a program: an AI agent, a test, or a script. Every editor action is a
command with JSON parameters and a JSON result. The same commands back the editor's own UI, its console, the
JSON-RPC agent server and the MCP bridge. This page is the workflow; `help` lists every command with its parameters.
`tools/agent/examples/tetris/` is a complete game built this way: `build_tetris.py` creates the project, materials,
scene and script through commands, and `tools/agent/tests/tetris_test.py` plays it with injected keys (moves,
rotations, drops, line clears, game over and restart). `tools/agent/tests/agent_game_test.py` is a smaller example
that also iterates on a script and exports.

## Connecting

**MCP** (for agents such as Claude): run the bridge as an MCP server. Every command becomes a tool (`entity.create`
becomes `entity_create`), and `render_capture` returns the image so the agent can see what it built.

```bash
python3 tools/agent/paralax_mcp.py --launch build/release/bin/Paralax-Editor -- --headless --new-project MyGame
python3 tools/agent/paralax_mcp.py --launch build/release/bin/Paralax-Editor -- --project MyGame
python3 tools/agent/paralax_mcp.py --info MyGame/.paralax/AgentServer.json   # attach to a running editor
```

**Python** (standard library only):

```python
from paralax_agent import EditorSession
with EditorSession.launch("build/release/bin/Paralax-Editor", ["--headless", "--project", "MyGame"]) as editor:
    player = editor.call("entity.create", name="Player")["id"]
```

**Raw JSON-RPC 2.0**: `Paralax-Editor --agent-port 0` listens on 127.0.0.1 and writes the port and a token to
`<project>/.paralax/AgentServer.json` (owner-readable only). Send one JSON object per line; the first request must be
`session.authenticate` with the token. `editor.quit` closes the editor.

`--headless` needs no display. A headless editor mixes audio silently, and its play mode behaves exactly like a
windowed one.

## The loop

1. **Set up.** Start with `--new-project <dir>` (or `--project`). Use `scene.new`, then `entity.create` with a
   `components` object. Components and their fields are listed by `component.types`, and documented for scripts in
   [Scripting.md](Scripting.md). Asset handles are strings (built-ins: `"1"` cube, `"2"` sphere, `"3"` plane,
   `"4"` cylinder, `"5"` capsule; see `asset.list`). Make materials with `material.create`, prefabs with
   `prefab.create`, and import files placed under `Assets/` with `asset.import`.
2. **Script.** `script.create { path, source }` writes a Lua script, and `script.update` and `script.get` change and
   read it. Attach a script with a `Script` component. A script returns a table with `OnCreate`, `OnUpdate`,
   `OnFixedUpdate` (physics control, at the physics rate), `OnDestroy` and collision callbacks; see
   [Scripting.md](Scripting.md) and `script.api`.
3. **Save.** `scene.save { path }`, then `project.set_start_scene`.
4. **Playtest deterministically.**
   - `play.start { paused: true }`, then `play.step { frames, dt }` advances exactly that many fixed steps.
   - `input.key { key: "Space", action: "press" }` (and `input.mouse_button`, `input.mouse_move`, `input.scroll`)
     injects input. It is delivered at the game's next step, so nothing is lost however long you take between
     requests, and each step is one input frame: a press shows as pressed once and as held until released.
   - `script.eval { code }` reads anything from the running game, for example `Score` or
     `Scene.FindEntityByName('Ball'):GetWorldPosition().y`.
   - `script.errors` lists script failures; check it after every run.
   - `render.capture { path }` saves what the camera sees (a project-relative path; MCP returns the image).
   - `audio.status` and `input.state` show what is playing and what is held.
5. **Iterate.** `play.stop` returns to the edited scene unchanged. Change scripts with `script.update` (while
   playing, the change waits for the next `play.start`). Change components with `component.set`. Undo with
   `edit.undo`.
6. **Ship.** `project.export { output }` writes a standalone game folder: the runtime, its resources and the project.
   Run that folder's executable with `--headless --frames N --fail-on-script-error` to check it on its own.

## Rules that keep runs reproducible

- Prefer `play.start { paused: true }` and `play.step` over letting the game run with the editor's frames. Simulation
  (scripts, physics, the time sound plays for) is deterministic for a given sequence of steps.
- Scripts have no file, OS or wall-clock access. Randomness comes from the seeded `math.random`, and time from `Time`.
- Never hold an entity id from play mode across `play.stop`: play mode runs on a copy, and `Scene.Load`/`Scene.Reload`
  in a game replace the scene. Look entities up again (`scene.find`) after either.
- Paths in commands are untrusted. They resolve inside the project's `Assets/` folder (or the project, for outputs);
  `project.export` is the one command that writes outside it, and it only replaces an empty folder or its own earlier
  export.
- Commands that edit the scene are undoable, and they are rolled back if they fail partway.
