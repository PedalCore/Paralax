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

import shutil
import sys
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
    for folder in ("Scenes", "Prefabs", "Materials"):
        shutil.rmtree(assets / folder, ignore_errors=True)


def entity(editor: EditorSession, name: str, components: dict, parent: str | None = None) -> str:
    components = {"Tag": {"Tag": "Feature"}, **components}
    params = {"name": name, "components": components}
    if parent:
        params["parent"] = parent
    return editor.call("entity.create", params)["id"]


def build(editor: EditorSession) -> None:
    editor.call("scene.new", name="FeatureTest")
    scripts = {name: editor.call("asset.import", path=f"Scripts/{name}.lua")["handle"] for name in ("FeatureTest", "Spinner", "Util", "Ball")}
    gold = editor.call("material.create", path="Materials/Gold.pxmat",
                       fields={"BaseColor": [1.0, 0.77, 0.34, 1.0], "Metallic": 1.0, "Roughness": 0.3})["handle"]

    # A prefab (root with a child), made from temporary entities.
    crate = editor.call("entity.create", name="Crate", components={"Mesh": {"Mesh": CUBE, "Materials": [gold]}})["id"]
    editor.call("entity.create", name="CrateLid", parent=crate,
                components={"Mesh": {"Mesh": CYLINDER}, "Transform": {"Translation": [0, 0.6, 0], "Scale": [0.4, 0.1, 0.4]}})
    crate_prefab = editor.call("prefab.create", id=crate, path="Prefabs/Crate.pxprefab")["handle"]
    editor.call("entity.destroy", id=crate)

    entity(editor, "Camera", {"Camera": {"Primary": True},
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
