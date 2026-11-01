#!/usr/bin/env python3
"""Regenerates the feature test project (Paralax-Tests/FeatureTest) by driving the editor through its agent API.

The project's scripts (Assets/Scripts/*.lua) are hand-written and kept; everything else (project file, asset registry,
material, prefab and the scene) is rebuilt here, so adding a component or scripting feature means extending this file
and the scripts, then running:

    python3 tools/featuretest/build_feature_test.py build/debug/bin/Paralax-Editor

Every entity in the scene carries the tag "Feature"; FeatureTest.lua and FeatureTestCoverageTests.cpp check that
together they hold every component type.
"""

from __future__ import annotations

import math
import shutil
import struct
import sys
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "agent"))
from paralax_agent import EditorSession  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
PROJECT = ROOT / "Paralax-Tests" / "FeatureTest"

CUBE, SPHERE, PLANE, CYLINDER, CAPSULE = "1", "2", "3", "4", "5"
DEFAULT_MATERIAL, GROUND_MATERIAL = "16", "17"
SKY_ENVIRONMENT = "18"  # Built-in HDR environment shipped in Resources/Environments.


def clean() -> None:
    """Removes generated files, keeping the hand-written scripts."""
    for path in PROJECT.glob("*.pxproj"):
        path.unlink()
    assets = PROJECT / "Assets"
    for name in ("AssetRegistry.json",):
        (assets / name).unlink(missing_ok=True)
    for folder in ("Scenes", "Prefabs", "Materials", "Audio", "Models"):
        shutil.rmtree(assets / folder, ignore_errors=True)


def write_beep(path: Path) -> None:
    """A quarter-second 660 Hz tone (16-bit mono, 22050 Hz), faded in and out so it loops without clicks."""
    rate, seconds = 22050, 0.25
    frames = int(rate * seconds)
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as file:
        file.setnchannels(1)
        file.setsampwidth(2)
        file.setframerate(rate)
        samples = []
        for i in range(frames):
            fade = min(1.0, i / 400, (frames - 1 - i) / 400)
            samples.append(int(0.3 * fade * 32767 * math.sin(2 * math.pi * 660 * i / rate)))
        file.writeframes(struct.pack(f"<{frames}h", *samples))


def write_swing_model(path: Path) -> None:
    """A glTF model with one triangle on a node "Swing" and an animation moving it up 1 m and back over 2 s (linear)."""
    import base64
    import json
    data = b"".join(struct.pack("<3f", *p) for p in ((-0.5, 0, 0), (0.5, 0, 0), (0, 0.8, 0)))      # positions, 36 bytes
    data += struct.pack("<3H", 0, 1, 2) + b"\0\0"                                                 # indices, 6 + 2 padding
    data += struct.pack("<3f", 0.0, 1.0, 2.0)                                                     # key times, 12 bytes
    data += b"".join(struct.pack("<3f", 3.0, y, -2.0) for y in (1.0, 2.0, 1.0))                   # translations, 36 bytes
    document = {
        "asset": {"version": "2.0", "generator": "Paralax build_feature_test.py"},
        "buffers": [{"byteLength": len(data), "uri": "data:application/octet-stream;base64," + base64.b64encode(data).decode()}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}, {"buffer": 0, "byteOffset": 36, "byteLength": 6},
                        {"buffer": 0, "byteOffset": 44, "byteLength": 12}, {"buffer": 0, "byteOffset": 56, "byteLength": 36}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [-0.5, 0, 0], "max": [0.5, 0.8, 0]},
                      {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"},
                      {"bufferView": 2, "componentType": 5126, "count": 3, "type": "SCALAR", "min": [0.0], "max": [2.0]},
                      {"bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC3"}],
        "meshes": [{"name": "SwingMesh", "primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
        "nodes": [{"name": "Swing", "mesh": 0, "translation": [3.0, 1.0, -2.0]}],
        "animations": [{"name": "SwingUpDown", "samplers": [{"input": 2, "output": 3}],
                        "channels": [{"sampler": 0, "target": {"node": 0, "path": "translation"}}]}],
        "scenes": [{"nodes": [0]}],
        "scene": 0,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document), encoding="utf-8")


def entity(editor: EditorSession, name: str, components: dict, parent: str | None = None) -> str:
    components = {"Tag": {"Tag": "Feature"}, **components}
    params = {"name": name, "components": components}
    if parent:
        params["parent"] = parent
    return editor.call("entity.create", params)["id"]


