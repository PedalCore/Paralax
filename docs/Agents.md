# Building games with an agent

The Paralax editor can be driven entirely by a program: an AI agent, a test, or a script. Every editor action is a
command with JSON parameters and a JSON result. The same commands back the editor's own UI, its console, the
JSON-RPC agent server and the MCP bridge. This page is the workflow, and its [command reference](#command-reference)
lists every command with its parameters, as `help` does.
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
`<project>/.paralax/AgentServer.json` (owner-readable only), or to the file `--agent-info` names. An editor that
cannot start the server or write that file exits at once with code 1, so a launcher waiting for the file can stop
waiting when the process ends. Send one JSON object per line; the first request must be `session.authenticate` with
the token. `editor.quit` closes the editor.

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
     Each message keeps at most 17 KiB of its text, room for a script's line or error (which keeps 16 KiB, see
     [Scripting.md](Scripting.md)) with what the editor says around it; a longer one is cut there and ends in
     `... (N bytes)`, giving its full size.
   - `render.capture { path }` saves what the scene's primary camera sees as a PNG (a project-relative path; MCP
     returns the image), at 1280 x 720 unless `width` and `height` say otherwise (16 to 8192 each). With
     `camera: "editor"` it renders through the editor camera instead, so `editor.camera.set { position, target }`
     and then `render.capture { path, camera: "editor" }` shows the scene from anywhere without adding or moving a
     scene camera, which would be an edit. `render.stats` says what the renderer drew in its latest frame (draw
     calls, triangles, culled submeshes, lights). The viewport renders every editor frame too, so to read a
     capture's own statistics send `render.capture` and `render.stats` in one JSON-RPC batch, which runs with no
     frame in between.
   - `audio.status` and `input.state` show what is playing and what is held, and `editor.gizmo.get` the viewport's
     gizmo (operation and space, as `editor.gizmo.set` takes them).
5. **Iterate.** `play.stop` returns to the edited scene unchanged. Change scripts with `script.update` (while
   playing, the change waits for the next `play.start`). Change components with `component.set`. When it refuses a
   value, the error names the field and what it expects and shows the value briefly, with its JSON type: a long one
   is cut, ends in `...` and gives its full size. Of many problems (with the components, and within each one), the
   first eight are listed and the rest counted. Undo with `edit.undo` and redo with `edit.redo`; both refuse while
   playing ("Stop play mode first"), since the history belongs to the edited scene, not the running copy. Should
   restoring the scene have to drop a value it cannot hold (an engine bug), the result lists it under `warnings`, and
   a failed command's rollback adds it to the error.
6. **Ship.** `project.export { output }` writes a standalone game folder: the runtime, its resources and the project.
   Run that folder's executable with `--headless --frames N --fail-on-script-error` to check it on its own.

## Experimental diffuse global illumination

Diffuse probe GI is experimental: its approximate distance-moment visibility still leaks in some configurations.
The retained grazing-corner diagnostic measures about +771% excess and the earlier sphere-underside diagnostic
about +26% against exact segment visibility using the same cache. In the authored maintenance-bay review,
the converged sphere underside is about +38.65% high; the bench-floor sample has no physically visible eligible
neighbours, so its exact-cache comparison is zero, not a ground-truth irradiance estimate. These cases need
stronger visibility and probe support; more convergence or AO does not fix them. The grazing diagnostic is
retained separately from baseline CI. Bounded real-geometry visibility is the planned follow-up.

Add an enabled `GIVolume` component to cover the surfaces that need indirect lighting. Its entity's world
translation is the minimum probe centre. The grid is world aligned: rotation and scale do not affect it.
`ProbeSpacing` is in metres (0.1–100), and `ProbeCountX`, `ProbeCountY`, `ProbeCountZ` are integers (2–12).
Coverage extends from the origin to `origin + spacing * (counts - 1)`. Include the room's surfaces within those
bounds, and place enough probes in free space: probes classified inside solid geometry do not contribute.
Classification counts geometric backface hits, independently of material `DoubleSided`; two-sided materials
still shade both sides. This is a winding-based heuristic, not a topology/closed-volume test. If at least 25%
of a probe's rays hit backfaces, it is inactive. A large open sheet can therefore deactivate probes on its
back side even in free space. Use consistently wound solid walls with thickness, or move the probe grid so
receiving surfaces retain valid neighbours; changing `DoubleSided` does not change this geometric limitation.
If several volumes are enabled, the lowest entity UUID wins and the renderer reports a warning.

