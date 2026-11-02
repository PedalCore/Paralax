# Paralax scripting (Lua)

Gameplay code is written in Lua 5.4. You attach a script to an entity with a **Script** component, and it runs
while the scene is playing, in the editor's play mode or in an exported game. The script engine is
`Paralax/src/Paralax/Scripting/ScriptSystem.{h,cpp}`. The test file
`Paralax-Tests/src/Scripting/ScriptSystemTests.cpp` shows every feature below in use.

## A script

A script is a Lua chunk that **returns a table**, which acts as its class. Each entity using the script gets its own
instance (a table whose metatable indexes the class), and `self.entity` is set on it.

```lua
local Player = { speed = 4 }          -- Fields on the class are defaults for every instance.

function Player:OnCreate()            -- Before the first update (entities created while playing start next update).
    self.jumps = 0
end

function Player:OnUpdate(dt)          -- Every update; dt in seconds.
    local transform = self.entity:GetComponent("Transform")
    if Input.IsKeyDown(Key.W) then
        transform.Translation = transform.Translation + vec3(0, 0, -self.speed * dt)
    end
end

function Player:OnFixedUpdate(dt)     -- Before each physics step; dt is the step (1/60 s in play mode).
    if Input.IsKeyDown(Key.Space) then
        Physics.AddForce(self.entity, vec3(0, 30, 0))
    end
end

function Player:OnDestroy()           -- When the entity or its Script component goes away, or play mode stops.
    Log.Info(self.entity.name, "destroyed")
end

return Player
```

The callbacks are optional, and may be inherited (an instance looks them up through its class, including the class's
own metatable). Within each phase (create, then update) instances run in hierarchy order. When a Script component is
removed or changed, its instance gets `OnDestroy` at the start of the next update, also in hierarchy order.

`OnFixedUpdate(self, dt)` runs at the physics rate rather than the frame rate: after every script's `OnUpdate`, once
before each physics step, so zero, one or several times per update (`dt` is the fixed step, 1/60 s in play mode and
exported games). Put physics control there (forces, velocities, moving kinematic platforms) to make it independent of
the frame rate:
- it sees the result of the previous step: transforms of dynamic bodies are up to date each time;
- what it changes counts in the coming step: a moved body starts the step there, a kinematic body moved by it arrives
  by the end of that step, and a force added by it acts for that step only;
- an entity destroyed during the update (by any script) gets no further `OnFixedUpdate` and leaves the simulation at
  once, though it is only removed from the scene when the update ends; an entity created in it starts (`OnCreate`)
  next update.

Whether a script has `OnFixedUpdate` is looked up once its `OnCreate` has run (so `OnCreate` may still add one). It
costs nothing when no script has one; when some do, the physics syncs the scene around every step, which costs a pass
over the scene per step. Without physics in the scene (it is always there in play mode and exported games)
`OnFixedUpdate` does not run.

All scripts share one global environment per play session: a global set by one script (`Score = 0`) is visible to
the others. Prefer fields on `self` or on a module table for state that belongs to one script.

If a callback raises an error, the error and its traceback are logged and recorded (see `script.errors`), and that
instance is disabled. Other scripts keep running.

## Entities

| Member | Description |
|---|---|
| `entity.id` | UUID as a string: stable for the entity's lifetime, and saved in scenes. Entities created while playing get ids that are the same in every run with the same inputs (see [Determinism](#determinism)). |
| `entity.name`, `entity.tag` | Read/write. Names cannot be empty; both are limited to 256 bytes. |
| `entity:IsValid()` | False once the entity has been destroyed. Using a destroyed entity raises an error. |
| `entity:GetComponent(name)` | A component reference, or `nil` if the entity doesn't have it. |
| `entity:HasComponent(name)` | Boolean. |
| `entity:AddComponent(name [, fields])` | Adds the component (or returns the existing one) and sets the given fields. All or nothing: an invalid field changes nothing. |
| `entity:RemoveComponent(name)` | Returns true if it was present. Core components (Tag, Transform) cannot be removed. |
| `entity:Destroy()` | Destroys the entity and its children at the end of the current update (immediately when called outside an update, e.g. from `script.eval`). Their `OnDestroy` callbacks run first. |
| `entity:GetParent()`, `entity:SetParent(parent or nil [, keepWorldTransform = true])` | Hierarchy. |
| `entity:GetChildren()` | Array of entities, in order. |
| `entity:GetWorldPosition()` | `vec3`. |
| `entity:GetScript()` | The entity's script instance (to call another script's functions), or `nil`. |

