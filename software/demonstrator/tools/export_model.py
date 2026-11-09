#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Export the actual Keel studio model for the offline demonstrator.

Run with ordinary Python (Pillow required):
    python software/demonstrator/tools/export_model.py --blender /path/to/blender

Only a background copy of the scene is modified. The studio .blend is never saved.
The GLB has self-contained PBR textures, functional picking groups and wheel pivots.
It uses EXT_meshopt_compression; configure GLTFLoader with the local MeshoptDecoder.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
DEFAULT_SOURCE = ROOT / "chassis/renders/Keel_R3_studio.blend"
DEFAULT_OUTPUT = ROOT / "software/demonstrator/web/public/assets"


def composite_board(output: Path) -> None:
    """Turn the studio's shader mixes into conventional glTF textures."""
    from PIL import Image

    source = ROOT / "chassis/renders/assets"
    size = (1448, 2048)
    mask = Image.open(source / "keel_mask_apertures.png").getchannel("A").resize(size, Image.Resampling.LANCZOS)
    silk = Image.open(source / "keel_silkscreen_white.png").getchannel("A").resize(size, Image.Resampling.LANCZOS)
    base = Image.new("RGBA", size, (23, 28, 32, 255))
    pads = Image.new("RGBA", size, (185, 155, 88, 255))
    pads.putalpha(mask)
    ink = Image.new("RGBA", size, (221, 226, 222, 255))
    ink.putalpha(silk)
    Image.alpha_composite(Image.alpha_composite(base, pads), ink).convert("RGB").save(output / "board-basecolor.png", optimize=True)
    # glTF ORM packing: R occlusion (unused), G roughness, B metallic.
    roughness = mask.point(lambda a: round(84 * (1-a/255) + 69 * a/255))
    Image.merge("RGB", (Image.new("L", size, 255), roughness, mask)).save(output / "board-metallic-roughness.png", optimize=True)