def build(editor: EditorSession) -> None:
    editor.call("scene.new", name="FeatureTest")
    scripts = {name: editor.call("asset.import", path=f"Scripts/{name}.lua")["handle"] for name in ("FeatureTest", "Spinner", "Util", "Ball")}
    write_beep(PROJECT / "Assets" / "Audio" / "Beep.wav")
    beep = editor.call("asset.import", path="Audio/Beep.wav")["handle"]
    gold = editor.call("material.create", path="Materials/Gold.pxmat",
                       fields={"BaseColor": [1.0, 0.77, 0.34, 1.0], "Metallic": 1.0, "Roughness": 0.3})["handle"]

    # A prefab (root with a child), made from temporary entities.
    # The crate is one compound body: a Rigidbody and box on the root, and the lid's cylinder collider on a child.
    crate = editor.call("entity.create", name="Crate", components={"Mesh": {"Mesh": CUBE, "Materials": [gold]},
                                                                 "Collider": {"Shape": "Box"}, "Rigidbody": {"Mass": 5}})["id"]
    editor.call("entity.create", name="CrateLid", parent=crate,
                components={"Mesh": {"Mesh": CYLINDER}, "Collider": {"Shape": "Cylinder"},
                            "Transform": {"Translation": [0, 0.6, 0], "Scale": [0.4, 0.1, 0.4]}})
    crate_prefab = editor.call("prefab.create", id=crate, path="Prefabs/Crate.pxprefab")["handle"]
    editor.call("entity.destroy", id=crate)

    entity(editor, "Camera", {"Camera": {"Primary": True}, "AudioListener": {"Active": True},
                              "Transform": {"Translation": [0, 3.5, 9], "Rotation": [-0.17, 0, 0, 0.985]}})
    entity(editor, "Sun", {"DirectionalLight": {"Intensity": 3.0}, "Transform": {"Rotation": [-0.40, 0.30, 0.14, 0.85]}})
    entity(editor, "Sky", {"SkyLight": {"Environment": SKY_ENVIRONMENT, "Intensity": 1.0}})
    # The built-in plane is 10 x 10; its collider is a thin box whose top is the plane's surface.
    entity(editor, "Ground", {"Mesh": {"Mesh": PLANE, "Materials": [GROUND_MATERIAL]},
                              "Collider": {"Shape": "Box", "Size": [10, 0.2, 10], "Offset": [0, -0.1, 0]}})
    entity(editor, "PhysicsBall", {"Mesh": {"Mesh": SPHERE}, "Transform": {"Translation": [-2.2, 2.0, 2.5], "Scale": [0.6, 0.6, 0.6]},
                                   "Collider": {"Shape": "Sphere", "Restitution": 0.3}, "Rigidbody": {"Mass": 2.0},
                                   "Script": {"Script": scripts["Ball"]}})
    entity(editor, "TriggerZone", {"Collider": {"Shape": "Box", "Size": [1.5, 0.3, 1.5], "IsTrigger": True},
                                   "Transform": {"Translation": [-2.2, 1.1, 2.5]}})
    spinner = entity(editor, "Spinner", {"Mesh": {"Mesh": CUBE, "Materials": [gold]}, "Script": {"Script": scripts["Spinner"]},
                                         "Transform": {"Translation": [0, 0.75, 0]}})
    entity(editor, "Lamp", {"PointLight": {"Color": [1.0, 0.55, 0.25], "Intensity": 20, "Range": 6},
                            "Transform": {"Translation": [0, 1.5, 0]}}, parent=spinner)
    entity(editor, "Spot", {"SpotLight": {"Intensity": 40, "Range": 12},
                            "Transform": {"Translation": [-3, 4, 2], "Rotation": [-0.5, 0, 0, 0.866]}})
    entity(editor, "Sphere", {"Mesh": {"Mesh": SPHERE}, "Transform": {"Translation": [-2.2, 0.5, 0.5]}})
    entity(editor, "Cylinder", {"Mesh": {"Mesh": CYLINDER, "Materials": [DEFAULT_MATERIAL]}, "Transform": {"Translation": [2.2, 0.5, 0.5]}})
    entity(editor, "Capsule", {"Mesh": {"Mesh": CAPSULE, "CastShadows": False}, "Transform": {"Translation": [0, 1, -2.5]}})
    instance = editor.call("prefab.instantiate", prefab=crate_prefab, position=[3.5, 0.5, -1.5])["id"]
    editor.call("component.set", id=instance, components={"Tag": {"Tag": "Feature"}})
    entity(editor, "Speaker", {"AudioSource": {"Clip": beep, "Volume": 0.5, "Loop": True, "PlayOnStart": True, "Spatial": True,
                                               "MinDistance": 1.0, "MaxDistance": 30.0},
                               "Transform": {"Translation": [2.2, 1.0, 2.5]}})
    # An imported model as entities (model.create_prefab): its clip plays on the Swing entity (Animation component).
    write_swing_model(PROJECT / "Assets" / "Models" / "Swing.gltf")
    swing_prefab = editor.call("model.create_prefab", model="Models/Swing.gltf", path="Prefabs/Swing.pxprefab", name="SwingModel")
    if swing_prefab["animations"] != 1 or swing_prefab["warnings"]:
        raise SystemExit(f"Swing model import: {swing_prefab}")
    swing_root = editor.call("prefab.instantiate", prefab=swing_prefab["handle"])["id"]
    editor.call("component.set", id=swing_root, components={"Tag": {"Tag": "Feature"}})
    for child in editor.call("entity.get", id=swing_root)["children"]:
        editor.call("component.set", id=child, components={"Tag": {"Tag": "Feature"}})
    entity(editor, "FeatureTest", {"Script": {"Script": scripts["FeatureTest"]}})

    editor.call("scene.save", path="Scenes/FeatureTest.pxscene")
    editor.call("project.set_start_scene", path="Scenes/FeatureTest.pxscene")

    # Check the scene before handing it to the runtime: it plays without script errors in the editor too.
    editor.call("play.start", paused=True)
    editor.call("play.step", frames=60)
    errors = editor.call("script.errors")["errors"]
    editor.call("play.stop")
    if errors:
        raise SystemExit("Feature test scripts failed in the editor:\n" + "\n".join(errors))


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    clean()
    with EditorSession.launch(sys.argv[1], ["--headless", "--new-project", str(PROJECT)]) as editor:
        build(editor)
    print(f"Feature test project written to {PROJECT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