## Components

Component and field names are the same as in scene files, the inspector and the agent API (`component.types`
lists them all). Read and assign fields directly:

```lua
local light = self.entity:AddComponent("PointLight", { Intensity = 5, Color = vec3(1, 0.6, 0.3) })
light.Range = 12
local t = self.entity:GetComponent("Transform")
t.Rotation = quat.FromEuler(vec3(0, math.pi / 2, 0))
```

Field values are **copies**. `t.Translation.x = 1` changes only the copy, so assign the whole value instead:
`t.Translation = vec3(1, 0, 0)`. Values are validated exactly as in scene files, for type, range and enum names.

| Field type | Lua value |
|---|---|
| bool, int, float | boolean, integer, number (angles in radians) |
| vec2/vec3/vec4, quat | `vec2`/`vec3`/`vec4`/`quat`, or an array table `{x, y, z}` when assigning |
| string, enum | string (enums by name, e.g. `"Perspective"`) |
| asset | handle as a string (`nil` clears it) |
| asset array | array of handle strings |

Asset paths (in `Scene.Instantiate`, `Scene.Load`, `Audio.PlayOneShot` and `require`) are relative to the project's
`Assets/` folder, separated by `/`, and UTF-8 on every platform, like the paths in project files.

## Scene

| Function | Description |
|---|---|
| `Scene.CreateEntity([name [, parent]])` | New entity (with Tag and Transform). |
| `Scene.GetEntity(id)` | Entity by id, or `nil`. |
| `Scene.FindEntityByName(name)` | First match in hierarchy order, or `nil`. |
| `Scene.FindEntitiesByTag(tag)` | Array of entities. |
| `Scene.Instantiate(prefab [, { parent = e, position = vec3 }])` | Spawns a prefab, given by handle or by asset path (e.g. `"Prefabs/Enemy.pxprefab"`), and returns its root entity. A `position` that is not finite is an error. |
| `Scene.Destroy(entity)` | Same as `entity:Destroy()`. |
| `Scene.GetEntityCount()` | Number of entities. |
| `Scene.Load(scene)` | Switches to another scene, given by handle or asset path (e.g. `"Scenes/Level2.pxscene"`), after the current update: this scene stops (its scripts get `OnDestroy`) and the new one starts fresh. An unknown scene is an error. |
| `Scene.Reload()` | Restarts the current scene from the state it started in, after the current update (e.g. after "Game Over"). |

## Game

Values that survive `Scene.Load` and `Scene.Reload` for the whole run: a score, the level reached, settings. Each
scene starts with fresh scripts, so this is the way to carry state from one to the next.

| Function | Description |
|---|---|
| `Game.Set(key, value)` | Stores a boolean, number or string under a key (1 to 256 bytes); `nil` removes it. Integers stay integers. Strings up to 64 KiB; at most 1024 keys and 1 MiB in all. Anything else is an error. |
| `Game.Get(key [, default])` | The stored value, or `default` (or `nil`) if there is none. |

A switch requested from `OnDestroy` while a scene is already stopping (during a switch, or when the game ends) is
ignored. In the editor, `Scene.Load` loads the scene's saved file, not unsaved edits; a scene that cannot be loaded
when the switch happens is reported like a script error, and the game stays where it is.

## Physics

