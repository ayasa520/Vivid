#!/usr/bin/env python3
"""Generate feature-isolated fixture scenes for the golden harness.

Workshop wallpapers are opaque bundles: a regression inside them is hard to attribute. The
fixtures here are minimal scenes that each exercise one documented Wallpaper Engine feature
(an effect, a particle preset, a text layer, a property animation, a user-property binding,
camera parallax, disabled clearing, a clock script). They reference the official shared assets
(effects, shaders, particle presets) through VIVID_SCENE_ASSETS_DIR. Local inputs include
procedural textures and selected tutorial assets with provenance recorded beside them.

Fixtures are described by fixtures.json next to this file and generated into
.build/golden/fixtures/<name>/ (project.json, scene.json, models/, materials/, *.tex) by
golden.py before the `fixtures` tier runs. Every generated scene lists the documentation page
it covers so COVERAGE-MATRIX.md can be derived from the same data.

Texture containers are written in the renderer's TEX format with an embedded PNG payload.
"""

import argparse
import io
import json
import math
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw

from effect_admission import make_fixtures as make_effect_admission_fixtures
from created_script_properties import make_fixture as make_created_script_properties_fixture
from effect_live_topology import make_fixture as make_effect_live_topology_fixture
from copy_extents import make_fixture as make_copy_extents_fixture
from script_source import make_fixture as make_script_source_fixture
from camera_path_pose import make_fixture as make_camera_path_pose_fixture
from general_zoom import make_fixture as make_general_zoom_fixture
from init_property_write import make_fixture as make_init_property_write_fixture
from animation_fractional_seek import make_fixture as make_animation_fractional_seek_fixture

HERE = Path(__file__).resolve().parent
SPEC_PATH = HERE / "fixtures.json"
DEFAULT_OUTPUT = HERE.parents[3] / ".build/golden/fixtures"

CANVAS = (1920, 1080)


# --- textures -------------------------------------------------------------------------------


