# Building games with an agent

The Paralax editor can be driven entirely by a program: an AI agent, a test, or a script. Every editor action is a
command with JSON parameters and a JSON result. The same commands back the editor's own UI, its console, the
JSON-RPC agent server and the MCP bridge. This page is the workflow; `help` lists every command with its parameters.
`tools/agent/examples/tetris/` is a complete game built this way: `build_tetris.py` creates the project, materials,
scene and script through commands, and `tools/agent/tests/tetris_test.py` plays it with injected keys (moves,
rotations, drops, line clears, game over and restart). `tools/agent/examples/gem_roller/` is its physics
counterpart: a rolling-ball game with rigid bodies, gem triggers, a kinematic moving platform, a raycast ground check
and forces applied in `OnFixedUpdate`; `tools/agent/tests/gem_roller_test.py` rolls, jumps, falls off, rides the
platform and wins, and checks that the ball moves identically at 60 and 30 frames per second.
`tools/agent/tests/agent_game_test.py` is a smaller example that also iterates on a script and exports.

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

1. **Set up.** Start with `--new-project <dir>` (or `--project`). When the editor opens no scene file at start (no
   `--scene` and no start scene, or one that cannot be opened), it shows a demo scene; that is the starting point,
   not an edit, so `scene.info` reports it clean with nothing to undo. A scene that opens is shown as it is, even
   empty. Use `scene.new`, then `entity.create` with a `components` object. Components and their fields are listed by
   `component.types`, and documented for scripts in [Scripting.md](Scripting.md). Asset handles are strings (built-ins: `"1"` cube, `"2"` sphere, `"3"` plane,
   `"4"` cylinder, `"5"` capsule; see `asset.list`). Make materials with `material.create`, prefabs with
   `prefab.create`, and import files placed under `Assets/` with `asset.import`. Show a score or a message with a
   `Text` component (screen space by default: position it with the Transform's X/Y as fractions of the viewport),
   and change it from a script (`entity:GetComponent("Text").Text = "Score 3"`).

   **Models from Blender (or any glTF tool).** Export glTF 2.0 (`.glb`, or `.gltf` with its `.bin` and textures
   beside it) into `Assets/`. `asset.import` makes it one mesh: put it on an entity's `Mesh` to show the whole file.
   `model.create_prefab { model: "Models/Bay.glb" }` (importing it if needed) instead makes a prefab with one entity
   per Blender object, nested as in Blender, each with its name, local transform and mesh, plus its camera (never
   primary) or light; `prefab.instantiate` places it. Blender actions (glTF animations of object location, rotation
   and scale) come too, each as an `Animation` component, playing and looping, on the object it animates (or the
   nearest object above all the ones it animates); start, stop or seek them through the component's `Playing`,
   `Loop`, `Speed` and `Time`. The result reports counts and any warnings (skins and shape keys are not imported;
   malformed light data is skipped). It is refused during play mode. Parallax occlusion
   mapping, which glTF cannot express, comes from material extras as a Blender export script can write them
   (`"pom": true`, `"height_map": "stone_height.png"`, `"height_depth_m"`, `"texture_repeat_m"`; the height map is
   found beside the model or in its `textures` folder, and 16-bit maps keep their precision). An object's
   `cast_shadow` custom property (node `extras.cast_shadow`, `true` or `false`; export with Include > Custom
   Properties ticked) sets its entity's `Mesh` `CastShadows` when the prefab is made, so a bulb mesh around a point
   light can stop shadowing it. Light strengths come through as in Blender with its default "Standard" lighting mode
   (sun strength 3 becomes Intensity 3; a 1000 W point light about 80), though the renderer's lighting is not
   physically calibrated, so check them with `render.capture`. Re-exporting to the same file updates the meshes in
   place (they are matched by mesh name, so reordering objects is fine); regenerate the prefab, to a new path, when
   objects are added or removed or their `cast_shadow` changes.
2. **Script.** `script.create { path, source }` writes a Lua script, and `script.update` and `script.get` change and
   read it. Attach a script with a `Script` component. A script returns a table with `OnCreate`, `OnUpdate`,
   `OnFixedUpdate` (physics control, at the physics rate), `OnDestroy` and collision callbacks; see
   [Scripting.md](Scripting.md) and `script.api`.
3. **Save.** `scene.save { path }`, then `project.set_start_scene`. Check the result's `warnings`: a scene saved while
   the asset registry cannot be written is not registered, so it cannot be the start scene yet.
4. **Playtest deterministically.**
   - `play.start { paused: true }`, then `play.step { frames, dt }` advances exactly that many fixed steps.
   - `input.key { key: "Space", action: "press" }` (and `input.mouse_button`, `input.mouse_move`, `input.scroll`)
     injects input. It is delivered at the game's next step, so nothing is lost however long you take between
     requests, and each step is one input frame: a press shows as pressed once and as held until released.
   - `script.eval { code }` reads anything from the running game, for example `Score` or
     `Scene.FindEntityByName('Ball'):GetWorldPosition().y`.
   - `script.errors` lists script failures; check it after every run.
   - `console.get { limit?, minLevel?, sinceRevision? }` reads the editor log, as the Console panel shows it. A
     command that fails with "see the log" explains itself there: note `revision` from one call, run the command,
     then `console.get { sinceRevision, minLevel: "error" }` returns only what it logged. `minLevel` is `trace`,
     `debug`, `info` (the default), `warning`, `error` or `critical`; the most recent `limit` messages (default 100)
     come back oldest first, and `omitted` counts older ones left out. `complete` is false when some of what was
     logged after `sinceRevision` is gone: the log keeps its latest 2000 messages, and `console.clear` empties it.
   - `render.capture { path }` saves what the camera sees (a project-relative path; MCP returns the image).
   - `audio.status` and `input.state` show what is playing and what is held.
5. **Iterate.** `play.stop` returns to the edited scene unchanged. Change scripts with `script.update` (while
   playing, the change waits for the next `play.start`). Change components with `component.set`. Undo with
   `edit.undo` and redo with `edit.redo`; both refuse while playing ("Stop play mode first"), since the history
   belongs to the edited scene, not the running copy. Should restoring the scene have to drop a value it cannot
   hold (an engine bug), the result lists it under `warnings`, and a failed command's rollback adds it to the error.
6. **Ship.** `project.export { output }` writes a standalone game folder: the runtime, its resources and the project.
   Run that folder's executable with `--headless --frames N --fail-on-script-error` to check it on its own.

## Rules that keep runs reproducible

- Prefer `play.start { paused: true }` and `play.step` over letting the game run with the editor's frames. Simulation
  (scripts, physics, the time sound plays for, the ids of entities created while playing) is deterministic for a given
  sequence of steps and commands.
- Scripts have no file, OS or wall-clock access. Randomness comes from the seeded `math.random`, and time from `Time`.
- Never hold an entity id from play mode across `play.stop`: play mode runs on a copy, and `Scene.Load`/`Scene.Reload`
  in a game replace the scene. Look entities up again (`scene.find`) after either. An id held over does not fail
  safely: every run hands out the same ids in the same order, so an id from an entity spawned in one run names
  whatever the next run spawned in its place.
- Paths in commands are untrusted. They resolve inside the project's `Assets/` folder (or the project, for outputs);
  `project.export` is the one command that writes outside it, and it only replaces an empty folder or its own earlier
  export. Paths in parameters and results are UTF-8 strings separated by `/` on every platform, as in project files.
  Only files whose names are valid UTF-8 can be imported or saved into a project. Paths on the editor's and the
  runtime's command lines (`--project`, `--new-project`, `--scene`, `--export`, `--agent-info`, `--screenshot`) may
  use any Unicode characters, on Windows too: there the engine reads the Unicode command line, not the system code
  page. (The last folder of `--new-project` names the project, so it takes only letters, digits, spaces, `_` and
  `-`; the folders above it may use anything.)
- Commands that edit the scene are undoable, and they are rolled back if they fail partway.