Add a **Collider** (shape: `Box`, `Sphere`, `Capsule` or `Cylinder`, sized by `Size` and the Transform's scale) to make
an entity solid. Add a **Rigidbody** as well to simulate it:
- `Dynamic` bodies fall, collide and are moved by forces.
- `Kinematic` bodies follow their Transform, and push dynamic ones.
- `Static` bodies (and colliders without a Rigidbody) never move.

A collider with `IsTrigger = true` detects overlaps without colliding. The simulation runs in fixed 1/60 s steps and is
deterministic. An entity destroyed during an update leaves the simulation at once (it no longer collides in the
update's remaining steps). Setting a body's Transform from a script teleports it (kinematic bodies move there smoothly).

A collider on a child entity without a Rigidbody of its own is part of the body of its nearest ancestor with a
Rigidbody: build a car from a body and wheel children, and it moves as one. Such a body takes its friction and
bounciness from its first collider, and collisions, raycasts and the `Physics` functions refer to the entity with the
Rigidbody. Triggers always stay separate (a trigger child of a moving body follows it, sensing what it passes). An entity whose world scale is zero
(for example, under a parent scaled to 0) has no body until its scale is restored. Changing a Collider or Rigidbody
field while playing rebuilds the body; a dynamic body keeps its velocity.

| Function | Description |
|---|---|
| `Physics.AddForce(entity, vec3)` | Pushes a dynamic body with a force (N) for the current frame; call it every frame for a steady push. The result does not depend on the frame rate. Called from `OnFixedUpdate`, the force acts for the coming step. Returns false if the entity has no dynamic body. |
| `Physics.AddImpulse(entity, vec3)` | Changes a dynamic body's momentum at once (impulse in N·s). Returns false if the entity has no dynamic body. |
| `Physics.SetLinearVelocity(entity, vec3)` / `Physics.GetLinearVelocity(entity)` | Velocity in m/s. `Set` works on dynamic bodies only (it returns false otherwise; move kinematic bodies with their Transform). `Get` works on any body and returns `nil` without one. |
| `Physics.SetAngularVelocity(entity, vec3)` / `Physics.GetAngularVelocity(entity)` | Angular velocity in rad/s, with the same rules. |
| `Physics.Raycast(origin, direction, maxDistance)` | The nearest solid (non-trigger) collider along the ray, as `{ entity, point, normal, distance }`, or `nil`. |
| `Physics.GetGravity()` / `Physics.SetGravity(vec3)` | Gravity in m/s² (default `vec3(0, -9.81, 0)`). |

Scripts on either entity of a contact receive callbacks before their next `OnUpdate`:
`OnCollisionEnter(self, other)` and `OnCollisionExit(self, other)` for solid contacts, and `OnTriggerEnter(self, other)`
and `OnTriggerExit(self, other)` when a trigger is involved. Bodies at rest stay in contact; no exit is reported when
they fall asleep. `other` may already be destroyed in an exit (check `other:IsValid()`).

## Animation

An **Animation** component plays a clip: keyframed movement of the entity and the entities below it, from a glTF
model (Blender actions come through `model.create_prefab`, already on the right entities). Its fields:
- `Clip`: the animation clip asset.
- `Playing`: whether it advances. A clip that does not loop clears it when it reaches its end (or its start, playing
  backwards).
- `Loop` (default true) and `Speed` (default 1; negative plays backwards).
- `Time`: seconds into the clip. Set it to seek; while paused, the entities are posed at the new time (wrapped into
  the clip if it loops, as playing would).

A clip that does not loop plays from `Time` to its end (or, with a negative `Speed`, back to 0) and stops there. To
play it again, set `Time` back and `Playing` to true; to play it backwards from the end, start with `Time` at the
clip's length (any large value is clamped to it).

```lua
local anim = Scene.FindEntityByName("Door"):GetComponent("Animation")
anim.Loop = false
anim.Time = 0
anim.Playing = true   -- Open once.
```

Clips run after every script's `OnUpdate` and before physics, so a script's changes to the component apply in the
same update, and animated kinematic bodies push what they touch. Playback follows simulated time only, so it is
deterministic. The clip sets local Translation, Rotation and Scale; a target that is missing is skipped (and found
when it appears).

## Text

A **Text** component draws text, in the engine's font, over the finished image, so exposure never dims it. Its
fields:
- `Text`: UTF-8, up to 4096 bytes; newlines start new lines. ASCII and Latin-1 characters have glyphs, anything else
  shows as a box.
- `ScreenSpace` (default true): a HUD. The entity's X and Y are the anchor in normalised viewport coordinates,
  (0, 0) top-left to (1, 1) bottom-right, and `Size` is the line height in pixels at a 1080-pixel-high viewport,
  scaled with the real one, so the layout holds at any resolution. Off: the text lies in the entity's local XY plane
  facing +Z, `Size` is the line height in metres, and it is hidden by things in front of it like any object.
- `Alignment`: `"Left"`, `"Center"` or `"Right"`, the side of the text the anchor holds.
- `Color`: linear RGBA.

```lua
function Hud:OnCreate()
	self.score = Scene.FindEntityByName("Score"):GetComponent("Text")
end
function Hud:OnUpdate(dt)
	self.score.Text = "Score " .. Game.Get("Score", 0) .. "\nLives " .. self.lives
end
```

## Audio

Add an **AudioSource** to play a clip (a `.wav`, `.flac` or `.mp3` asset) from an entity:
- `Volume` (linear gain) and `Pitch` (playback speed; 2 is an octave up).
- `Loop`, and `PlayOnStart`: start when the scene starts, or when the source is created while it runs.
- `Spatial`: heard from the entity's position, fading between `MinDistance` and `MaxDistance` from the listener and
  panned to its side. A non-spatial source (music, interface sounds) plays as is.

The scene is heard from the first active **AudioListener**, or from the primary camera if there is none. When a sound
ends follows the game's simulated time (scaled by pitch), so `Audio.IsPlaying` gives the same answers in a headless run
(CI, agents), which mixes sound silently, as in one you can hear. Changing a playing or paused source's `Clip` carries
on with the new clip from its start; pausing play mode in the editor pauses sound too.

| Function | Description |
|---|---|
| `Audio.Play(entity)` | Starts the entity's source from the beginning, or resumes it if paused. Returns false if the entity has no AudioSource with a playable clip, or too many sounds are playing. Controlling a source before its first frame stops `PlayOnStart` from starting it again. |
| `Audio.Pause(entity)` / `Audio.Stop(entity)` | Pauses (keeping the position) or stops (rewinding) the source. Return false without a source. |
| `Audio.IsPlaying(entity)` | Whether the source is playing (false once a non-looping clip has ended). |
| `Audio.PlayOneShot(clip [, position [, volume]])` | Plays a clip once, given by handle or asset path (e.g. `"Audio/Hit.wav"`): at a world position (`vec3`), or as is without one. Returns false if the clip cannot be decoded or too many sounds are playing (64 by default); a path or handle that is not an audio clip, a non-vec3 position or a negative volume is an error. |
| `Audio.GetMasterVolume()` / `Audio.SetMasterVolume(volume)` | Overall gain, 0 to 10 (default 1). |

## Input, time and logging

- `Input.IsKeyDown(Key.W)`, `Input.WasKeyPressed(key)`, `Input.WasKeyReleased(key)`. "Pressed" and "Released" mean
  the change happened this frame.
- `Input.IsMouseButtonDown(Mouse.Left)`, `Input.WasMouseButtonPressed(button)`, `Input.WasMouseButtonReleased(button)`.
  Buttons are `Mouse.Left`, `Mouse.Right` and `Mouse.Middle`.
- `Input.GetMousePosition()`, `Input.GetMouseScrollDelta()` return a `vec2`.
- Key names: `Key.A`–`Key.Z`, `Key.D0`–`Key.D9`, `Key.Space`, `Key.Enter`, `Key.Escape`, `Key.Left`/`Right`/`Up`/`Down`,
  `Key.LeftShift`, `Key.LeftControl`, `Key.F1`–`Key.F12`, `Key.KP0`–`Key.KP9`, and more (see `s_KeyNames`).
- `Time.delta`, `Time.elapsed` (seconds since play started) and `Time.frame` (update count) come from the engine's
  timestep, the same `dt` that `OnUpdate` gets. By default that is the real time each frame took; under a fixed
  timestep (see [Determinism](#determinism)) it is the fixed step, so runs are reproducible.
- `Log.Trace(...)`, `Log.Info(...)`, `Log.Warn(...)`, `Log.Error(...)` and `print(...)` write to the engine log and the
  editor console.

## Math

`vec2`, `vec3`, `vec4` and `quat` are value types. Their components are read/write (`v.x`, `v.y`, `v.z`, `v.w`).

- Constructors: `vec3()`, `vec3(s)`, `vec3(x, y, z)`; `quat()` is the identity, `quat(w, x, y, z)`.
- Vector operators: `+` and `-` between two vectors of the same type; `*` by a vector (per component) or by a scalar
  on either side; `/` by a vector (per component) or by a scalar on the right (`2 / v` is an error); unary `-`, `==`
  and `tostring`. Division by zero gives the IEEE result per component (`inf`, or NaN for 0/0).
- Vector methods: `:Length()`, `:Normalized()` (the zero vector stays zero), `:Dot(v)`, `:Lerp(v, t)`. vec3 also has
  `:Cross(v)` and `:Distance(v)`.
- Quaternions: `quat.FromEuler(vec3 radians)`, `quat.AngleAxis(radians, axis)`, `q:ToEuler()`, `q * q2`,
  `q * vec3` (rotates the vector), `:Normalized()`, `:Inverse()`, `:Slerp(q2, t)`.
- `math.random(...)` is deterministic for a run (see below). `math.randomseed(n)` reseeds it.

## Determinism

A run is reproducible under a fixed timestep: `--fixed-timestep <seconds>` for the editor or an exported game, or
`play.start { paused: true }` and `play.step { frames, dt }` for agents. Scripts then see time only through `Time`,
`math.random` starts from the same seed, and physics, animation and sound follow simulated time. Without one (the
default in play mode and exported games), `OnUpdate`'s `dt` and `Time` follow the real time each frame took, so two
runs differ; physics still steps at a fixed 1/60 s, but how many steps an update runs depends on the frame time.

Some things in Lua itself need care even under a fixed timestep:

- `pairs` and `next` visit a table in hash order. That order is the same in every run for integer keys, and for
  string keys as long as the strings are the same in every run (the hash seed is fixed). Tables, functions and
  userdata (entities, components, script instances) hash by their memory address, which differs from run to run. A
  table that holds any such key, even mixed with string or integer keys, has no reproducible order for any of its
  keys. Never let gameplay depend on the order of `pairs` over such a table: keep sets of entities or script
  instances in arrays (`table.insert`, remove by index) and walk them with `ipairs`, or key them by a string or
  integer of your own.
- `entity.id` of an entity created while playing (by `Scene.CreateEntity`, `Scene.Instantiate`, or an editor command
  in play mode) comes from a sequence that starts from the same seed in every game session and carries on through
  `Scene.Load` and `Scene.Reload`. The same inputs give the same ids in the same order, in the editor's play mode and
  in the exported game alike, so keying a table by them, sorting by them or saving them in `Game` replays exactly,
  and a session never hands out an id twice. The ids depend on creation order, though: creating one more entity
  earlier renumbers every later one, so never write one into a script; only ids saved in the scene file are fixed.
- `tostring` of a plain table or a function contains its address, and so does `string.format('%p', value)`: neither
  is a usable key or sort value.

## Modules

`require("Scripts/Util")` loads `Assets/Scripts/Util.lua` from the project (the `.lua` extension is optional). It
runs once per play session and returns the module's result. Circular requires raise an error.

## Sandbox and limits

Scripts get no file, OS or process access, and cannot crash or stall the engine:
- There is no `io`, `os`, `debug`, `package`, `load`, `loadfile`, `dofile`, `collectgarbage`, `rawget`/`rawset`/
  `rawequal`/`rawlen` or `string.dump`, and no bytecode.
- `getmetatable` returns `nil` for anything but tables: the metatables of strings and engine types are internal.
  `setmetatable` refuses `__gc` (finalizers would run script code at arbitrary points).
- Each callback (and each `script.eval`) may execute up to 200 million Lua instructions. An endless loop is stopped
  with an error instead of freezing the engine. The error cannot be caught: `pcall` and coroutines inside the script
  do not stop it.
- `string.find`, `match`, `gmatch` and `gsub` run in C, outside the instruction count. A call whose worst case
  `(#subject + 1) ^ (quantifiers + 1)` exceeds 10^9 is refused with an error. Plain finds (`string.find(s, p, 1, true)`)
  are always allowed.
- A scene's scripts may allocate up to 256 MB in total. One callback may create at most 10,000 entities, counting
  prefab instances by their entity count.
- Errors anywhere, including the engine's own calls into Lua, are reported to the log and `script.errors`. They
  never crash the engine.

## Prefabs

A prefab (`.pxprefab`) is a saved entity subtree. Create one from an entity with the agent command `prefab.create`,
place it with `prefab.instantiate`, or spawn it from scripts with `Scene.Instantiate`. Each instance gets new ids,
and its root carries a `PrefabInstance` component that records the prefab.

## Agent commands

| Command | Description |
|---|---|
| `script.create { path [, source] }` | Writes a script, from a template with the three callbacks if no source is given. |
| `script.eval { code }` | Runs Lua in the playing scene and returns the results as text. Useful for checking game state. |
| `script.errors { [clear] }` | Errors (with tracebacks) recorded in this play session. |
| `script.api` | Every name scripts can use (generated from the bindings). |
| `prefab.create { id, path }`, `prefab.instantiate { prefab [, parent, position] }` | Save and place prefabs. |
| `play.start { [paused] }`, `play.step { frames, dt }`, `play.pause`, `play.resume`, `play.stop` | Run the game. Start paused and step for deterministic, agent-driven runs. |