```python
editor.call("entity.create", name="Room GI", components={
    "Transform": {"Translation": [-5.0, 0.5, -5.0]},
    "GIVolume": {"ProbeSpacing": 2.0, "ProbeCountX": 6, "ProbeCountY": 3, "ProbeCountZ": 6}
})
editor.call("render.settings.set", settings={
    "GIEnabled": True, "GIRaysPerProbe": 64, "GIProbesPerFrame": 32, "GIHysteresis": 0.85
})
editor.call("render.settings.set", settings={"DebugView": "Indirect"})
editor.call("render.capture", path="Captures/Indirect.png", width=1280, height=720, camera="scene")
editor.call("render.settings.set", settings={"DebugView": "Lit"})
```

`render.settings.get` reports the active controls. Rays per probe range from 32 to 256, probes updated per rendered
frame from 1 to 1728, and hysteresis from 0 to 0.99. Higher ray counts reduce noise at greater tracing cost;
higher hysteresis smooths noise but delays changes. Receiver bias controls are described below.
`Indirect` (also named `IndirectDiffuse`) shows the exact composited diffuse GI term, including surface colour,
Fresnel/metallic diffuse response and material occlusion. `GICoverage` shows where a volume replaces environment
diffuse; coverage does not certify validity or lighting quality. `DirectDiffuse`, `DirectSpecular`,
`EnvironmentDiffuse`, `EnvironmentSpecular` and `Emissive` isolate the other terms. These views and GI diagnostics
bypass exposure, tone mapping and dithering; `GIDebugScale` (0.0001–1000, default 1) scales their display.
PNG output still uses sRGB encoding and clips values above 1. HDR readbacks retain the linear values.
`EnvironmentSpecular` is an independent boolean setting (default true); GI replaces environment diffuse only.

The first rendered frame fills the volume. Later frames update probes in a deterministic round-robin order;
multiple diffuse bounces converge over time. Moving static meshes rebuilds and uploads the whole CPU trace scene, including expanded textures and BVH sorting.
That is an expected cost of this experimental implementation; dynamic/kinematic rigid-body hierarchies are excluded.
Geometry/material edits upload new tracing data while preserving
history and the update budget, so occlusion and lighting can remain stale until probes refresh. Volume
placement/count/spacing changes, scene replacement and disabling GI reset history. Camera movement and viewport
resizing preserve it. Simulation
steps and rendered frames are distinct: editor rendering between agent requests can also update GI. Record
warmup conditions when comparing captures; a count of capture requests is not an exact GI frame count.

Each trace buffer is limited by the adapter's maximum storage-buffer range. Triangle records cost 144 bytes,
BVH nodes 48 bytes (at most one per triangle), materials 96 bytes, and expanded texture texels 16 bytes.
The independent hard caps are 1,048,576 triangles/materials and 16,777,216 texels; the adapter usually lowers them.
At the Vulkan baseline 128 MiB range this allows 932,067 triangles and 8,388,608 texels including the white
fallback texel (31 complete 512×512 textures plus the fallback). Material records remain capped at 1,048,576.
The CPU retains accepted arrays for diagnostics; rebuilding and GPU upload add temporary memory costs. Packed
8-bit texels, channel stripping and incremental rebuilds are follow-ups. Invalid or oversized data disables GI
with a named, memoized cause, preserving the last accepted snapshot; unrelated movement does not retry the
same asset failure. Correcting/reloading the asset, removing its use or correcting a failing transform recovers.

For reproducible investigation, keep camera, lights, volume and update settings fixed, stop or pause play mode,
and set `GIFreeze: true`. Freeze preserves initialized history; the first render after a reset still fills every
probe. `render.gi.reset {}` invalidates history and sets the update counter to zero. `render.gi.step {frames: N}`
then renders exactly N updates without advancing simulation, requiring frozen GI and N in 1–1024. These synchronous
commands wait for the GPU and can be slow. A scene/volume must have rendered once before reset or step is available.
`render.capture` also accepts `giWarmupFrames` (0–1024, default 0); a positive count requires frozen GI and performs
exactly that many updates before the final frozen capture. Background renders do not add updates while frozen.