def procedural_image(kind, size, seed=0):
    width, height = size
    image = Image.new("RGBA", size)
    draw = ImageDraw.Draw(image)
    if kind == "checker":
        cell = max(8, width // 8)
        for y in range(0, height, cell):
            for x in range(0, width, cell):
                on = ((x // cell) + (y // cell) + seed) % 2 == 0
                color = (235, 235, 235, 255) if on else (40, 60, 90, 255)
                draw.rectangle([x, y, x + cell - 1, y + cell - 1], fill=color)
        draw.ellipse([width * 0.3, height * 0.3, width * 0.7, height * 0.7], fill=(220, 80, 60, 255))
    elif kind == "gradient":
        pixels = image.load()
        for y in range(height):
            for x in range(width):
                pixels[x, y] = (int(255 * x / max(1, width - 1)), int(255 * y / max(1, height - 1)),
                                int(127 + 127 * math.sin((x + seed) * 0.1)), 255)
    elif kind == "rings":
        pixels = image.load()
        cx, cy = width / 2, height / 2
        for y in range(height):
            for x in range(width):
                r = math.hypot(x - cx, y - cy)
                v = int(127 + 127 * math.sin(r * 0.25 + seed))
                pixels[x, y] = (v, 255 - v, (v * 3) % 256, 255)
    elif kind == "alpha-disc":
        # Transparent background, soft disc: exercises translucent blending and alpha masks.
        pixels = image.load()
        cx, cy = width / 2, height / 2
        radius = min(width, height) * 0.45
        for y in range(height):
            for x in range(width):
                r = math.hypot(x - cx, y - cy)
                alpha = max(0.0, min(1.0, (radius - r) / (radius * 0.2)))
                pixels[x, y] = (90, 200, 255, int(255 * alpha))
    elif kind == "noise":
        # Deterministic hash noise; used as flow/normal-like input for distortion effects.
        pixels = image.load()
        for y in range(height):
            for x in range(width):
                h = (x * 374761393 + y * 668265263 + seed * 982451653) & 0xFFFFFFFF
                h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
                pixels[x, y] = (h & 255, (h >> 8) & 255, 255, 255)
    else:
        raise ValueError(f"unknown texture kind {kind}")
    return image


def write_tex(path, image, clamp=True, nearest=False):
    """Write a TEXV0005 container with one PNG-encoded mip level."""
    payload = io.BytesIO()
    image.save(payload, format="PNG")
    png = payload.getvalue()
    width, height = image.size
    flags = (1 if nearest else 0) | (2 if clamp else 0)
    out = bytearray()
    out += b"TEXV0005\0"
    out += b"TEXI0001\0"
    out += struct.pack("<iI", 0, flags)  # RGBA8888, flags
    out += struct.pack("<iiii", width, height, width, height)  # tex size, picture size
    out += struct.pack("<i", 0)  # unused
    out += b"TEXB0003\0"
    out += struct.pack("<ii", 1, 13)  # image count, ImageType::PNG
    out += struct.pack("<i", 1)  # mip count
    out += struct.pack("<iiii", width, height, 0, len(png))  # mip size, lz4=0, decompressed size
    out += struct.pack("<i", len(png))
    out += png
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(out))


# --- scene assembly -------------------------------------------------------------------------


def vec(values):
    return " ".join(f"{float(v):.5f}" for v in values)


def base_scene(general_overrides=None):
    general = {
        "ambientcolor": "0.30000 0.30000 0.30000",
        "bloom": False,
        "camerafade": False,
        "cameraparallax": False,
        "camerapreview": False,
        "camerashake": False,
        "clearcolor": "0.10000 0.10000 0.12000",
        "clearenabled": True,
        "farz": 1000.0,
        "fov": 50.0,
        "nearz": 0.1,
        "orthogonalprojection": {"auto": False, "height": CANVAS[1], "width": CANVAS[0]},
        "skylightcolor": "0.30000 0.30000 0.30000",
        "zoom": 1.0,
    }
    general.update(general_overrides or {})
    return {
        "camera": {"center": vec([0, 0, 0]), "eye": vec([0, 0, 1]), "up": vec([0, 1, 0])},
        "general": general,
        "objects": [],
        "version": 8,
    }


def strip_trailing_commas(text):
    """Official effect descriptors are not always strict JSON (trailing commas)."""
    import re

    return re.sub(r",(\s*[\]}])", r"\1", text)


class FixtureBuilder:
    def __init__(self, name, out_dir, spec, assets_dir):
        self.name = name
        self.dir = out_dir / name
        self.spec = spec
        self.assets_dir = Path(assets_dir)
        self.scene = base_scene(spec.get("general"))
        if "camera" in spec:
            self.scene["camera"] = spec["camera"]
        self.next_id = 1
        self.textures = {}

    def import_effect(self, effect_file):
        """Copy an official effect into the project the way the editor does.

        Wallpaper projects ship their own copy of every effect they use: the descriptor under
        effects/<name>/effect.json plus that effect's materials/ and shaders/ trees. The renderer
        resolves those material and shader paths relative to the project, so a fixture that only
        names the shared effect descriptor would find no materials. Returns the project-relative
        descriptor path to reference from scene.json.
        """
        import shutil

        source = self.assets_dir / effect_file
        if not source.exists():
            return effect_file
        effect_root = source.parent
        for subdir in ("materials", "shaders"):
            tree = effect_root / subdir
            if tree.is_dir():
                shutil.copytree(tree, self.dir / subdir, dirs_exist_ok=True)
        # Effect source trees keep auxiliary textures as PNG plus a compile descriptor; the
        # editor compiles them into .tex containers when a project is saved. Do the same.
        for png in (self.dir / "materials").rglob("*.png"):
            tex = png.with_suffix(".tex")
            if tex.exists():
                continue
            options = {}
            descriptor = png.with_suffix(".tex-json")
            if descriptor.exists():
                try:
                    options = json.loads(strip_trailing_commas(descriptor.read_text()))
                except json.JSONDecodeError:
                    options = {}
            write_tex(tex, Image.open(png).convert("RGBA"),
                      clamp=bool(options.get("clampuvs", False)),
                      nearest=bool(options.get("nointerpolation", False)))
        target = self.dir / effect_file
        target.parent.mkdir(parents=True, exist_ok=True)
        descriptor = json.loads(strip_trailing_commas(source.read_text()))
        target.write_text(json.dumps(descriptor, indent=1, ensure_ascii=False))
        return effect_file

    def allocate_id(self):
        value = self.next_id
        self.next_id += 1
        return value

    def effect_entries(self, entries):
        effects = []
        for effect in entries:
            obj = {"file": self.import_effect(effect["file"]),
                   "id": self.allocate_id(), "name": "", "visible": True}
            if "passes" in effect:
                obj["passes"] = [{"id": self.allocate_id(), **p} for p in effect["passes"]]
            effects.append(obj)
        return effects

    def texture(self, kind, size=(256, 256), seed=0, clamp=True):
        key = f"{kind}-{size[0]}x{size[1]}-{seed}"
        if key not in self.textures:
            write_tex(self.dir / "materials" / f"{key}.tex", procedural_image(kind, size, seed), clamp=clamp)
            self.textures[key] = True
        return key

    def image_model(self, texture_key, size, shader="genericimage2", blending="translucent",
                    extra_material=None):
        material = {
            "passes": [{
                "blending": blending,
                "cullmode": "nocull",
                "depthtest": "disabled",
                "depthwrite": "disabled",
                "shader": shader,
                "textures": [texture_key],
                **(extra_material or {}),
            }]
        }
        material_name = f"{texture_key}-{shader}-{blending}"
        if extra_material:
            material_name += "-" + "".join(sorted(str(k) for k in extra_material))[:24]
        (self.dir / "materials").mkdir(parents=True, exist_ok=True)
        (self.dir / "materials" / f"{material_name}.json").write_text(json.dumps(material, indent=1))
        model = {
            "autosize": False,
            "fullscreen": False,
            "material": f"materials/{material_name}.json",
            "width": size[0],
            "height": size[1],
        }
        (self.dir / "models").mkdir(parents=True, exist_ok=True)
        model_name = f"{material_name}-{size[0]}x{size[1]}"
        (self.dir / "models" / f"{model_name}.json").write_text(json.dumps(model, indent=1))
        return f"models/{model_name}.json"

    def add_image_layer(self, layer):
        size = layer.get("size", [512, 512])
        model = layer.get("model")
        if model is None:
            texture_key = layer.get("texture_key")
            if texture_key is None:
                texture_key = self.texture(layer.get("texture", "checker"), tuple(layer.get("texture_size", (256, 256))),
                                           layer.get("seed", 0))
            model = self.image_model(texture_key, size, layer.get("shader", "genericimage2"),
                                     layer.get("blending", "translucent"), layer.get("material"))
        obj = {
            "id": self.allocate_id(),
            "name": layer.get("name", f"image{self.next_id}"),
            "image": model,
            "origin": layer.get("origin_value", vec(layer.get("origin", [960, 540, 0]))),
            "angles": vec(layer.get("angles", [0, 0, 0])),
            "scale": vec(layer.get("scale", [1, 1, 1])),
            "size": vec(size),
            "alpha": layer.get("alpha", 1.0),
            "color": vec(layer.get("color", [1, 1, 1])),
            "visible": layer.get("visible", True),
            "parallaxDepth": vec(layer.get("parallax_depth", [0, 0])),
        }
        if "instance" in layer:
            obj["instance"] = layer["instance"]
        for key in ("origin", "alpha", "color", "angles", "scale", "visible"):
            if f"{key}_binding" in layer:
                obj[key] = layer[f"{key}_binding"]
        effects = self.effect_entries(layer.get("effects", []))
        if effects:
            obj["effects"] = effects
        self.scene["objects"].append(obj)
        return obj

    def add_text_layer(self, layer):
        obj = {
            "id": self.allocate_id(),
            "name": layer.get("name", "text"),
            "origin": vec(layer.get("origin", [960, 540, 0])),
            "angles": vec([0, 0, 0]),
            "scale": vec([1, 1, 1]),
            "size": vec(layer.get("size", [1200, 300])),
            "alpha": 1.0,
            "anchor": "none",
            "brightness": 1.0,
            "color": vec(layer.get("color", [1, 1, 1])),
            "colorBlendMode": 0,
            "copybackground": False,
            "opaquebackground": layer.get("opaque_background", False),
            "backgroundcolor": vec(layer.get("background_color", [0, 0, 0])),
            "backgroundbrightness": 1.0,
            "horizontalalign": layer.get("halign", "center"),
            "verticalalign": layer.get("valign", "center"),
            "ledsource": False,
            "locktransforms": False,
            "padding": layer.get("padding", 10),
            "parallaxDepth": vec([0, 0]),
            "perspective": False,
            "pointsize": layer.get("pointsize", 24.0),
            "solid": True,
            "visible": True,
            "text": layer["text"],
        }
        if "font" in layer:
            obj["font"] = layer["font"]
        effects = self.effect_entries(layer.get("effects", []))
        if effects:
            obj["effects"] = effects
        self.scene["objects"].append(obj)
        return obj

    def add_particle_layer(self, layer):
        obj = {
            "id": self.allocate_id(),
            "name": layer.get("name", "particles"),
            "particle": layer["particle"],
            "origin": vec(layer.get("origin", [960, 540, 0])),
            "angles": vec([0, 0, 0]),
            "scale": vec(layer.get("scale", [1, 1, 1])),
            "visible": True,
            "parallaxDepth": vec([0, 0]),
        }
        if "instanceoverride" in layer:
            obj["instanceoverride"] = layer["instanceoverride"]
        self.scene["objects"].append(obj)
        return obj

    def build(self):
        self.dir.mkdir(parents=True, exist_ok=True)
        for name in self.spec.get("asset_files", []):
            target = self.dir / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((HERE / "assets" / name).read_bytes())
        # Explicit local assets let comparison panels use independently authored material and
        # shader inputs. Keep their names and values intact: a resolved control must not be
        # synthesized by the same merge operation that the fixture is meant to observe.
        for name, content in self.spec.get("files", {}).items():
            target = self.dir / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content if isinstance(content, str)
                              else json.dumps(content, indent=1, ensure_ascii=False))
        # Replacement inputs must exist without a layer holding them alive. Declaring those
        # assets separately lets lifetime fixtures distinguish an unreferenced old selection
        # from a texture retained by another visible or hidden material.
        for key, texture in self.spec.get("texture_assets", {}).items():
            write_tex(self.dir / "materials" / f"{key}.tex",
                      procedural_image(texture["kind"], tuple(texture.get("size", (256, 256))),
                                       texture.get("seed", 0)),
                      clamp=texture.get("clamp", True), nearest=texture.get("nearest", False))
        for layer in self.spec.get("layers", []):
            kind = layer.get("kind", "image")
            if kind == "image":
                self.add_image_layer(layer)
            elif kind == "text":
                self.add_text_layer(layer)
            elif kind == "particle":
                self.add_particle_layer(layer)
            else:
                raise ValueError(f"{self.name}: unknown layer kind {kind}")
        self.scene["objects"].extend(self.spec.get("objects", []))
        (self.dir / "scene.json").write_text(json.dumps(self.scene, indent=1, ensure_ascii=False))
        (self.dir / "project.json").write_text(json.dumps({
            "file": "scene.json",
            "title": f"golden fixture {self.name}",
            "type": "scene",
            "version": 7,
            "general": {"properties": self.spec.get("properties", {})},
            "golden": {"docs": self.spec.get("docs", []), "description": self.spec.get("description", "")},
        }, indent=1, ensure_ascii=False))


def expand_spec(spec, assets_dir):
    """Materialize generated fixture families (one per official effect) into concrete entries."""
    fixtures = dict(spec.get("fixtures", {}))
    for family in spec.get("families", []):
        if family["kind"] == "animation-fractional-seek":
            fixtures["animation-fractional-seek"] = make_animation_fractional_seek_fixture()
        if family["kind"] == "init-property-write":
            fixtures["init-property-write"] = make_init_property_write_fixture(
                fixtures["effect-material-bulk"])
        if family["kind"] == "general-zoom":
            fixtures["general-zoom"] = make_general_zoom_fixture()
        if family["kind"] == "camera-path-pose":
            fixtures["camera-path-pose"] = make_camera_path_pose_fixture()
        if family["kind"] == "script-source":
            fixtures["script-source"] = make_script_source_fixture()
        if family["kind"] == "copy-extents":
            fixtures["copy-extents"] = make_copy_extents_fixture()
        if family["kind"] == "effect-live-topology":
            fixtures["effect-live-topology"] = make_effect_live_topology_fixture()
        if family["kind"] == "created-script-properties":
            fixtures["created-script-properties"] = make_created_script_properties_fixture()
        if family["kind"] == "effect-admission":
            fixtures.update(make_effect_admission_fixtures())
        if family["kind"] == "effects":
            effects_root = Path(assets_dir) / "effects"
            for effect_dir in sorted(effects_root.iterdir()):
                effect_json = effect_dir / "effect.json"
                if not effect_json.exists() or effect_dir.name in family.get("skip", []):
                    continue
                name = f"effect-{effect_dir.name}"
                if name in fixtures:
                    continue
                overrides = family.get("overrides", {}).get(effect_dir.name, {})
                fixtures[name] = {
                    "description": f"official effect '{effect_dir.name}' with default parameters on a checker layer",
                    "docs": [f"scene/effects/effect/{family.get('docs_alias', {}).get(effect_dir.name, effect_dir.name)}.md"],
                    "layers": [{
                        "kind": "image",
                        "texture": overrides.get("texture", "checker"),
                        "size": [1280, 720],
                        "effects": [{"file": f"effects/{effect_dir.name}/effect.json",
                                     **({"passes": overrides["passes"]} if "passes" in overrides else {})}],
                    }],
                    **({"general": overrides["general"]} if "general" in overrides else {}),
                }
    return fixtures


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", default=str(DEFAULT_OUTPUT))
    parser.add_argument("--assets", required=True, help="Wallpaper Engine assets directory")
    parser.add_argument("--only", help="comma-separated fixture names")
    parser.add_argument("--list", action="store_true", help="print fixture names and docs, generate nothing")
    args = parser.parse_args()

    spec = json.loads(SPEC_PATH.read_text())
    fixtures = expand_spec(spec, args.assets)
    if args.only:
        wanted = set(args.only.split(","))
        fixtures = {k: v for k, v in fixtures.items() if k in wanted}
    if args.list:
        for name, entry in sorted(fixtures.items()):
            print(f"{name}\t{';'.join(entry.get('docs', []))}")
        return
    out_dir = Path(args.output)
    for name, entry in sorted(fixtures.items()):
        FixtureBuilder(name, out_dir, entry, args.assets).build()
    (out_dir / "index.json").write_text(json.dumps(
        {name: {"docs": entry.get("docs", []), "description": entry.get("description", ""),
                "scenario": entry.get("scenario"),
                "log_expectations": entry.get("log_expectations", {})}
         for name, entry in sorted(fixtures.items())}, indent=1, ensure_ascii=False))
    print(f"generated {len(fixtures)} fixtures under {out_dir}")


if __name__ == "__main__":
    main()
