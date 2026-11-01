#!/usr/bin/env python3
"""End-to-end test: an agent imports a multi-object glTF scene (as Blender exports one) and gets editable entities.

The scene is generated here: a small room (floor, walls, a table with a crate on it, nested as Blender parents them,
two objects sharing one mesh) with a sun, a point light, a spot light and a camera. model.create_prefab turns it into
a prefab, prefab.instantiate places it, and the test checks the entities, views the room through the imported camera
and plays the scene. Run by CTest: model_import_test.py <Paralax-Editor> <output dir>.
"""

from __future__ import annotations

import base64
import json
import math
import shutil
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from paralax_agent import EditorSession  # noqa: E402


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def cube_buffer() -> tuple[bytes, int, int]:
    """A unit cube (24 vertices with normals, 36 indices): the buffer, its vertex count and index count."""
    faces = (((1, 0, 0), (0, 1, 0)), ((-1, 0, 0), (0, 1, 0)), ((0, 1, 0), (0, 0, -1)),
             ((0, -1, 0), (0, 0, 1)), ((0, 0, 1), (0, 1, 0)), ((0, 0, -1), (0, 1, 0)))
    positions, normals, indices = [], [], []
    for normal, up in faces:
        n = normal
        u = up
        r = (u[1] * n[2] - u[2] * n[1], u[2] * n[0] - u[0] * n[2], u[0] * n[1] - u[1] * n[0])  # up x normal
        base = len(positions)
        for su, sr in ((-1, -1), (-1, 1), (1, 1), (1, -1)):
            positions.append(tuple(0.5 * (n[i] + su * u[i] + sr * r[i]) for i in range(3)))
            normals.append(n)
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    data = b"".join(struct.pack("<3f", *p) for p in positions)
    data += b"".join(struct.pack("<3f", *n) for n in normals)
    data += b"".join(struct.pack("<H", i) for i in indices)
    return data, len(positions), len(indices)


def quat_y(degrees: float) -> list[float]:
    half = math.radians(degrees) / 2
    return [0.0, math.sin(half), 0.0, math.cos(half)]


def quat_x(degrees: float) -> list[float]:
    half = math.radians(degrees) / 2
    return [math.sin(half), 0.0, 0.0, math.cos(half)]


def write_room(path: Path) -> None:
    data, vertex_count, index_count = cube_buffer()
    stride = vertex_count * 12
    document = {
        "asset": {"version": "2.0", "generator": "Paralax model_import_test"},
        "extensionsUsed": ["KHR_lights_punctual"],
        "buffers": [{"byteLength": len(data), "uri": "data:application/octet-stream;base64," + base64.b64encode(data).decode()}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": stride},
                        {"buffer": 0, "byteOffset": stride, "byteLength": stride},
                        {"buffer": 0, "byteOffset": 2 * stride, "byteLength": index_count * 2}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": vertex_count, "type": "VEC3",
                       "min": [-0.5, -0.5, -0.5], "max": [0.5, 0.5, 0.5]},
                      {"bufferView": 1, "componentType": 5126, "count": vertex_count, "type": "VEC3"},
                      {"bufferView": 2, "componentType": 5123, "count": index_count, "type": "SCALAR"}],
        "materials": [{"name": "Plaster", "pbrMetallicRoughness": {"baseColorFactor": [0.8, 0.78, 0.72, 1], "metallicFactor": 0, "roughnessFactor": 0.9}},
                      {"name": "Wood", "pbrMetallicRoughness": {"baseColorFactor": [0.45, 0.25, 0.1, 1], "metallicFactor": 0, "roughnessFactor": 0.6}},
                      {"name": "Steel", "pbrMetallicRoughness": {"baseColorFactor": [0.6, 0.6, 0.65, 1], "metallicFactor": 1, "roughnessFactor": 0.3}}],
        "meshes": [{"name": "PlasterCube", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}]},
                   {"name": "WoodCube", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 1}]},
                   {"name": "SteelCube", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 2}]}],
        "cameras": [{"type": "perspective", "perspective": {"yfov": 0.9, "znear": 0.05, "zfar": 200}}],
        "extensions": {"KHR_lights_punctual": {"lights": [
            {"name": "Sun", "type": "directional", "color": [1, 0.95, 0.9], "intensity": 2.5 * 683},
            {"name": "Bulb", "type": "point", "color": [1, 0.8, 0.6], "intensity": 60 * 683},
            {"name": "Spot", "type": "spot", "intensity": 120 * 683, "range": 12,
             "spot": {"innerConeAngle": 0.3, "outerConeAngle": 0.6}}]}},
        "nodes": [
            {"name": "Room", "children": [1, 2, 3, 4, 5]},                                                   # 0
            {"name": "Floor", "mesh": 0, "scale": [8, 0.2, 8], "translation": [0, -0.1, 0]},                  # 1
            {"name": "BackWall", "mesh": 0, "scale": [8, 3, 0.2], "translation": [0, 1.5, -4]},              # 2
            {"name": "LeftWall", "mesh": 0, "scale": [0.2, 3, 8], "translation": [-4, 1.5, 0]},              # 3
            {"name": "Table", "mesh": 1, "scale": [2, 0.1, 1], "translation": [0, 0.8, -1], "children": [6]},  # 4
            {"name": "Crate", "mesh": 2, "scale": [0.6, 0.6, 0.6], "translation": [1.8, 0.3, 1.5],          # 5
             "rotation": quat_y(30)},
            {"name": "Box", "mesh": 2, "scale": [0.15, 3.0, 0.3], "translation": [0.2, 2.5, 0]},           # 6: on the table
            {"name": "Camera", "camera": 0, "translation": [2.5, 2.2, 5.5], "rotation": quat_y(20)},       # 7
            {"name": "Sun", "rotation": quat_x(-50), "extensions": {"KHR_lights_punctual": {"light": 0}}},  # 8
            {"name": "Bulb", "translation": [0, 2.6, 0], "extensions": {"KHR_lights_punctual": {"light": 1}}},  # 9
            {"name": "Spot", "translation": [-2, 2.8, 1], "rotation": quat_x(-80),
             "extensions": {"KHR_lights_punctual": {"light": 2}}},                                        # 10
        ],
        "scenes": [{"name": "Scene", "nodes": [0, 7, 8, 9, 10]}],
        "scene": 0,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document), encoding="utf-8")