`render.gi.state` returns fixed keys: `enabled`, `supported`, `frozen`, `historyValid`, `updates`, `queued`,
`origin` ([x,y,z]), `spacing`, `counts` ([x,y,z]), and `frame` (renderer frame count, distinct from GI updates).
A reset followed by background rendering can already show `updates: 1`, the initial fill. Compare counters,
not request counts. Freeze is a diagnostic control; changing geometry or lights while frozen leaves stale GI.

`GIVisibility: false` bypasses distance-moment visibility during reconstruction, leaving facing, validity and
trilinear weighting intact; expect light leaking. `GIBounceFeedback: false` disables previous probe irradiance
at traced hits, retaining first-bounce direct, emissive and environment lighting. Reset and converge history
after changing feedback. `GIShowProbes` overlays probe centres after tone mapping: green valid, red inactive,
yellow selected. These are x-ray markers, including probes behind walls, and never modify scene depth.

GI receiver bias uses two explicit units: `GINormalBias` is in metres (default 0.05, range 0–0.05);
`GIViewBias` is a **fraction of probe spacing** (default 0.20, range 0–0.5). The view bias is capped at half the receiver-to-camera distance in a
perspective view, or half the known traced segment for probe-hit feedback. The independent normal offset is not
part of that clamp. Orthographic views use parallel
rays with an origin at infinity, so no finite-distance cap applies. Distance moments use 32×32 tiles with
30×30 interiors, separately from 8×8 irradiance tiles; the angular filter follows the distance resolution.
The distance atlas pair consumes 54 MiB at the 1728-probe limit (15.75 MiB for 504 probes), in addition to
the irradiance atlas and scene tracing data.

`render.gi.inspect {position:[x,y,z], normal:[x,y,z], view:[x,y,z], viewDistance?:metres}` performs a synchronous GPU query using the
same reconstruction as shading. Directions are normalized; view points from the receiver towards the camera.
It stalls the GPU, returns owned records for exactly eight neighbours, and does not advance history. Results
include each neighbour's index, position, validity, distance, stored mean and second moment, raw and clamped
variance, trilinear/facing/visibility weights, raw and normalized weight, and directional irradiance. Normalized weights can sum below one when confidence
is insufficient: reconstruction normally divides by visible support, but a black prior limits amplification
below 5% mean visibility or 0.01% eligible cell support. These dimensionless thresholds prevent all-occluded
probes leaking full brightness and keep vanishing support continuous; they are not additional ambient light. Outside
coverage, zero directions, uninitialized GI and unpaused play are errors. Reset, step and captures with positive
`giWarmupFrames` also reject unpaused play. Ordinary captures, including explicit zero warmup, work during play;
`render.gi.state` is always read-only.
Omit `viewDistance` for an unbounded view ray; otherwise supply a finite value in [0,1000000] to reproduce the perspective clamp. This query does not infer a camera distance from the unit view vector.
Each neighbour reports `distanceSupportMass`, `distanceExpectedMass` (the analytic angular-filter integral), and `distanceConfidence` (their ratio clamped to 0–1). Distance moments and mass are blended together through history and bilinear reconstruction; normalization happens at lookup. Confidence weights eligible neighbour support before visibility normalization, so lack of sampling confidence is distinct from confirmed occlusion. Zero mass invalidates that neighbour.
Each neighbour also includes `surfaceRay` and `biasedRay`: the production BVH tracer tests the segment
from the physical receiver or its GI-biased position to that probe. Both report `origin`, `end`, `length`,
`epsilon`, `hitPosition`, `hitDistance`, `triangle`, `material`, `blocked`, `backface` and `reliable`. Starts are offset
along the supplied normal and ends trimmed by the reported world-space epsilon; coincident endpoints
produce a clear zero-length segment. Other segments too short to trim, or whose normal offset cannot be
represented accurately at the receiver coordinates, have `reliable:false`; they are conservatively rejected,
not classified as clear. The aggregate `surfaceReliable`/`biasedReliable` flags are false if any eligible
neighbour is indeterminate. Interpret those aggregate estimates only when their reliability flag is true. Clear segments use triangle/material ID 4294967295 and their endpoint
as the hit position. IDs index the current GPU trace snapshot, not scene entity IDs. Use geometric normals
to isolate material-normal effects. `surfaceRejected`/`biasedRejected` are RGB irradiance/PI after rejecting
blocked neighbours while retaining moment visibility. `surfaceExact`/`biasedExact` replace moment visibility
with binary segment visibility. All four preserve the original eligible-support normalization, active
probe classification and cached irradiance. These diagnostics do not repair lighting already stored in probes.