def in_blender(source: Path, output: Path) -> None:
    import bpy
    from mathutils import Matrix, Vector

    bpy.ops.wm.open_mainfile(filepath=str(source))
    for key in list(bpy.context.scene.keys()):
        del bpy.context.scene[key]
    keep = {"Keel — assembly geometry", "HC-SR04 and optical details"}
    objects = [o for c in bpy.data.collections if c.name in keep for o in c.objects]
    if not objects:
        raise RuntimeError("The source scene has no robot assembly collections")
    for obj in list(bpy.data.objects):
        if obj not in objects:
            bpy.data.objects.remove(obj, do_unlink=True)

    # Thin sensor grille curves can use four-sided wire sections at browser scale.
    for obj in objects:
        if obj.type == "CURVE":
            obj.data.bevel_resolution = 0
            obj.data.resolution_u = 1
    bpy.ops.object.select_all(action="DESELECT")
    for obj in objects:
        obj.hide_set(False)
        obj.hide_render = False
        obj.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]
    bpy.ops.object.convert(target="MESH")
    objects = list(bpy.context.selected_objects)

    # Export only glTF-supported surface nodes. The procedural studio microtexture
    # and shader bevel are intentionally omitted; the real mesh remains unchanged.
    replacements = {}
    for old in {m for o in objects for m in o.data.materials if m}:
        new = bpy.data.materials.new("Web / " + old.name)
        new.use_nodes = True
        shader = new.node_tree.nodes.get("Principled BSDF")
        original = old.node_tree.nodes.get("Principled BSDF") if old.use_nodes else None
        for key in ("Base Color", "Roughness", "Metallic", "Coat Weight", "Coat Roughness", "Emission Color", "Emission Strength"):
            if original and key in original.inputs and key in shader.inputs:
                shader.inputs[key].default_value = original.inputs[key].default_value
        if old.name.startswith("Black PCB — actual KiCad"):
            nodes, links = new.node_tree.nodes, new.node_tree.links
            color = nodes.new("ShaderNodeTexImage")
            color.image = bpy.data.images.load(str(output / "board-basecolor.png"))
            links.new(color.outputs["Color"], shader.inputs["Base Color"])
            metal = nodes.new("ShaderNodeTexImage")
            metal.image = bpy.data.images.load(str(output / "board-metallic-roughness.png"))
            metal.image.colorspace_settings.name = "Non-Color"
            separate = nodes.new("ShaderNodeSeparateColor")
            links.new(metal.outputs["Color"], separate.inputs[0])
            links.new(separate.outputs["Green"], shader.inputs["Roughness"])
            links.new(separate.outputs["Blue"], shader.inputs["Metallic"])
        replacements[old.name] = new
    for obj in objects:
        for i, old in enumerate(obj.data.materials):
            if old:
                obj.data.materials[i] = replacements[old.name]

    export_collection = bpy.data.collections.new("Browser robot")
    bpy.context.scene.collection.children.link(export_collection)
    root_obj = bpy.data.objects.new("KeelRobot", None)
    export_collection.objects.link(root_obj)
    root_obj["source"] = "Keel_R3_studio.blend"
    root_obj["units"] = "metres"
    root_obj["front"] = "-Z in glTF"

    def gltf(v):
        return [round(v[0], 7), round(v[2], 7), round(-v[1], 7)]

    def center(items):
        corners = [o.matrix_world @ Vector(v) for o in items for v in o.bound_box]
        return Vector(tuple((min(v[i] for v in corners)+max(v[i] for v in corners))/2 for i in range(3)))

    groups = {}
    def group(name, kind, pivot=None, index=None):
        if name not in groups:
            obj = bpy.data.objects.new(name, None)
            export_collection.objects.link(obj)
            obj.parent = root_obj
            if pivot is not None:
                obj.location = pivot
            obj["kind"] = kind
            if index is not None:
                obj["index"] = index
            groups[name] = {"object": obj, "items": [], "kind": kind, "index": index}
        return groups[name]

    emitter_names = []
    def emitter(obj, name, kind, index):
        obj.name = name
        mat = bpy.data.materials.new(name + " material")
        mat.use_nodes = True
        bs = mat.node_tree.nodes.get("Principled BSDF")
        bs.inputs["Base Color"].default_value = (.42, .44, .40, 1)
        bs.inputs["Roughness"].default_value = .4
        bs.inputs["Emission Color"].default_value = (0, 0, 0, 1)
        bs.inputs["Emission Strength"].default_value = 0
        obj.data.materials.clear()
        obj.data.materials.append(mat)
        obj["kind"] = kind
        obj["index"] = index
        obj["emitter"] = True
        emitter_names.append(name)

    for obj in objects:
        name = obj.get("source_3mf_name", obj.name)
        rgb = re.match(r"Keel R3 board: RGB([1-6]) \((\d)\)", name)
        button_led = re.match(r"Keel R3 board: D([1-4]) \((\d)\)", name)
        switch = re.fullmatch(r"Keel R3 board: SW([1-4])", name)
        if name.startswith("Wheel, left:"):
            g = group("WheelLeft", "motor", (-.061,.028002,.0235), 0)
        elif name.startswith("Wheel, right:"):
            g = group("WheelRight", "motor", (.061,.028002,.0235), 1)
        elif rgb:
            index = int(rgb[1])-1
            g = group("RGB"+rgb[1], "rgb", index=index)
            if rgb[2] == "2":
                emitter(obj, "RGB"+rgb[1]+"_Emitter", "rgb", index)
        elif button_led:
            index = int(button_led[1])-1
            g = group("ButtonLED"+button_led[1], "buttonLed", index=index)
            if button_led[2] == "4":
                emitter(obj,"ButtonLED"+button_led[1]+"_Emitter", "buttonLed", index)
        elif switch:
            g = group("SW"+switch[1], "button", index=int(switch[1])-1)
        elif name.startswith("Keel R3 board: pcb"):
            g = group("Baseboard", "board")
        elif name.startswith("Keel R3 board:"):
            g = group("BoardElectronics", "board")
        elif name.startswith(("Camera Module", "Camera —", "Camera cable")):
            g = group("CameraModule", "camera")
        elif name.startswith(("HC-SR04", "Crystal")):
            g = group("RangeSensor", "range")
        elif name.startswith("Raspberry Pi") or name.startswith("2x20"):
            g = group("RaspberryPi", "computer")
        elif name.startswith("Power bank"):
            g = group("PowerBank", "power")
        elif name.startswith("Gearmotor"):
            g = group("DriveMotors", "motor")
        elif re.match(r"P\d\d", name):
            g = group("PrintedChassis", "chassis")
        else:
            g = group("Fasteners", "hardware")
        g["items"].append(obj)

    metadata = {"units":"metres", "coordinates":"X right, Y up, -Z front",
                "source":"chassis/renders/Keel_R3_studio.blend", "groups":{},
                "emitters":emitter_names, "buttonLabels":["A","B","X","Y"]}
    for name, g in groups.items():
        items, parent = g["items"], g["object"]
        at = center(items)
        metadata["groups"][name] = {"kind":g["kind"], "index":g["index"], "center":gltf(at),
                                     "pivot":gltf(parent.location), "sourceParts":[o.get("source_3mf_name",o.name) for o in items]}
        parent["sourceParts"] = metadata["groups"][name]["sourceParts"]
        # Join static geometry within each functional group; emitters remain separate.
        static = [o for o in items if not o.get("emitter",False)]
        pieces = [o for o in items if o.get("emitter",False)]
        if static:
            bpy.ops.object.select_all(action="DESELECT")
            for o in static:
                o.select_set(True)
            bpy.context.view_layer.objects.active = static[0]
            if len(static)>1:
                bpy.ops.object.join()
            combined = bpy.context.view_layer.objects.active
            combined.name = name+"_Body"
            combined["kind"] = g["kind"]
            if g["index"] is not None:
                combined["index"] = g["index"]
            pieces.append(combined)
        for obj in pieces:
            world = obj.matrix_world.copy()
            obj.parent = parent
            obj.matrix_parent_inverse = Matrix.Identity(4)
            obj.matrix_world = world
            for c in list(obj.users_collection):
                c.objects.unlink(obj)
            export_collection.objects.link(obj)

    bpy.context.view_layer.update()
    bpy.ops.object.select_all(action="DESELECT")
    for obj in export_collection.objects:
        obj.select_set(True)
    bpy.ops.export_scene.gltf(filepath=str(output/"robot.glb"), export_format="GLB",
        use_selection=True, export_yup=True, export_apply=True, export_extras=True,
        export_cameras=False, export_lights=False, export_animations=False,
        export_normals=True, export_texcoords=True, export_materials="EXPORT",
        export_image_format="AUTO", export_draco_mesh_compression_enable=False,
        export_meshopt_compression_enable=True)
    metadata["bytes"] = (output/"robot.glb").stat().st_size
    (output/"robot-metadata.json").write_text(json.dumps(metadata,indent=2)+"\n",encoding="utf-8")
    print("ROBOT_EXPORT_COMPLETE",metadata["bytes"],"bytes",len(groups),"groups")


def main():
    args_in = sys.argv[sys.argv.index("--")+1:] if "--" in sys.argv else sys.argv[1:]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source",type=Path,default=DEFAULT_SOURCE)
    parser.add_argument("--output",type=Path,default=DEFAULT_OUTPUT)
    parser.add_argument("--blender",default=shutil.which("blender") or r"C:\Program Files (x86)\Steam\steamapps\common\Blender\blender.exe")
    parser.add_argument("--in-blender",action="store_true")
    args=parser.parse_args(args_in)
    args.output.mkdir(parents=True,exist_ok=True)
    if args.in_blender:
        in_blender(args.source,args.output)
    else:
        composite_board(args.output)
        subprocess.run([args.blender,"--background","--factory-startup","--python",str(Path(__file__).resolve()),
                        "--","--in-blender","--source",str(args.source.resolve()),"--output",str(args.output.resolve())],check=True)


if __name__ == "__main__":
    main()