def png_luminance(path: Path) -> float:
    """Mean luminance (0-255) of an 8-bit RGB/RGBA PNG, decoded with the standard library."""
    data = path.read_bytes()
    check(data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG")
    pos, idat, width, height, channels = 8, b"", 0, 0, 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", chunk[:10])
            check(depth == 8 and color in (2, 6), "expected an 8-bit RGB or RGBA PNG")
            channels = 3 if color == 2 else 4
        elif kind == b"IDAT":
            idat += chunk
        pos += 12 + length
    raw = zlib.decompress(idat)
    row_bytes = width * channels
    previous = bytearray(row_bytes)
    total = 0
    for y in range(height):
        start = y * (row_bytes + 1)
        kind, row = raw[start], bytearray(raw[start + 1:start + 1 + row_bytes])
        for i in range(row_bytes):
            left = row[i - channels] if i >= channels else 0
            up = previous[i]
            corner = previous[i - channels] if i >= channels else 0
            if kind == 1:
                row[i] = (row[i] + left) & 255
            elif kind == 2:
                row[i] = (row[i] + up) & 255
            elif kind == 3:
                row[i] = (row[i] + (left + up) // 2) & 255
            elif kind == 4:
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                row[i] = (row[i] + (left if pa <= pb and pa <= pc else up if pb <= pc else corner)) & 255
        for x in range(width):
            r, g, b = row[x * channels:x * channels + 3]
            total += 0.2126 * r + 0.7152 * g + 0.0722 * b
        previous = row
    return total / (width * height)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    executable, output = Path(sys.argv[1]), Path(sys.argv[2])
    shutil.rmtree(output, ignore_errors=True)
    output.mkdir(parents=True)
    project = output / "ModelImport"
    with EditorSession.launch(executable, ["--headless", "--new-project", str(project)]) as editor:
        assets = Path(editor.call("project.info")["assets"])
        write_room(assets / "Models" / "Room.gltf")

        editor.call("scene.new", name="Imported")
        created = editor.call("model.create_prefab", model="Models/Room.gltf")
        check(created["entities"] == 12, f"the room and its 11 nodes: {created}")
        check((created["meshes"], created["cameras"], created["lights"]) == (6, 1, 3), f"meshes, cameras, lights: {created}")
        check(created["warnings"] == [], f"no warnings: {created['warnings']}")
        root = editor.call("prefab.instantiate", prefab=created["handle"])["id"]

        # The hierarchy, names and components are Blender's.
        def entity(name: str) -> dict:
            ids = editor.call("scene.find", name=name)["ids"]
            check(len(ids) == 1, f"one entity named {name}: {ids}")
            return editor.call("entity.get", id=ids[0])

        check(editor.call("entity.get", id=root)["name"] == "Room", "the prefab's root is named after the file")
        table, box = entity("Table"), entity("Box")
        check("Mesh" in table["components"] and "Mesh" in box["components"], "objects keep their meshes")
        check(box["components"]["Mesh"]["Mesh"] == entity("Crate")["components"]["Mesh"]["Mesh"],
              "objects sharing a mesh in Blender share one mesh asset")
        check(abs(box["components"]["Transform"]["Translation"][1] - 2.5) < 1e-5, "transforms stay local to the parent")
        check(abs(entity("Bulb")["components"]["PointLight"]["Intensity"] - 60) < 1e-3, "light strengths come through")
        check(abs(entity("Spot")["components"]["SpotLight"]["OuterConeAngle"] - 0.6) < 1e-5, "spot cones come through")
        camera = entity("Camera")
        check(camera["components"]["Camera"]["Primary"] is False, "an imported camera never takes over by itself")

        # Look through the imported camera.
        editor.call("component.set", id=camera["id"], components={"Camera": {"Primary": True}})
        editor.call("scene.save", path="Scenes/Imported.pxscene")
        capture = editor.call("render.capture", path="Captures/Room.png", width=640, height=360, camera="scene")
        luminance = png_luminance(project / capture["path"] if not Path(capture["path"]).is_absolute() else Path(capture["path"]))
        check(20 < luminance < 235, f"the room should be lit and visible through its camera (mean luminance {luminance:.1f})")

        # It plays like any scene.
        editor.call("play.start", paused=True)
        editor.call("play.step", frames=10)
        check(editor.call("script.errors")["errors"] == [], "no script errors")
        box_world = editor.call("script.eval", code="local p = Scene.FindEntityByName('Box'):GetWorldPosition() return p.y")["result"]
        check(abs(float(box_world) - 1.05) < 1e-4, f"the box sits on the table (world y 0.8 + 2.5 * 0.1): {box_world}")
        editor.call("play.stop")
    print(f"Model import test passed (mean luminance {luminance:.1f})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