`GIWeightSum` shows raw total weight and `GIValidNeighbours` shows active count divided by eight.
Select a probe with `GIDebugProbe` (-1 means none); `GIProbeWeight`, `GIMeanDistance`, `GISecondMoment` and
`GIVariance` show its normalized contribution and stored statistics at affected surfaces. Values are zero
where that probe is not one of the eight neighbours. Distances use metres, second moments and variance m².

This implementation traces opaque/masked geometry through a portable compute BVH, including surfaces outside
the camera view. Dynamic and kinematic rigid bodies and their descendant meshes receive GI but do not occlude or
contribute bounce lighting. Other transform animation still rebuilds the tracing scene; efficient animated
geometry updates are future work. It samples emissive, base-colour and metallic textures, shadows hit lighting, and stores
irradiance and directional distance moments. Diffuse material textures use existing mips up to 512 pixels per
axis when available; alpha-cutout base textures retain full resolution. POM and normal maps remain raster shading
detail, and blended/transmissive surfaces do not participate. Limits are 1,048,576 triangles and 16,777,216 cached
texels. Invalid data or unavailable resources disable GI and report an error; retrying after a repair is supported.
The adapter's storage-buffer range and texture-array layer limits may impose smaller budgets. Adapters with fewer
than six compute storage-buffer descriptors retain normal forward rendering with GI unavailable. Probe sampling
uses explicit bilinear loads, so optional 32-bit-float linear filtering is not required.

At probe hits, GI always tests direct-light occlusion regardless of the **light's** `CastShadows` setting.
A **mesh** with `CastShadows: false` is excluded from those direct-light shadow rays, matching raster shadow
passes. It remains visible to radiance rays, probe classification and distance moments, so an emissive bulb
proxy can emit without blocking its own point light, while remaining physical geometry for GI visibility.

Within the volume, GI replaces environment diffuse. Outside it, the existing environment lighting remains.
An interior with no valid visible probes stays dark. Environment specular is still separate and unoccluded;
this is diffuse GI, not ray-traced reflections, SSAO, SSR or a visibility-buffer renderer. Hardware ray tracing
is not required. Software Vulkan tests establish correctness, not hardware frame-rate performance.

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
  page. (The last folder of `--new-project` names the project, so it must be 1 to 64 characters, each an ASCII letter
  or digit, a space, `_` or `-`; the folders above it may use anything.)
- Commands that edit the scene are undoable, and they are rolled back if they fail partway.
- The asset registry, `Assets/AssetRegistry.json`, lists every imported file under its handle. Commands that register
  a file (`asset.import`, `material.create`, `script.create`, `prefab.create`, `model.create_prefab`,
  `project.set_start_scene`, a scene's first `scene.save`) save it, but never over a file that has changed on disk
  since the editor read or last wrote it (a version-control checkout or merge, another editor), nor over one it could
  not read: malformed (merge-conflict markers, say) or of a newer `Version` than this build reads. A registry with
  entries the editor had to skip (an asset type it does not know, a path outside `Assets/`, a broken parent) is
  read-only too, since saving would drop them for good; the editor warns once with their count. Such a command fails
  (a `scene.save` still writes the scene, and says under `warnings` that it is not registered), the file is left as
  it is, and the log says why (`console.get`). `asset.list` shows both cases beforehand: `readOnly` (the file could
  not be read, or had entries skipped, which `skipped` counts) and `changedOnDisk` (the file is not the one the editor
  last read or wrote; null if it cannot be read). Once the file is as it should be, `asset.reload_registry` reads it
  again (not while playing). A loaded asset whose entry the file dropped, or gave another type, file or parent, is
  unloaded, and its next use loads what the entry now names; the rest stay loaded. The result counts the `assets`
  registered and the entries `skipped`, and `readOnly` stays true while any are. Then the next command saves. A
  reload of a file that still cannot be read fails and leaves nothing registered until one succeeds. A hand-edited
  registry may list one file under several handles: all are kept, and a path names the lowest.

## Command reference

Every command `help` lists, by area. Parameters are shown as `{ name: type }`, with `?` after the optional ones; the
types are JSON's, and `any` takes an entity id as a string or a number (ids and asset handles come back as strings).
Unknown parameters are refused. A command that fails answers with an error saying why (JSON-RPC code -32000).
Commands marked (undoable) edit the scene: each records one undo step and is rolled back whole if it fails. What a
command returns follows the colon; a command with no result returns `{}`. The tests compare this list with the
editor's commands, names and parameters both, so an entry cannot go missing or stale.

### Discovery and the log

- `help`: every command with its description, its parameters (name, type, description, required) and whether it is
  undoable: `{ commands }`.
- `component.types`: the component types, each with its fields (name, type, description, and the range, enum values,
  units or `maxLength` where they apply) and whether it can be removed: `{ components }`.
- `console.get { limit?: integer, minLevel?: string, sinceRevision?: integer }`: the editor log, as described in
  step 4 above: `{ messages, revision, omitted, complete }`, each message `{ revision, level, logger, text, time }`.
- `console.clear`: empties the log: `{ revision }`.
- `editor.quit`: closes the editor at the end of the frame; unsaved changes are lost.

### Scene

- `scene.info`: the open scene (the running copy while playing): `{ name, path, entities, dirty, playing, canUndo,
  canRedo }`.
- `scene.new { name?: string }`: replaces the scene with an empty one (default name "Untitled"), stopping play mode;
  unsaved changes and the undo history are lost.
- `scene.open { path: string }`: opens a `.pxscene` (relative to `Assets/`; an absolute path must lie inside it),
  stopping play mode and clearing the history: `{ entities }`. When it fails, the log says why.
- `scene.save { path?: string }`: saves the scene to `path` (relative to `Assets/` and inside it; `.pxscene` is added
  if missing), which is required the first time: `{ path, warnings? }`. It is refused while playing.
- `scene.list`: every entity in hierarchy order: `{ entities }`, each `{ id, name, tag, parent, children,
  components }` with the component names.
- `scene.find { name?: string, tag?: string }`: the entities whose name and tag are exactly these: `{ ids }`.

### Entities and components

- `entity.create { name?: string, parent?: any, components?: object }` (undoable): creates an entity under `parent`
  (null or left out: at the root) with components and field values, as in `{"Mesh": {"Mesh": "1"}}`, and selects it:
  `{ id }`.
- `entity.get { id: any }`: the entity with every component and its field values: `{ id, name, tag, parent,
  children, components }`.
- `entity.rename { id: any, name: string }` (undoable): renames it; a name cannot be empty.
- `entity.destroy { id: any }` (undoable): destroys it and everything below it.
- `entity.duplicate { id: any }` (undoable): copies it, with everything below it, and selects the copy: `{ id }`.
- `entity.set_parent { id: any, parent?: any }` (undoable): moves it under `parent` (null or left out: to the root),
  keeping its world transform.
- `component.set { id: any, components: object }` (undoable): sets fields, adding components the entity lacks, as in
  `{"Transform": {"Translation": [0, 1, 0]}}`; fields left out keep their values: `{ id }`.
- `component.remove { id: any, component: string }` (undoable): removes a component (`component.types` says which
  can be removed).

### Selection and undo

- `selection.get`: the selected entities, the primary one last: `{ ids }`.
- `selection.set { ids: array }`: selects these entities; unknown ids are ignored: `{ count }`.
- `edit.undo`: undoes the last scene change: `{ undone, warnings? }`, `undone` naming the change. It is refused
  while playing.
- `edit.redo`: redoes the last undone change: `{ redone, warnings? }`. It is refused while playing.

### Assets, materials and prefabs

- `asset.list`: the project's registered assets, each `{ handle, type, path, parent }` (a model's sub-assets name it
  as their `parent`), and the built-in ones, each `{ handle, type, name }`, with whether commands that register files
  would be refused and why (see the asset registry above): `{ assets, builtins, readOnly, skipped, changedOnDisk }`.
  `readOnly` means the file could not be read or had entries skipped, which `skipped` counts; `changedOnDisk` means
  it is not the file the editor last read or wrote (null if it cannot be read).
- `asset.import { path: string }`: registers a file inside `Assets/` (relative to it, or absolute): `{ handle }`.
- `asset.reload_registry`: reads `Assets/AssetRegistry.json` again, replacing the registry in memory, after a
  checkout or merge changed it or once a file that could not be read, or had entries skipped, is fixed (see the
  asset registry above): `{ assets, skipped, readOnly, warnings? }`, the numbers of entries registered and skipped,
  and whether saves stay refused because some were. Assets whose entries it dropped or changed are unloaded. It is
  refused while playing, and fails if the file cannot be read.
- `material.create { path: string, fields?: object }`: writes a new `.pxmat` (failing if the file exists) with these
  fields, as in `{"BaseColor": [1, 0, 0, 1], "Roughness": 0.4}`, and imports it: `{ handle }`.
- `material.set { handle: string, fields: object }`: changes material fields; a material file is saved, while a
  model's sub-material changes in memory only.
- `prefab.create { id: any, path: string }`: saves an entity and everything below it as a new `.pxprefab` (relative
  to `Assets/`): `{ handle, entities }`.
- `prefab.instantiate { prefab: string, parent?: any, position?: array }` (undoable): places a prefab (its handle or
  path) under `parent`, at the local `position` `[x, y, z]`: `{ id }`, the instance's root.
- `model.create_prefab { model: string, path?: string, name?: string }`: makes a prefab of a glTF model's objects
  (see step 1), importing the model first if needed; `path` defaults to `Prefabs/<model name>.pxprefab` and `name`,
  the root entity's, to the model's file name: `{ handle, model, entities, meshes, cameras, lights, animations,
  warnings }`. It is refused while playing.

### Scripts

- `script.create { path: string, source?: string }`: writes a new Lua script (relative to `Assets/`) from `source`,
  or from a template with `OnCreate`, `OnUpdate` and `OnDestroy`, and imports it: `{ handle, path }`.
- `script.get { path: string }`: a script's source as it is on disk (`path` may also be its handle): `{ handle, path,
  source }`.
- `script.update { path: string, source: string }`: replaces a script's source on disk; a running game keeps its code
  until the next `play.start`: `{ handle, appliesNow }`.
- `script.eval { code: string }`: runs Lua statements, or an expression, in the running game and returns the results
  as text (play mode only): `{ result }`.
- `script.errors { clear?: boolean }`: the script errors of the current play session, oldest first, with tracebacks,
  clearing them if asked: `{ errors, playing }`.
- `script.api`: every name scripts can use, as `Table.Function` and `Type:Member`: `{ api }`.

### Play mode, input and audio

- `play.start { paused?: boolean }`: starts playing a copy of the scene; started paused, the game advances only
  through `play.step`: `{ paused }`.
- `play.step { frames?: integer, dt?: number }`: advances the game by `frames` fixed steps (1 to 10000, default 1) of
  `dt` seconds (above 0 and at most 1, default 1/60), each one input frame: `{ frames }`.
- `play.pause`: pauses the game and its sound; it then advances only through `play.step`.
- `play.resume`: lets a paused game advance with the editor's frames again.
- `play.stop`: stops playing, discarding the running copy.
- `input.key { key: string, action: string }`: presses or releases (`action` is `press` or `release`) a key, named as
  in scripts (`A` to `Z`, `D0` to `D9`, `Space`, `Enter`, `Left`, `LeftShift`, `F1`, ...): `{ queued }`, the events
  waiting for the game's next step. This and the next three need a running game.
- `input.mouse_button { button: string, action: string }`: presses or releases `Left`, `Right` or `Middle`:
  `{ queued }`.
- `input.mouse_move { x: number, y: number }`: moves the cursor to window coordinates, from the top-left:
  `{ queued }`.
- `input.scroll { x: number, y: number }`: scrolls the wheel (a positive `y` is away from the user): `{ queued }`.
- `input.release_all`: releases every key and button, at the game's next step while playing and at once otherwise:
  `{ queued }`.
- `input.state`: what is held and where the cursor is: `{ keys, buttons, mouse, queued }`.
- `audio.set_output { output: string }`: `Device` (heard) or `Offline` (silent and deterministic, the default for a
  headless editor), from the next `play.start`: `{ output }`.
- `audio.status`: `{ playing, chosenOutput }`, and while playing also `{ output, voices, masterVolume, paused }`
  (`output` is `Offline` when `Device` had no sound device to use).

### Rendering and the viewport

- `render.capture { path: string, width?: integer, height?: integer, camera?: string, giWarmupFrames?: integer }`: renders the scene (the
  running copy while playing) to a PNG of `width` x `height` pixels (16 to 8192 each, default 1280 x 720). `path`
  must end in `.png`; with a project open it is relative to the project directory and must stay inside it.
  `camera` is `"scene"` (the default: the scene's primary camera, or the editor camera if it has none) or
  `"editor"`, the editor camera wherever `editor.camera.set` or `editor.camera.focus` left it: `{ path, width,
  height }`. MCP returns the image. `giWarmupFrames` (0–1024, default 0) performs exactly that many GI updates
  before capture; a positive count requires frozen GI and paused/stopped simulation.
- `render.stats`: the statistics of the renderer's latest frame: `{ drawCalls, shadowDrawCalls, triangles,
  culledSubmeshes, textGlyphs, lights, staticLightingGenerations }`, `lights` counting point and spot lights. The
  viewport renders every editor frame: send `render.capture` and `render.stats` in one batch to read a capture's.
- `render.settings.get`: the renderer settings: `{ Exposure, Tonemapper, Shadows, ShadowResolution, ShadowDistance,
  ShadowSoftness, ShadowCascadeBlend, SkyIntensity, DebugView }`, plus the GI, diagnostics and
  `EnvironmentSpecular` controls described in the experimental GI section above.
- `render.settings.set { settings: object }`: changes some of them, returning them all; nothing changes if any value
  is refused. The settings are the editor's view, not part of the scene, so they are not undoable.
- `render.gi.state`: the current GI status and update counters, without rendering: `{ enabled, supported, frozen,
  historyValid, updates, queued, origin, spacing, counts, frame }`.
- `render.gi.reset`: discards history and returns GI state; the next render fills the entire volume, even when
  frozen. Requires an enabled supported volume rendered at least once and paused/stopped simulation.
- `render.gi.step { frames: integer }`: performs exactly `frames` (1–1024) GI updates without advancing simulation,
  returning GI state and `before` (the prior update count). Requires frozen, previously rendered GI and
  paused/stopped simulation; waits for the GPU.
- `render.gi.inspect { position: array, normal: array, view: array, viewDistance?: number }`: reads the eight
  contributing neighbours at a world-space receiver, including moments, weights and exact visibility rays.
  The three arrays contain finite `[x,y,z]` values; normal and view must be nonzero. `viewDistance` is metres
  (0–1,000,000); omit it for an unbounded orthographic view. Returns `irradiance`, `surfaceReliable`,
  `biasedReliable`, `surfaceRejected`, `surfaceExact`, `biasedRejected`, `biasedExact`, `weightSum`,
  `activeNeighbours` and `neighbours`. Requires enabled GI with valid history and paused/stopped simulation; synchronous
  GPU readback leaves history unchanged.
- `editor.camera.get`: the editor camera: `{ position, target, verticalFov }`, the field of view in degrees.
- `editor.camera.set { position: array, target: array }`: places the editor camera at `position` (`[x, y, z]` in
  meters) looking at `target`, returning it as `editor.camera.get` does.
- `editor.camera.focus { id: any }`: frames an entity and everything below it in the editor camera, as F does,
  returning the camera as `editor.camera.get` does.
- `editor.gizmo.get`: the viewport gizmo: `{ operation, space }`, the operation `none`, `translate`, `rotate` or
  `scale` and the space `local` or `world`.
- `editor.gizmo.set { operation?: string, space?: string }`: changes either or both, returning the gizmo as
  `editor.gizmo.get` does; nothing changes if either is refused.

To look at the scene from any point, place the editor camera and capture through it:

```python
editor.call("editor.camera.set", position=[6, 4, 8], target=[0, 1, 0])
editor.call("render.capture", path="Captures/Overview.png", camera="editor", width=640, height=360)
```

### Project

- `project.info`: the open project (it fails without one): `{ name, directory, assets, startScene }`.
- `project.set_start_scene { path: string }`: makes a saved scene (relative to `Assets/`) the one the game starts
  with, and saves the project: `{ handle }`.
- `project.export { output: string }`: writes the standalone game folder of step 6: `{ path, executable, files,
  warnings? }`. `output` lies outside the project (a relative one is taken from the project directory), its parent
  must exist, and it must be new, empty, or an earlier export holding nothing else.
